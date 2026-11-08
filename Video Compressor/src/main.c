#include "compress.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <string.h>

typedef enum {
    ST_NEW,        /* listed (and probed, unless unreadable); Start queues it */
    ST_QUEUED,     /* waiting for the encoder */
    ST_HELD,       /* paused while queued; Start queues it again */
    ST_ACTIVE,     /* the one encode (app.job), possibly paused */
    ST_CHECKING,   /* output being verified, then the original trashed */
    ST_DONE,       /* finished; `out` is set when a smaller output was kept */
    ST_TRASHED,
} State;

typedef struct {
    char *path;
    guint64 size;
    VcProbe *probe;      /* NULL until probed, and stays NULL when unreadable */
    GtkWidget *row, *info, *status, *bar;
    State state;
    char *out;           /* the kept output, or NULL */
    guint64 out_size;
    gboolean stop;       /* Stop pressed while encoding or checking */
} Item;

typedef struct {
    GtkWindow *win;
    GtkWidget *list, *folder_label, *count_label, *speed_info;
    GtkWidget *choose_btn, *start_btn, *pause_btn, *stop_btn, *trash_btn;
    GtkWidget *speed, *subs, *trash;
    char *folder;
    GPtrArray *items;
    GCancellable *probe_cancel;
    guint probe_next;
    gboolean closing;    /* window destroyed: late async callbacks must not touch widgets */

    /* the one encode allowed at a time (SPEC); a paused one still counts */
    Item *job;
    gboolean paused;
    GSubprocess *proc;
    int pending;         /* stdout EOF + stderr EOF + process exit */
    char *part_path, *out_path, *last_err;
} App;

static App app;

static void
item_free (Item *it)
{
    g_free (it->path);
    g_free (it->out);
    vc_probe_free (it->probe);
    g_free (it);
}

static void
alert (const char *message, const char *detail, GAsyncReadyCallback done)
{
    g_autoptr (GtkAlertDialog) d = gtk_alert_dialog_new ("%s", message);
    gtk_alert_dialog_set_detail (d, detail);
    gtk_alert_dialog_choose (d, app.win, NULL, done, NULL);
}

static const char *
current_preset (void)
{
    return VC_PRESETS[gtk_drop_down_get_selected (GTK_DROP_DOWN (app.speed))];
}

/* ---- which items an action applies to: the selection, or every item when none is selected ---- */

static gboolean
any_selected (void)
{
    GList *rows = gtk_list_box_get_selected_rows (GTK_LIST_BOX (app.list));
    g_list_free (rows);
    return rows != NULL;
}

static gboolean
targeted (Item *it, gboolean selection)
{
    return !selection || gtk_list_box_row_is_selected (GTK_LIST_BOX_ROW (it->row));
}

static gboolean
can_start (Item *it)
{
    return (it->state == ST_NEW && it->probe != NULL) || it->state == ST_HELD
           || (it == app.job && app.paused);
}

static gboolean
can_pause (Item *it)
{
    return it->state == ST_QUEUED || (it == app.job && !app.paused);
}

static gboolean
can_stop (Item *it)
{
    return it->state == ST_QUEUED || it->state == ST_HELD || it->state == ST_ACTIVE
           || it->state == ST_CHECKING;
}

static gboolean
can_trash (Item *it)
{
    return it->state == ST_DONE && it->out != NULL;
}

static void
update_controls (void)
{
    gboolean probing = app.items != NULL && app.probe_next < app.items->len;
    gboolean selection = app.items != NULL && any_selected ();
    gboolean start = FALSE, pause = FALSE, stop = FALSE, trash = FALSE, busy = FALSE;
    guint left = 0;
    for (guint i = 0; app.items != NULL && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        busy |= it->state == ST_QUEUED || it->state == ST_ACTIVE || it->state == ST_CHECKING;
        left += it->state == ST_QUEUED || it->state == ST_ACTIVE;
        if (!targeted (it, selection))
            continue;
        start |= can_start (it);
        pause |= can_pause (it);
        stop |= can_stop (it);
        trash |= can_trash (it);
    }
    gtk_widget_set_sensitive (app.start_btn, start && !probing);
    gtk_widget_set_sensitive (app.pause_btn, pause);
    gtk_widget_set_sensitive (app.stop_btn, stop);
    gtk_widget_set_sensitive (app.trash_btn, trash);
    /* Settings apply to whole runs, and a new folder would free items still in use. */
    gtk_widget_set_sensitive (app.choose_btn, !busy);
    gtk_widget_set_sensitive (app.speed, !busy);
    gtk_widget_set_sensitive (app.subs, !busy);
    gtk_widget_set_sensitive (app.trash, !busy);

    g_autofree char *count = left == 0 ? g_strdup ("")
                             : g_strdup_printf ("%s%u left", app.paused ? "Paused · " : "", left);
    gtk_label_set_text (GTK_LABEL (app.count_label), count);
}

static void
set_status (Item *it, const char *text, const char *tooltip)
{
    gtk_label_set_text (GTK_LABEL (it->status), text);
    gtk_widget_set_tooltip_text (it->status, tooltip);
}

/* "20 MB · −78%" */
static char *
result_text (Item *it)
{
    g_autofree char *size = g_format_size (it->out_size);
    return g_strdup_printf ("%s · −%d%%", size,
                            (int) (100 - (it->out_size * 100.0 / it->size) + 0.5));
}

/* ---- scanning, probing and estimates ---- */

static void
refresh_info (Item *it)
{
    g_autofree char *size = g_format_size (it->size);
    if (it->probe == NULL) {
        gtk_label_set_text (GTK_LABEL (it->info), size);
        return;
    }
    VcProbe *p = it->probe;
    g_autofree char *dur = vc_format_duration (p->duration);
    const char *depth = !vc_probe_is_10bit (p) ? "" : vc_probe_is_hdr (p) ? " · 10-bit HDR"
                                                                          : " · 10-bit";
    guint64 est = vc_estimate_size (p, it->size, current_preset ());
    g_autofree char *est_size = g_format_size (est);
    g_autofree char *guess =
        est == 0 ? g_strdup ("")
        : !vc_output_is_worth_keeping (it->size, est)
            ? g_strdup ("  →  ≈ no saving")
            : g_strdup_printf ("  →  ≈ %s (−%d%%)", est_size,
                               (int) (100 - (est * 100.0 / it->size) + 0.5));
    g_autofree char *info = g_strdup_printf ("%s · %s · %s · %s%s%s",
                                             vc_class_for (p->width, p->height)->label,
                                             p->codec, dur, size, depth, guess);
    gtk_label_set_text (GTK_LABEL (it->info), info);
    gtk_widget_set_tooltip_text (it->info, est == 0 ? NULL
                                 : "≈ is a rough estimate of the compressed size at the chosen "
                                   "speed. The real size depends on the footage.");
}

/* The ⓘ next to Speed: time against medium, and size against the originals in this folder. */
static void
update_speed_guide (void)
{
    guint64 src = 0, est[16] = { 0 };
    for (guint i = 0; app.items != NULL && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->probe == NULL || it->probe->duration <= 0)
            continue;
        src += it->size;
        for (guint p = 0; VC_PRESETS[p] != NULL && p < G_N_ELEMENTS (est); p++)
            est[p] += vc_estimate_size (it->probe, it->size, VC_PRESETS[p]);
    }
    /* ASCII signs inside <tt>, so the columns line up. */
    GString *s = g_string_new ("<b>Speed guide</b> (rough estimates)\n\n"
                               "<tt>         time vs   size vs</tt>\n"
                               "<tt>speed    medium    originals</tt>");
    for (guint p = 0; VC_PRESETS[p] != NULL && p < G_N_ELEMENTS (est); p++) {
        double time, size;
        vc_preset_factors (VC_PRESETS[p], &time, &size);
        int t = (int) (fabs (time - 1) * 100 + 0.5);
        g_autofree char *time_text = t == 0 ? g_strdup ("same")
                                     : g_strdup_printf ("%c%d%%", time > 1 ? '+' : '-', t);
        g_autofree char *size_text = src == 0 ? g_strdup ("?")
                                     : g_strdup_printf ("-%d%%", (int) (100 - est[p] * 100.0
                                                                              / src + 0.5));
        g_string_append_printf (s, "\n<tt>%-8s %-9s %s</tt>", VC_PRESETS[p], time_text,
                                size_text);
    }
    g_string_append (s, src > 0 ? "\n\nSizes are for the videos in this folder. At the same\n"
                                  "quality setting, slower speeds keep more detail;\n"
                                  "they don't reliably make files smaller."
                                : "\n\nChoose a folder to see sizes for your videos.");
    gtk_widget_set_tooltip_markup (app.speed_info, s->str);
    gtk_widget_set_tooltip_markup (app.speed, s->str);
    g_string_free (s, TRUE);
}

static void
show_probe (Item *it, const char *error)
{
    refresh_info (it);
    set_status (it, it->probe == NULL ? "Unreadable" : "", error);
}

static void probe_step (void);

static void
probe_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL, *err_text = NULL;
    g_autoptr (GError) error = NULL;

    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, &err_text, &error)) {
        /* A newer folder replaced this one (or the window closed); its items are gone. */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return;
    }
    Item *it = app.items->pdata[app.probe_next++];
    if (error == NULL && g_subprocess_get_successful (proc))
        it->probe = vc_parse_probe (out, &error);
    else if (error == NULL)
        error = g_error_new (VC_ERROR, 0, "%s", err_text != NULL ? g_strstrip (err_text) : "");
    show_probe (it, error != NULL ? error->message : NULL);
    probe_step ();
}

/* One ffprobe at a time, so a folder of hundreds of files doesn't fork hundreds at once. */
static void
probe_step (void)
{
    while (app.probe_next < app.items->len) {
        Item *it = app.items->pdata[app.probe_next];
        g_auto (GStrv) argv = vc_build_ffprobe_argv (it->path);
        g_autoptr (GError) error = NULL;
        GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                               G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                               | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
        if (proc != NULL) {
            g_subprocess_communicate_utf8_async (proc, NULL, app.probe_cancel, probe_done, NULL);
            return;
        }
        show_probe (it, error->message);
        app.probe_next++;
    }
    update_speed_guide ();
    update_controls ();
}

static int
compare_items (gconstpointer a, gconstpointer b)
{
    return g_utf8_collate ((*(Item **) a)->path, (*(Item **) b)->path);
}

static GtkWidget *
make_row (Item *it)
{
    g_autofree char *base = g_path_get_basename (it->path);
    g_autofree char *display = g_filename_display_name (base);
    GtkWidget *name = gtk_label_new (display);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_widget_set_hexpand (name, TRUE);
    gtk_widget_add_css_class (name, "heading");

    it->status = gtk_label_new ("");
    it->info = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (it->info), 0);
    gtk_widget_add_css_class (it->info, "dim-label");
    it->bar = gtk_progress_bar_new ();
    gtk_widget_set_visible (it->bar, FALSE);

    GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append (GTK_BOX (top), name);
    gtk_box_append (GTK_BOX (top), it->status);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 8);
    gtk_widget_set_margin_bottom (box, 8);
    gtk_box_append (GTK_BOX (box), top);
    gtk_box_append (GTK_BOX (box), it->info);
    gtk_box_append (GTK_BOX (box), it->bar);
    return box;
}

static void
load_folder (const char *folder)
{
    g_cancellable_cancel (app.probe_cancel);
    g_clear_object (&app.probe_cancel);
    app.probe_cancel = g_cancellable_new ();
    gtk_list_box_remove_all (GTK_LIST_BOX (app.list));
    g_clear_pointer (&app.items, g_ptr_array_unref);
    app.items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
    app.probe_next = 0;
    g_free (app.folder);
    app.folder = g_strdup (folder);
    g_autofree char *display = g_filename_display_name (folder);
    gtk_label_set_text (GTK_LABEL (app.folder_label), display);

    g_autoptr (GError) error = NULL;
    g_autoptr (GDir) dir = g_dir_open (folder, 0, &error);
    if (dir == NULL) {
        alert ("Cannot open folder", error->message, NULL);
        update_speed_guide ();
        update_controls ();
        return;
    }
    /* Not recursive, by design; `compressed/` has no video extension so it never matches. */
    for (const char *name; (name = g_dir_read_name (dir)) != NULL;) {
        if (!vc_is_video_name (name))
            continue;
        g_autofree char *path = g_build_filename (folder, name, NULL);
        GStatBuf st;
        if (g_stat (path, &st) != 0 || !S_ISREG (st.st_mode))
            continue;
        Item *it = g_new0 (Item, 1);
        it->path = g_steal_pointer (&path);
        it->size = st.st_size;
        g_ptr_array_add (app.items, it);
    }
    g_ptr_array_sort (app.items, compare_items);
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        GtkWidget *box = make_row (it);
        gtk_list_box_append (GTK_LIST_BOX (app.list), box);
        it->row = gtk_widget_get_parent (box);
        refresh_info (it);
        set_status (it, "Reading…", NULL);
    }
    probe_step ();
}

static void
folder_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GFile) file = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source),
                                                                   res, NULL);
    g_autofree char *path = file != NULL ? g_file_get_path (file) : NULL;
    if (path != NULL)
        load_folder (path);
}

static void
on_choose (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (d, "Choose a folder of videos");
    gtk_file_dialog_select_folder (d, app.win, NULL, folder_chosen, NULL);
}

static void
on_speed_changed (GObject *speed, GParamSpec *pspec, gpointer user_data)
{
    (void) speed; (void) pspec; (void) user_data;
    for (guint i = 0; app.items != NULL && i < app.items->len; i++)
        refresh_info (app.items->pdata[i]);
}

/* ---- checking an output, then trashing its original ---- */

static void
keep_original (Item *it, const char *why)
{
    it->state = ST_DONE;
    g_autofree char *result = result_text (it);
    g_autofree char *text = g_strdup_printf ("%s – original kept", result);
    set_status (it, text, why);
    update_controls ();
}

static void
trash_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    Item *it = user_data;
    g_autoptr (GError) error = NULL;
    gboolean ok = g_file_trash_finish (G_FILE (source), res, &error);
    if (app.closing)
        return;
    it->stop = FALSE;   /* too late to stop: the trash call was already made */
    if (ok) {
        it->state = ST_TRASHED;
        set_status (it, "Done – original trashed", it->out);
    } else {
        it->state = ST_DONE;
        set_status (it, "Compressed – original kept (trash failed)", error->message);
    }
    update_controls ();
}

static void
verify_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    Item *it = user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    g_autoptr (GError) error = NULL;
    g_autoptr (VcProbe) probe = NULL;
    if (g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, &error)
        && g_subprocess_get_successful (proc))
        probe = vc_parse_probe (out, &error);

    if (app.closing)
        return;   /* never trash after the window is gone; the original stays */
    if (it->stop) {
        it->stop = FALSE;
        keep_original (it, "Stopped before the original was moved to Trash");
        return;
    }
    /* `out` only ever exists after ffmpeg exited 0 and the .part was renamed. */
    GStatBuf st;
    gboolean kept = g_stat (it->out, &st) == 0 && vc_output_is_worth_keeping (it->size, st.st_size);
    if (!vc_should_trash_original (TRUE, TRUE, it->probe->duration,
                                   probe != NULL ? probe->duration : 0, kept)) {
        g_autofree char *why = g_strdup_printf (
            "Output check failed: %s", error != NULL ? error->message
                                       : !kept ? "output missing or not smaller"
                                               : "duration differs");
        keep_original (it, why);
        return;
    }
    g_autoptr (GFile) file = g_file_new_for_path (it->path);
    g_file_trash_async (file, G_PRIORITY_DEFAULT, NULL, trash_done, it);
}

static void
check_then_trash (Item *it)
{
    it->state = ST_CHECKING;
    it->stop = FALSE;
    set_status (it, "Checking output…", NULL);
    g_auto (GStrv) argv = vc_build_ffprobe_argv (it->out);
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL)
        keep_original (it, error->message);
    else
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, verify_done, it);
}

/* ---- encoding ---- */

static void encode_next (void);

static void
finish_job (void)
{
    Item *it = g_steal_pointer (&app.job);
    g_autoptr (GSubprocess) proc = g_steal_pointer (&app.proc);
    g_autofree char *part = g_steal_pointer (&app.part_path);
    g_autofree char *out = g_steal_pointer (&app.out_path);
    g_autofree char *last_err = g_steal_pointer (&app.last_err);
    app.paused = FALSE;
    gtk_widget_set_visible (it->bar, FALSE);
    it->state = ST_NEW;

    GStatBuf st;
    if (it->stop) {
        it->stop = FALSE;
        g_unlink (part);
        set_status (it, "Stopped", NULL);
    } else if (!g_subprocess_get_if_exited (proc) || g_subprocess_get_exit_status (proc) != 0
               || g_stat (part, &st) != 0) {
        g_unlink (part);
        set_status (it, "Failed", last_err != NULL ? last_err : "ffmpeg failed");
    } else if (!vc_output_is_worth_keeping (it->size, st.st_size)) {
        g_unlink (part);
        it->state = ST_DONE;   /* no output: nothing to trash, and no point encoding again */
        set_status (it, "Skipped – already efficient", NULL);
    } else {
        g_autoptr (GFile) src = g_file_new_for_path (part);
        g_autoptr (GFile) dst = g_file_new_for_path (out);
        g_autoptr (GError) error = NULL;
        /* No OVERWRITE flag: an existing output is never replaced. */
        if (!g_file_move (src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
            g_unlink (part);
            set_status (it, "Failed", error->message);
        } else {
            it->state = ST_DONE;
            it->out = g_steal_pointer (&out);
            it->out_size = st.st_size;
            g_autofree char *text = result_text (it);
            set_status (it, text, it->out);
            if (gtk_check_button_get_active (GTK_CHECK_BUTTON (app.trash)))
                check_then_trash (it);   /* runs alongside the next encode */
        }
    }
    encode_next ();
}

static void
job_step_done (void)
{
    if (--app.pending == 0 && !app.closing)   /* on_close already removed the .part */
        finish_job ();
}

static void
on_progress_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDataInputStream *ds = G_DATA_INPUT_STREAM (source);
    g_autofree char *line = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);
    if (line == NULL) {
        g_object_unref (ds);
        job_step_done ();
        return;
    }
    if (!app.closing) {   /* after close, keep draining to EOF but leave the widgets alone */
        Item *it = app.job;
        double fraction;
        if (vc_parse_progress_line (line, it->probe->duration, &fraction))
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), fraction);
        else if (it->probe->duration <= 0 && g_str_has_prefix (line, "progress="))
            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (it->bar));
    }
    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_progress_line,
                                         user_data);
}

/* Keeps the last stderr line for the Failed tooltip; draining also stops ffmpeg blocking. */
static void
on_stderr_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDataInputStream *ds = G_DATA_INPUT_STREAM (source);
    g_autofree char *line = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);
    if (line == NULL) {
        g_object_unref (ds);
        job_step_done ();
        return;
    }
    if (*g_strstrip (line) != '\0') {
        g_free (app.last_err);
        app.last_err = g_utf8_make_valid (line, -1);
    }
    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_stderr_line,
                                         user_data);
}

static void
on_ffmpeg_exit (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
    job_step_done ();
}

static gboolean
start_job (Item *it, char *out, char *part)
{
    gboolean keep_subs = gtk_check_button_get_active (GTK_CHECK_BUTTON (app.subs));
    g_auto (GStrv) argv = vc_build_ffmpeg_argv (it->path, part, it->probe, current_preset (),
                                                keep_subs);
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                           | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    if (proc == NULL) {
        it->state = ST_NEW;
        set_status (it, "Failed", error->message);
        g_free (out);
        g_free (part);
        return FALSE;
    }
    app.job = it;
    app.proc = proc;
    app.out_path = out;
    app.part_path = part;
    app.pending = 3;
    it->state = ST_ACTIVE;
    it->stop = FALSE;
    set_status (it, "Encoding…", NULL);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), 0);
    gtk_widget_set_visible (it->bar, TRUE);

    GDataInputStream *so = g_data_input_stream_new (g_subprocess_get_stdout_pipe (proc));
    GDataInputStream *se = g_data_input_stream_new (g_subprocess_get_stderr_pipe (proc));
    g_data_input_stream_read_line_async (so, G_PRIORITY_DEFAULT, NULL, on_progress_line, NULL);
    g_data_input_stream_read_line_async (se, G_PRIORITY_DEFAULT, NULL, on_stderr_line, NULL);
    g_subprocess_wait_async (proc, NULL, on_ffmpeg_exit, NULL);
    return TRUE;
}

/* Starts the first queued item, in list order, unless an encode (even a paused one) is running. */
static void
encode_next (void)
{
    gboolean keep_subs = gtk_check_button_get_active (GTK_CHECK_BUTTON (app.subs));
    for (guint i = 0; app.job == NULL && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state != ST_QUEUED)
            continue;
        char *part = NULL;
        char *out = vc_output_path (it->path, keep_subs, &part);
        GStatBuf st;
        if (g_stat (out, &st) == 0) {
            g_autofree char *base = g_path_get_basename (out);
            g_autofree char *display = g_filename_display_name (base);
            g_autofree char *text = g_strdup_printf ("Skipped – compressed/%s exists", display);
            it->state = ST_DONE;
            g_free (it->out);
            it->out = out;   /* an earlier finished output, so its original can be trashed */
            it->out_size = st.st_size;
            set_status (it, text, out);
            g_free (part);
            continue;
        }
        start_job (it, out, part);   /* takes out and part */
    }
    update_controls ();
}

/* ---- the action buttons ---- */

static void
confirm (const char *message, const char *detail, const char *accept,
         GAsyncReadyCallback done)
{
    g_autoptr (GtkAlertDialog) d = gtk_alert_dialog_new ("%s", message);
    gtk_alert_dialog_set_detail (d, detail);
    const char *buttons[] = { "Cancel", accept, NULL };
    gtk_alert_dialog_set_buttons (d, buttons);
    gtk_alert_dialog_set_cancel_button (d, 0);
    gtk_alert_dialog_set_default_button (d, 0);
    gtk_alert_dialog_choose (d, app.win, NULL, done, NULL);
}

static void
do_start (void)
{
    g_autofree char *dir = g_build_filename (app.folder, "compressed", NULL);
    if (g_mkdir_with_parents (dir, 0755) != 0) {
        alert ("Cannot create the compressed folder", g_strerror (errno), NULL);
        return;
    }
    gboolean selection = any_selected ();
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!targeted (it, selection) || !can_start (it))
            continue;
        if (it == app.job) {
            g_subprocess_send_signal (app.proc, SIGCONT);
            app.paused = FALSE;
            set_status (it, "Encoding…", NULL);
        } else {
            it->state = ST_QUEUED;
            set_status (it, "Queued", NULL);
        }
    }
    encode_next ();
}

static void
start_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) == 1)
        do_start ();
}

static void
on_start (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    guint n = 0;
    gboolean keep_subs = gtk_check_button_get_active (GTK_CHECK_BUTTON (app.subs));
    gboolean selection = any_selected ();
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!targeted (it, selection) || !can_start (it) || it == app.job)
            continue;
        g_autofree char *out = vc_output_path (it->path, keep_subs, NULL);
        n += !g_file_test (out, G_FILE_TEST_EXISTS);
    }
    if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (app.trash)) || n == 0) {
        do_start ();
        return;
    }
    g_autofree char *message = g_strdup_printf (
        "Up to %u original%s will be moved to Trash after compressing. Continue?",
        n, n == 1 ? "" : "s");
    confirm (message, "An original is moved only after its compressed copy has been checked. "
                      "Files can be restored from the Trash.",
             "Compress and Move to Trash", start_confirmed);
}

static void
on_pause (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    gboolean selection = any_selected ();
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!targeted (it, selection) || !can_pause (it))
            continue;
        if (it == app.job) {
            /* SIGSTOP freezes ffmpeg in place; SIGCONT (Start) resumes it. */
            g_subprocess_send_signal (app.proc, SIGSTOP);
            app.paused = TRUE;
        } else {
            it->state = ST_HELD;
        }
        set_status (it, "Paused", NULL);
    }
    update_controls ();
}

static void
on_stop (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    gboolean selection = any_selected ();
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!targeted (it, selection) || !can_stop (it))
            continue;
        if (it->state == ST_QUEUED || it->state == ST_HELD) {
            it->state = ST_NEW;
            set_status (it, "Stopped", NULL);
        } else {
            it->stop = TRUE;   /* checking: verify_done keeps the original */
            if (it == app.job) {
                /* SIGKILL also ends a paused (stopped) ffmpeg. finish_job removes the .part
                 * once ffmpeg is gone and has closed it, then starts the next queued item. */
                g_subprocess_force_exit (app.proc);
                set_status (it, "Stopping…", NULL);
            }
        }
    }
    update_controls ();
}

static void
trash_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) != 1)
        return;
    gboolean selection = any_selected ();
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (targeted (it, selection) && can_trash (it))
            check_then_trash (it);
    }
    update_controls ();
}

static void
on_trash (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    guint n = 0;
    gboolean selection = any_selected ();
    for (guint i = 0; i < app.items->len; i++)
        n += targeted (app.items->pdata[i], selection) && can_trash (app.items->pdata[i]);
    g_autofree char *message = g_strdup_printf ("Move %u original%s to Trash?", n,
                                                n == 1 ? "" : "s");
    confirm (message, "Each compressed copy is checked first: it must play for the same length "
                      "and be smaller. Files can be restored from the Trash.",
             "Move to Trash", trash_confirmed);
}

static gboolean
unselect_all (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    (void) widget; (void) args; (void) user_data;
    gtk_list_box_unselect_all (GTK_LIST_BOX (app.list));
    return TRUE;
}

static void
on_selection_changed (GtkListBox *list, gpointer user_data)
{
    (void) list; (void) user_data;
    if (!app.closing)
        update_controls ();
}

static gboolean
on_close (GtkWindow *win, gpointer user_data)
{
    (void) win; (void) user_data;
    /* The main loop ends with the window, so clean up synchronously here. */
    app.closing = TRUE;
    if (app.proc != NULL) {
        g_subprocess_force_exit (app.proc);   /* SIGKILL works on a paused ffmpeg too */
        g_subprocess_wait (app.proc, NULL, NULL);
        g_unlink (app.part_path);
    }
    g_cancellable_cancel (app.probe_cancel);
    return FALSE;
}

/* ---- startup ---- */

static char *
check_tools (void)
{
    g_autofree char *ffmpeg = g_find_program_in_path ("ffmpeg");
    g_autofree char *ffprobe = g_find_program_in_path ("ffprobe");
    if (ffmpeg == NULL || ffprobe == NULL)
        return g_strdup ("ffmpeg and ffprobe must be installed and on PATH.\n"
                         "On Fedora: sudo dnf install ffmpeg (from RPM Fusion).");
    const char *argv[] = { ffmpeg, "-hide_banner", "-encoders", NULL };
    g_autofree char *out = NULL;
    if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                       &out, NULL, NULL, NULL)
        || strstr (out, " libx265 ") == NULL)
        return g_strdup ("This ffmpeg has no libx265 (H.265) encoder.\n"
                         "Install the full ffmpeg build from RPM Fusion.");
    return NULL;
}

static void
quit_after_alert (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
    gtk_window_destroy (app.win);
}

#define SELECTION_HINT "\nActs on the selected videos, or on all of them when none are " \
                       "selected (Ctrl+click selects several, Esc clears)."

static GtkWidget *
icon_button (const char *icon, const char *name, const char *tooltip, GCallback clicked)
{
    GtkWidget *b = gtk_button_new_from_icon_name (icon);
    gtk_widget_set_tooltip_text (b, tooltip);
    gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect (b, "clicked", clicked, NULL);
    return b;
}

static void
activate (GtkApplication *gapp, gpointer user_data)
{
    (void) user_data;
    if (app.win != NULL) {
        gtk_window_present (app.win);
        return;
    }
    app.win = GTK_WINDOW (gtk_application_window_new (gapp));
    gtk_window_set_title (app.win, "Video Compressor");
    gtk_window_set_default_size (app.win, 800, 560);
    g_signal_connect (app.win, "close-request", G_CALLBACK (on_close), NULL);

    GtkWidget *header = gtk_header_bar_new ();
    app.choose_btn = gtk_button_new_with_mnemonic ("_Choose Folder");
    g_signal_connect (app.choose_btn, "clicked", G_CALLBACK (on_choose), NULL);
    app.start_btn = icon_button ("media-playback-start-symbolic", "Start",
                                 "Start: compress the videos, or resume a paused one."
                                 SELECTION_HINT, G_CALLBACK (on_start));
    app.pause_btn = icon_button ("media-playback-pause-symbolic", "Pause",
                                 "Pause: freeze the running encode, or hold queued videos. "
                                 "Nothing else starts while an encode is paused. Press Start "
                                 "to continue." SELECTION_HINT, G_CALLBACK (on_pause));
    app.stop_btn = icon_button ("media-playback-stop-symbolic", "Stop",
                                "Stop: end the encode and delete its unfinished file, and take "
                                "queued videos off the queue. Originals are kept."
                                SELECTION_HINT, G_CALLBACK (on_stop));
    app.trash_btn = icon_button ("user-trash-symbolic", "Move originals to Trash",
                                 "Move originals to Trash: for finished videos, after checking "
                                 "the compressed copy. They can be restored from the Trash."
                                 SELECTION_HINT, G_CALLBACK (on_trash));
    gtk_widget_add_css_class (app.start_btn, "suggested-action");
    GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (controls, "linked");
    gtk_box_append (GTK_BOX (controls), app.start_btn);
    gtk_box_append (GTK_BOX (controls), app.pause_btn);
    gtk_box_append (GTK_BOX (controls), app.stop_btn);
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app.choose_btn);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app.trash_btn);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), controls);
    gtk_window_set_titlebar (app.win, header);

    app.speed = gtk_drop_down_new_from_strings (VC_PRESETS);
    for (guint i = 0; VC_PRESETS[i] != NULL; i++)
        if (strcmp (VC_PRESETS[i], VC_DEFAULT_PRESET) == 0)
            gtk_drop_down_set_selected (GTK_DROP_DOWN (app.speed), i);
    g_signal_connect (app.speed, "notify::selected", G_CALLBACK (on_speed_changed), NULL);
    GtkWidget *speed_label = gtk_label_new_with_mnemonic ("S_peed");
    gtk_label_set_mnemonic_widget (GTK_LABEL (speed_label), app.speed);
    app.speed_info = gtk_image_new_from_icon_name ("help-about-symbolic");
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.speed_info),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, "Speed guide", -1);
    app.subs = gtk_check_button_new_with_mnemonic ("Keep _subtitles (.mkv)");
    app.trash = gtk_check_button_new_with_mnemonic ("Move originals to _Trash after compressing");
    app.folder_label = gtk_label_new ("No folder chosen");
    gtk_label_set_ellipsize (GTK_LABEL (app.folder_label), PANGO_ELLIPSIZE_START);
    gtk_label_set_xalign (GTK_LABEL (app.folder_label), 0);
    gtk_widget_set_hexpand (app.folder_label, TRUE);
    gtk_widget_add_css_class (app.folder_label, "dim-label");
    app.count_label = gtk_label_new ("");

    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (bar, 12);
    gtk_widget_set_margin_end (bar, 12);
    gtk_widget_set_margin_top (bar, 8);
    gtk_widget_set_margin_bottom (bar, 8);
    gtk_box_append (GTK_BOX (bar), app.folder_label);
    gtk_box_append (GTK_BOX (bar), app.count_label);
    gtk_box_append (GTK_BOX (bar), speed_label);
    gtk_box_append (GTK_BOX (bar), app.speed);
    gtk_box_append (GTK_BOX (bar), app.speed_info);
    gtk_box_append (GTK_BOX (bar), app.subs);
    gtk_box_append (GTK_BOX (bar), app.trash);

    app.list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (app.list), GTK_SELECTION_MULTIPLE);
    /* With single-click activation, a plain click adds to a multiple selection instead of
     * replacing it; off, a click selects one row, Ctrl+click toggles, Shift+click extends. */
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (app.list), FALSE);
    g_signal_connect (app.list, "selected-rows-changed", G_CALLBACK (on_selection_changed), NULL);
    GtkWidget *placeholder = gtk_label_new ("No videos. Choose a folder to start.");
    gtk_widget_add_css_class (placeholder, "dim-label");
    gtk_widget_set_margin_top (placeholder, 48);
    gtk_list_box_set_placeholder (GTK_LIST_BOX (app.list), placeholder);
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), app.list);
    gtk_widget_set_vexpand (scroll, TRUE);

    GtkEventController *keys = gtk_shortcut_controller_new ();
    gtk_shortcut_controller_add_shortcut (
        GTK_SHORTCUT_CONTROLLER (keys),
        gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("Escape"),
                          gtk_callback_action_new (unselect_all, NULL, NULL)));
    gtk_widget_add_controller (GTK_WIDGET (app.win), keys);

    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (vbox), bar);
    gtk_box_append (GTK_BOX (vbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (vbox), scroll);
    gtk_window_set_child (app.win, vbox);

    update_speed_guide ();
    update_controls ();
    gtk_window_present (app.win);

    g_autofree char *problem = check_tools ();
    if (problem != NULL) {
        gtk_widget_set_sensitive (app.choose_btn, FALSE);
        alert ("Video Compressor can't run", problem, quit_after_alert);
    }
}

int
main (int argc, char **argv)
{
    g_autoptr (GtkApplication) gapp = gtk_application_new ("io.github.dasantha.VideoCompressor",
                                                           G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (gapp, "activate", G_CALLBACK (activate), NULL);
    return g_application_run (G_APPLICATION (gapp), argc, argv);
}
