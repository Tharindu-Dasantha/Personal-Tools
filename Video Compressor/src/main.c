#include "compress.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

typedef struct {
    char *path;
    guint64 size;
    VcProbe *probe;      /* NULL until probed, and stays NULL when unreadable */
    GtkWidget *info, *status, *bar;
} Item;

typedef struct {
    GtkWindow *win;
    GtkWidget *list, *folder_label, *count_label;
    GtkWidget *choose_btn, *start_btn, *cancel_btn, *speed, *subs, *trash;
    char *folder;
    GPtrArray *items;
    GCancellable *probe_cancel;
    guint probe_next;

    /* the running batch */
    gboolean running, cancelled, keep_subs, trash_on;
    const char *preset;
    guint current, n_todo, n_started;
    GSubprocess *proc;
    int pending;         /* stdout EOF + stderr EOF + process exit */
    char *part_path, *out_path, *last_err;
} App;

static App app;

static void encode_next (void);
static void verify_output (Item *it, const char *out);

static void
item_free (Item *it)
{
    g_free (it->path);
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

static void
update_controls (void)
{
    gboolean probing = app.items != NULL && app.probe_next < app.items->len;
    gboolean any = FALSE;
    for (guint i = 0; app.items != NULL && i < app.items->len; i++)
        any |= ((Item *) app.items->pdata[i])->probe != NULL;

    gtk_widget_set_sensitive (app.choose_btn, !app.running);
    gtk_widget_set_sensitive (app.start_btn, !app.running && !probing && any);
    gtk_widget_set_sensitive (app.cancel_btn, app.running && !app.cancelled);
    gtk_widget_set_sensitive (app.speed, !app.running);
    gtk_widget_set_sensitive (app.subs, !app.running);
    gtk_widget_set_sensitive (app.trash, !app.running);
}

static void
set_status (Item *it, const char *text, const char *tooltip)
{
    gtk_label_set_text (GTK_LABEL (it->status), text);
    gtk_widget_set_tooltip_text (it->status, tooltip);
}

/* ---- scanning and probing ---- */

static void
show_probe (Item *it, const char *error)
{
    g_autofree char *size = g_format_size (it->size);
    if (it->probe == NULL) {
        gtk_label_set_text (GTK_LABEL (it->info), size);
        set_status (it, "Unreadable", error);
        return;
    }
    VcProbe *p = it->probe;
    g_autofree char *dur = vc_format_duration (p->duration);
    const char *depth = !vc_probe_is_10bit (p) ? "" : vc_probe_is_hdr (p) ? " · 10-bit HDR"
                                                                          : " · 10-bit";
    g_autofree char *info = g_strdup_printf ("%s · %s · %s · %s%s",
                                             vc_class_for (p->width, p->height)->label,
                                             p->codec, dur, size, depth);
    gtk_label_set_text (GTK_LABEL (it->info), info);
    set_status (it, "", NULL);
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
        /* A newer folder replaced this one; its items are gone. */
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
    gtk_label_set_text (GTK_LABEL (app.count_label), "");

    g_autoptr (GError) error = NULL;
    g_autoptr (GDir) dir = g_dir_open (folder, 0, &error);
    if (dir == NULL) {
        alert ("Cannot open folder", error->message, NULL);
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
        gtk_list_box_append (GTK_LIST_BOX (app.list), make_row (it));
        g_autofree char *size = g_format_size (it->size);
        gtk_label_set_text (GTK_LABEL (it->info), size);
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

/* ---- encoding ---- */

static void
finish_batch (void)
{
    app.running = FALSE;
    gtk_label_set_text (GTK_LABEL (app.count_label), app.cancelled ? "Cancelled" : "Finished");
    update_controls ();
}

static void
next_job (void)
{
    /* A cancel during output checks lands here, after this file's encode has finished. */
    if (app.cancelled) {
        finish_batch ();
        return;
    }
    app.current++;
    encode_next ();
}

static void
keep_original (Item *it, const char *why)
{
    g_autofree char *text = g_strdup_printf ("%s – original kept",
                                             gtk_label_get_text (GTK_LABEL (it->status)));
    set_status (it, text, why);
    next_job ();
}

static void
trash_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    Item *it = app.items->pdata[app.current];
    g_autoptr (GError) error = NULL;
    if (g_file_trash_finish (G_FILE (source), res, &error))
        set_status (it, "Done – original trashed", NULL);
    else
        set_status (it, "Compressed – original kept (trash failed)", error->message);
    next_job ();
}

static void
verify_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    g_autoptr (GError) error = NULL;
    g_autoptr (VcProbe) probe = NULL;
    if (g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, &error)
        && g_subprocess_get_successful (proc))
        probe = vc_parse_probe (out, &error);

    Item *it = app.items->pdata[app.current];
    if (app.cancelled) {
        keep_original (it, "Cancelled before the original was moved to Trash");
        return;
    }
    /* Only reached after exit 0, a successful rename and passing the "not smaller" rule. */
    if (!vc_should_trash_original (app.trash_on, TRUE, it->probe->duration,
                                   probe != NULL ? probe->duration : 0, TRUE)) {
        g_autofree char *why = g_strdup_printf (
            "Output check failed: %s", error != NULL ? error->message : "duration differs");
        keep_original (it, why);
        return;
    }
    g_autoptr (GFile) file = g_file_new_for_path (it->path);
    g_file_trash_async (file, G_PRIORITY_DEFAULT, NULL, trash_done, NULL);
}

static void
verify_output (Item *it, const char *out)
{
    g_auto (GStrv) argv = vc_build_ffprobe_argv (out);
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL)
        keep_original (it, error->message);
    else
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, verify_done, NULL);
}

static void
finish_job (void)
{
    Item *it = app.items->pdata[app.current];
    g_autoptr (GSubprocess) proc = g_steal_pointer (&app.proc);
    g_autofree char *part = g_steal_pointer (&app.part_path);
    g_autofree char *out = g_steal_pointer (&app.out_path);
    g_autofree char *last_err = g_steal_pointer (&app.last_err);
    gtk_widget_set_visible (it->bar, FALSE);

    if (app.cancelled) {
        g_unlink (part);
        set_status (it, "Cancelled", NULL);
        finish_batch ();
        return;
    }
    GStatBuf st;
    if (!g_subprocess_get_if_exited (proc) || g_subprocess_get_exit_status (proc) != 0
        || g_stat (part, &st) != 0) {
        g_unlink (part);
        set_status (it, "Failed", last_err != NULL ? last_err : "ffmpeg failed");
    } else if (!vc_output_is_worth_keeping (it->size, st.st_size)) {
        g_unlink (part);
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
            g_autofree char *size = g_format_size (st.st_size);
            g_autofree char *text = g_strdup_printf (
                "%s · −%d%%", size,
                (int) (100 - (st.st_size * 100.0 / it->size) + 0.5));
            set_status (it, text, out);
            if (app.trash_on) {
                verify_output (it, out);   /* continues with next_job () */
                return;
            }
        }
    }
    next_job ();
}

static void
job_step_done (void)
{
    if (--app.pending == 0)
        finish_job ();
}

static void on_progress_line (GObject *source, GAsyncResult *res, gpointer user_data);
static void on_stderr_line (GObject *source, GAsyncResult *res, gpointer user_data);

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
    Item *it = app.items->pdata[app.current];
    double fraction;
    if (vc_parse_progress_line (line, it->probe->duration, &fraction))
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), fraction);
    else if (it->probe->duration <= 0 && g_str_has_prefix (line, "progress="))
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (it->bar));
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
    g_auto (GStrv) argv = vc_build_ffmpeg_argv (it->path, part, it->probe, app.preset,
                                                app.keep_subs);
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                           | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    if (proc == NULL) {
        set_status (it, "Failed", error->message);
        g_free (out);
        g_free (part);
        return FALSE;
    }
    app.proc = proc;
    app.out_path = out;
    app.part_path = part;
    app.pending = 3;
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

static void
encode_next (void)
{
    for (; app.current < app.items->len; app.current++) {
        Item *it = app.items->pdata[app.current];
        if (it->probe == NULL)
            continue;
        app.n_started++;
        g_autofree char *count = g_strdup_printf ("%u / %u", app.n_started, app.n_todo);
        gtk_label_set_text (GTK_LABEL (app.count_label), count);

        char *part = NULL;
        char *out = vc_output_path (it->path, app.keep_subs, &part);
        if (g_file_test (out, G_FILE_TEST_EXISTS)) {
            g_autofree char *base = g_path_get_basename (out);
            g_autofree char *display = g_filename_display_name (base);
            g_autofree char *text = g_strdup_printf ("Skipped – compressed/%s exists", display);
            set_status (it, text, NULL);
            g_free (out);
            g_free (part);
            continue;
        }
        if (start_job (it, out, part))   /* takes out and part */
            return;
    }
    finish_batch ();
}

static void
start_batch (void)
{
    g_autofree char *dir = g_build_filename (app.folder, "compressed", NULL);
    if (g_mkdir_with_parents (dir, 0755) != 0) {
        alert ("Cannot create the compressed folder", g_strerror (errno), NULL);
        return;
    }
    app.running = TRUE;
    app.cancelled = FALSE;
    app.preset = VC_PRESETS[gtk_drop_down_get_selected (GTK_DROP_DOWN (app.speed))];
    app.keep_subs = gtk_check_button_get_active (GTK_CHECK_BUTTON (app.subs));
    app.trash_on = gtk_check_button_get_active (GTK_CHECK_BUTTON (app.trash));
    app.current = app.n_started = app.n_todo = 0;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->probe != NULL) {
            app.n_todo++;
            set_status (it, "Pending", NULL);
        }
    }
    update_controls ();
    encode_next ();
}

static void
trash_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) == 1)
        start_batch ();
}

static void
on_start (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    guint n = 0;
    gboolean keep_subs = gtk_check_button_get_active (GTK_CHECK_BUTTON (app.subs));
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        g_autofree char *out = it->probe != NULL ? vc_output_path (it->path, keep_subs, NULL)
                                                 : NULL;
        n += out != NULL && !g_file_test (out, G_FILE_TEST_EXISTS);
    }
    if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (app.trash)) || n == 0) {
        start_batch ();
        return;
    }
    g_autoptr (GtkAlertDialog) d = gtk_alert_dialog_new (
        "Up to %u original%s will be moved to Trash after compressing. Continue?",
        n, n == 1 ? "" : "s");
    gtk_alert_dialog_set_detail (d, "An original is moved only after its compressed copy "
                                    "has been checked. Files can be restored from the Trash.");
    const char *buttons[] = { "Cancel", "Compress and Move to Trash", NULL };
    gtk_alert_dialog_set_buttons (d, buttons);
    gtk_alert_dialog_set_cancel_button (d, 0);
    gtk_alert_dialog_set_default_button (d, 0);
    gtk_alert_dialog_choose (d, app.win, NULL, trash_confirmed, NULL);
}

static void
on_cancel (GtkButton *button, gpointer user_data)
{
    (void) button; (void) user_data;
    app.cancelled = TRUE;
    update_controls ();
    /* The .part is removed in finish_job, once ffmpeg is gone and has closed it. */
    if (app.proc != NULL)
        g_subprocess_force_exit (app.proc);
}

static gboolean
on_close (GtkWindow *win, gpointer user_data)
{
    (void) win; (void) user_data;
    /* The main loop ends with the window, so clean up synchronously here. */
    if (app.proc != NULL) {
        app.cancelled = TRUE;
        g_subprocess_force_exit (app.proc);
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
    gtk_window_set_default_size (app.win, 760, 560);
    g_signal_connect (app.win, "close-request", G_CALLBACK (on_close), NULL);

    GtkWidget *header = gtk_header_bar_new ();
    app.choose_btn = gtk_button_new_with_mnemonic ("_Choose Folder");
    app.start_btn = gtk_button_new_with_mnemonic ("_Start");
    app.cancel_btn = gtk_button_new_with_mnemonic ("C_ancel");
    gtk_widget_add_css_class (app.start_btn, "suggested-action");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app.choose_btn);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app.start_btn);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app.cancel_btn);
    gtk_window_set_titlebar (app.win, header);
    g_signal_connect (app.choose_btn, "clicked", G_CALLBACK (on_choose), NULL);
    g_signal_connect (app.start_btn, "clicked", G_CALLBACK (on_start), NULL);
    g_signal_connect (app.cancel_btn, "clicked", G_CALLBACK (on_cancel), NULL);

    app.speed = gtk_drop_down_new_from_strings (VC_PRESETS);
    for (guint i = 0; VC_PRESETS[i] != NULL; i++)
        if (strcmp (VC_PRESETS[i], VC_DEFAULT_PRESET) == 0)
            gtk_drop_down_set_selected (GTK_DROP_DOWN (app.speed), i);
    GtkWidget *speed_label = gtk_label_new_with_mnemonic ("S_peed");
    gtk_label_set_mnemonic_widget (GTK_LABEL (speed_label), app.speed);
    gtk_widget_set_tooltip_text (app.speed, "Slower gives smaller files and takes longer");
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
    gtk_box_append (GTK_BOX (bar), app.subs);
    gtk_box_append (GTK_BOX (bar), app.trash);

    app.list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (app.list), GTK_SELECTION_NONE);
    GtkWidget *placeholder = gtk_label_new ("No videos. Choose a folder to start.");
    gtk_widget_add_css_class (placeholder, "dim-label");
    gtk_widget_set_margin_top (placeholder, 48);
    gtk_list_box_set_placeholder (GTK_LIST_BOX (app.list), placeholder);
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), app.list);
    gtk_widget_set_vexpand (scroll, TRUE);

    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (vbox), bar);
    gtk_box_append (GTK_BOX (vbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (vbox), scroll);
    gtk_window_set_child (app.win, vbox);

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
