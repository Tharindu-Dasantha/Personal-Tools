#include "compress.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

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
    gboolean probed;
    State state;
    char *out;           /* the kept output, or NULL */
    guint64 out_size;
    gboolean stop;       /* Stop pressed while encoding or checking */
    char *note;          /* pill text while ST_NEW after a run ("Failed", "Stopped"), or NULL */
    char *tip;           /* tooltip for the size column */
    const char *extra;   /* after the meta line: "Original in Trash", "Original kept" */
    gboolean hover, in_batch;
    guint prio;          /* > 0: "Compress next" was pressed; the highest goes first */
    double fraction;     /* encode progress while active */
    GtkWidget *row, *box, *check, *meta, *chips, *size_lbl, *pill, *bar, *pct;
    GtkWidget *b_play, *b_pause, *b_stop, *b_show, *b_trash;
} Item;

typedef struct {
    GtkWindow *win;
    GtkWidget *title, *subtitle, *choose_btn, *pages, *list, *select_all;
    GtkWidget *summary, *sum_title, *sum_sub, *sum_before, *sum_after, *sum_pct;
    GtkWidget *bottom, *bottom_stack, *controls, *start_btn, *pause_btn, *stop_btn;
    GtkWidget *speed_btns[8], *speed_caption, *guide_rows, *subs, *trash, *total, *total_sub;
    GtkWidget *fps30, *max_size, *choose_empty;
    GtkWidget *run_title, *run_eta, *run_bar, *run_sub;
    GtkWidget *selbar, *sel_label, *sel_start, *sel_pause, *sel_stop, *sel_trash;
    GtkWidget *toast, *toast_label;
    GtkSizeGroup *col_quality, *col_status, *col_actions, *guide_cols[3];
    guint toast_id, preset, prio_seq;
    gboolean syncing, tools_ok, gpu_ok;
    Item *anchor;        /* last checkbox clicked, for Shift-click ranges */
    Item *ask_one;       /* scope of the action waiting on a confirm dialog */
    gboolean ask_sel;
    gint64 batch_t0, batch_end;

    char *folder;
    GPtrArray *items;
    GCancellable *probe_cancel;
    guint probe_next, probes_done;   /* ffprobe started / finished, in list order */
    gboolean closing;    /* window destroyed: late async callbacks must not touch widgets */

    /* the one encode allowed at a time (SPEC); a paused one still counts */
    Item *job;
    gboolean paused;
    gint64 job_t0, paused_at, paused_total;
    GSubprocess *proc;
    int pending;         /* stdout EOF + stderr EOF + process exit */
    char *part_path, *out_path, *last_err;
} App;

static App app;

static void update_controls (void);
static void render_row (Item *it);

static void
item_free (Item *it)
{
    g_free (it->path);
    g_free (it->out);
    g_free (it->note);
    g_free (it->tip);
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

static const char *
current_preset (void)
{
    return VC_PRESETS[app.preset];
}

/* The encode settings chosen in the bottom bar. */
static VcOptions
current_options (void)
{
    static const int sizes[] = { 0, 1080, 720 };   /* the Max size dropdown, in order */
    guint size = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.max_size));
    return (VcOptions) {
        .preset = current_preset (),
        .keep_subs = gtk_switch_get_active (GTK_SWITCH (app.subs)),
        .max_short_side = size < G_N_ELEMENTS (sizes) ? sizes[size] : 0,
        .max_fps = gtk_switch_get_active (GTK_SWITCH (app.fps30)) ? 30 : 0,
    };
}

static char *
preset_title (guint i)
{
    if (strcmp (VC_PRESETS[i], VC_PRESET_GPU) == 0)
        return g_strdup ("GPU");
    char *s = g_strdup (VC_PRESETS[i]);
    s[0] = g_ascii_toupper (s[0]);
    return s;
}

static int
percent_smaller (guint64 before, guint64 after)
{
    return (int) (100 - after * 100.0 / before + 0.5);
}

static char *
basename_display (const char *path)
{
    g_autofree char *base = g_path_get_basename (path);
    return g_filename_display_name (base);
}

/* Tooltip copy as in the design: a bold title, an optional shortcut, then one line of help. */
static void
set_tip (GtkWidget *w, const char *title, const char *key, const char *body)
{
    g_autofree char *m = g_markup_printf_escaped ("<b>%s</b>%s%s\n%s", title, key ? "   " : "",
                                                  key ? key : "", body);
    gtk_widget_set_tooltip_markup (w, m);
}

/* ---- which items an action applies to: one row, the checked rows, or all ---- */

static gboolean
checked (Item *it)
{
    return gtk_check_button_get_active (GTK_CHECK_BUTTON (it->check));
}

static gboolean
in_scope (Item *it, Item *one, gboolean sel)
{
    return one != NULL ? it == one : !sel || checked (it);
}

static gboolean
can_start (Item *it)
{
    return (it->state == ST_NEW && it->probe != NULL) || it->state == ST_HELD
           || (it == app.job && app.paused);
}

/* Start with nothing selected leaves videos predicted to save nothing (SPEC D2); selecting one,
 * or its own play button, still compresses it. */
static gboolean
skip_in_bulk (Item *it)
{
    VcOptions o = current_options ();
    return it->state == ST_NEW && it->probe != NULL && !vc_worth_encoding (it->probe, it->size, &o);
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

static gboolean
is_busy (void)
{
    for (guint i = 0; app.items != NULL && i < app.items->len; i++) {
        State s = ((Item *) app.items->pdata[i])->state;
        if (s == ST_QUEUED || s == ST_ACTIVE || s == ST_CHECKING)
            return TRUE;
    }
    return FALSE;
}

/* ---- one row ---- */

static void
set_status (Item *it, const char *note, const char *tip)
{
    g_free (it->note);
    g_free (it->tip);
    it->note = g_strdup (note);
    it->tip = g_strdup (tip);
    render_row (it);
}

static void
set_pill (Item *it, const char *text, const char *style)
{
    gtk_widget_set_visible (it->pill, text != NULL);
    if (text == NULL)
        return;
    gtk_label_set_text (GTK_LABEL (it->pill), text);
    const char *classes[] = { "pill", style, NULL };
    gtk_widget_set_css_classes (it->pill, classes);
}

static void
render_actions (Item *it)
{
    gboolean job = it == app.job;
    gboolean run = it->hover || job || it->state == ST_HELD;
    gtk_widget_set_visible (it->b_play, run && (can_start (it) || it->state == ST_QUEUED));
    gtk_widget_set_visible (it->b_pause, run && can_pause (it));
    gtk_widget_set_visible (it->b_stop, run && can_stop (it));
    gtk_widget_set_visible (it->b_show, it->hover && it->out != NULL);
    gtk_widget_set_visible (it->b_trash, it->hover && can_trash (it));

    if (job && app.paused) {
        g_autofree char *body = g_strdup_printf ("Continue from %d%%.", (int) (it->fraction * 100));
        set_tip (it->b_play, "Resume", NULL, body);
    } else if (app.job != NULL || is_busy ()) {
        set_tip (it->b_play, "Compress next", NULL, "Move this video to the front of the queue.");
    } else {
        set_tip (it->b_play, "Compress this video", NULL, "Compress only this one.");
    }
    if (job) {
        set_tip (it->b_pause, "Pause this video", NULL,
                 "Resumes from the same point. The rest of the queue waits.");
        set_tip (it->b_stop, "Stop", NULL, "Stop and remove the unfinished file. The original stays.");
    } else {
        set_tip (it->b_pause, "Hold", NULL, "The queue skips it until you press its play button.");
        set_tip (it->b_stop, it->state == ST_CHECKING ? "Stop" : "Skip", NULL,
                 it->state == ST_CHECKING ? "Keep the original instead of moving it to Trash."
                                          : "Take it out of this batch.");
    }
    if (job && !app.paused)
        gtk_widget_add_css_class (it->b_pause, "suggested-action");
    else
        gtk_widget_remove_css_class (it->b_pause, "suggested-action");
}

static void
render_row (Item *it)
{
    if (app.closing)
        return;
    g_autofree char *size = g_format_size (it->size);
    g_autofree char *size_text = NULL, *pill = NULL;
    const char *style = "neutral", *tip = it->tip;

    switch (it->state) {
    case ST_ACTIVE:
        break;
    case ST_QUEUED:
        pill = g_strdup ("● Waiting");
        style = "waiting";
        break;
    case ST_HELD:
        pill = g_strdup ("On hold");
        style = "hold";
        break;
    case ST_CHECKING:
        pill = g_strdup ("Checking copy…");
        style = "waiting";
        break;
    case ST_DONE:
    case ST_TRASHED:
        if (it->out == NULL) {
            pill = g_strdup ("Already efficient");
            tip = tip != NULL ? tip : "A compressed copy would not be at least 5% smaller, "
                                      "so none was kept.";
        } else {
            g_autofree char *out = g_format_size (it->out_size);
            size_text = g_markup_printf_escaped ("<span alpha=\"60%%\">%s →</span> <b>%s</b>",
                                                 size, out);
            pill = g_strdup_printf ("✓ −%d%%", percent_smaller (it->size, it->out_size));
            style = "done";
        }
        break;
    case ST_NEW:
        if (it->probed && it->probe == NULL) {
            pill = g_strdup ("Unreadable");
            style = "error";
        } else if (it->note != NULL) {
            size_text = g_strdup (size);
            pill = g_strdup (it->note);
            style = strcmp (it->note, "Failed") == 0 ? "error" : "neutral";
        } else {
            VcOptions o = current_options ();
            guint64 est = it->probe != NULL ? vc_estimate_size (it->probe, it->size, &o) : 0;
            if (est == 0) {
                size_text = g_strdup (size);
                break;
            }
            g_autofree char *est_size = g_format_size (est);
            size_text = g_strdup_printf ("%s  <span alpha=\"60%%\">→ ≈ %s</span>", size, est_size);
            pill = vc_output_is_worth_keeping (it->size, est)
                       ? g_strdup_printf ("−%d%%", percent_smaller (it->size, est))
                       : g_strdup ("no saving");
            style = "estimate";
            tip = tip != NULL ? tip : "≈ is a rough estimate at the chosen speed. "
                                      "The real size depends on the footage.";
        }
        break;
    }

    gboolean active = it->state == ST_ACTIVE;
    gtk_widget_set_visible (it->size_lbl, size_text != NULL);
    if (size_text != NULL)
        gtk_label_set_markup (GTK_LABEL (it->size_lbl), size_text);
    set_pill (it, pill, style);
    gtk_widget_set_tooltip_text (it->size_lbl, tip);
    gtk_widget_set_tooltip_text (it->pill, tip);
    gtk_widget_set_visible (it->bar, active);
    gtk_widget_set_visible (it->pct, active);
    if (active) {
        g_autofree char *pct = it->stop ? g_strdup ("Stopping…")
                               : it->probe->duration <= 0 ? g_strdup ("")
                               : g_strdup_printf ("%d%%", (int) (it->fraction * 100));
        gtk_label_set_text (GTK_LABEL (it->pct), pct);
        if (app.paused)
            gtk_widget_add_css_class (it->bar, "paused");
        else
            gtk_widget_remove_css_class (it->bar, "paused");
    }

    GString *m = g_string_new (NULL);
    if (it->probe != NULL) {
        g_autofree char *codec = vc_codec_label (it->probe->codec);
        g_autofree char *dur = vc_format_duration (it->probe->duration);
        g_string_printf (m, "%s · %s · %d×%d", codec, dur, it->probe->width, it->probe->height);
    } else {
        g_string_assign (m, it->probed ? "Couldn't read this file" : "Reading…");
    }
    if (it->extra != NULL)
        g_string_append_printf (m, "  ·  %s", it->extra);
    gtk_label_set_text (GTK_LABEL (it->meta), m->str);
    g_string_free (m, TRUE);
    gtk_widget_set_tooltip_text (it->meta, it->probe == NULL ? it->tip : NULL);

    if (active)
        gtk_widget_add_css_class (it->row, "active");
    else
        gtk_widget_remove_css_class (it->row, "active");
    if (checked (it))
        gtk_widget_add_css_class (it->row, "checked");
    else
        gtk_widget_remove_css_class (it->row, "checked");
    render_actions (it);
}

static GtkWidget *
chip (const char *text, const char *style)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_widget_add_css_class (l, "chip");
    if (style != NULL)
        gtk_widget_add_css_class (l, style);
    gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
    return l;
}

static void
refresh_chips (Item *it)
{
    for (GtkWidget *c; (c = gtk_widget_get_first_child (it->chips)) != NULL;)
        gtk_box_remove (GTK_BOX (it->chips), c);
    VcProbe *p = it->probe;
    if (p == NULL)
        return;
    gtk_box_append (GTK_BOX (it->chips), chip (vc_class_for (p->width, p->height)->label, NULL));
    if (vc_probe_is_10bit (p))
        gtk_box_append (GTK_BOX (it->chips),
                        chip (vc_probe_is_hdr (p) ? "10-bit HDR" : "10-bit", "hdr"));
}

/* ---- bottom bar, summary, selection bar ---- */

static void
update_speed_guide (void)
{
    guint64 src = 0, est[8] = { 0 };
    guint np = 0;
    while (VC_PRESETS[np] != NULL && np < G_N_ELEMENTS (est))
        np++;
    VcOptions o = current_options ();
    for (guint i = 0; app.items != NULL && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->probe == NULL || it->probe->duration <= 0)
            continue;
        src += it->size;
        for (guint p = 0; p < np; p++) {
            o.preset = VC_PRESETS[p];
            est[p] += vc_estimate_size (it->probe, it->size, &o);
        }
    }
    double tmax = 1;
    for (guint p = 0; p < np; p++) {
        double t, s;
        vc_preset_factors (VC_PRESETS[p], &t, &s);
        tmax = MAX (tmax, t);
    }
    for (GtkWidget *c; (c = gtk_widget_get_first_child (app.guide_rows)) != NULL;)
        gtk_box_remove (GTK_BOX (app.guide_rows), c);

    for (guint p = 0; p < np; p++) {
        if (strcmp (VC_PRESETS[p], VC_PRESET_GPU) == 0 && !app.gpu_ok)
            continue;
        double t, s;
        vc_preset_factors (VC_PRESETS[p], &t, &s);
        int smaller = src > 0 ? percent_smaller (src, est[p]) : -1;
        g_autofree char *name = preset_title (p);
        g_autofree char *size_text = smaller < 0 ? g_strdup ("—")
                                     : g_strdup_printf ("≈ %d%% smaller", smaller);
        g_autofree char *ft = g_strdup_printf ("%.2g×", t);
        int d = (int) lround ((t - 1) * 100);
        g_autofree char *delta = d == 0 ? g_strdup ("baseline")
                                 : g_strdup_printf ("%s%d%%", d > 0 ? "+" : "−", abs (d));

        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_widget_add_css_class (row, "guide-row");
        if (p == app.preset)
            gtk_widget_add_css_class (row, "current");
        GtkWidget *n = gtk_label_new (name);
        gtk_label_set_xalign (GTK_LABEL (n), 0);
        gtk_size_group_add_widget (app.guide_cols[0], n);

        GtkWidget *tl = gtk_label_new (ft), *dl = gtk_label_new (delta);
        gtk_widget_add_css_class (tl, "heading");
        gtk_widget_add_css_class (dl, "caption");
        GtkWidget *th = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append (GTK_BOX (th), tl);
        gtk_box_append (GTK_BOX (th), dl);
        GtkWidget *tb = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (tb), t / tmax);
        gtk_widget_add_css_class (tb, "time");
        GtkWidget *tv = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append (GTK_BOX (tv), th);
        gtk_box_append (GTK_BOX (tv), tb);
        gtk_size_group_add_widget (app.guide_cols[1], tv);

        GtkWidget *sl = gtk_label_new (size_text);
        gtk_label_set_xalign (GTK_LABEL (sl), 0);
        gtk_widget_add_css_class (sl, "heading");
        GtkWidget *sb = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (sb), MAX (smaller, 0) / 100.0);
        gtk_widget_add_css_class (sb, "size");
        GtkWidget *sv = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append (GTK_BOX (sv), sl);
        gtk_box_append (GTK_BOX (sv), sb);
        gtk_size_group_add_widget (app.guide_cols[2], sv);

        gtk_box_append (GTK_BOX (row), n);
        gtk_box_append (GTK_BOX (row), tv);
        gtk_box_append (GTK_BOX (row), sv);
        gtk_box_append (GTK_BOX (app.guide_rows), row);

        g_autofree char *body = smaller < 0 ? g_strdup_printf ("%s Medium time", ft)
                                : g_strdup_printf ("%s Medium time · %s", ft, size_text);
        set_tip (app.speed_btns[p], name, NULL, body);
        if (p == app.preset) {
            g_autofree char *cap = smaller < 0 ? g_strdup_printf ("%s Medium time", ft)
                                   : g_strdup_printf ("%s · %s Medium time", size_text, ft);
            gtk_label_set_text (GTK_LABEL (app.speed_caption), cap);
        }
    }
}

static void
update_progress (void)
{
    if (app.items == NULL || app.closing)
        return;
    guint total = 0, done = 0;
    guint64 all = 0, finished = 0, held = 0, saved = 0;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!it->in_batch)
            continue;
        total++;
        all += it->size;
        if (it->state == ST_HELD)
            held += it->size;
        else if (it->state != ST_QUEUED && it->state != ST_ACTIVE) {
            done++;
            finished += it->size;
        }
        if (it->out != NULL)
            saved += it->size - it->out_size;
    }
    Item *job = app.job;
    double progressed = finished + (job != NULL ? job->fraction * job->size : 0);
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (app.run_bar), all > 0 ? progressed / all : 0);
    if (app.paused)
        gtk_widget_add_css_class (app.run_bar, "paused");
    else
        gtk_widget_remove_css_class (app.run_bar, "paused");

    g_autofree char *title = NULL, *eta = NULL;
    if (job != NULL) {
        g_autofree char *name = basename_display (job->path);
        title = g_markup_printf_escaped ("<b>%s %u of %u</b> · %s",
                                         app.paused ? "Paused ·" : "Compressing",
                                         MIN (done + 1, total), total, name);
        double active = (g_get_monotonic_time () - app.job_t0 - app.paused_total) / 1e6;
        double rate = active > 0 ? job->fraction * job->size / active : 0;   /* source bytes/s */
        eta = app.paused ? g_strdup ("Press play to resume")
              : job->fraction < 0.02 || active < 5 || rate <= 0
                  ? g_strdup ("Estimating time left…")
                  : vc_format_time_left ((all - held - progressed) / rate);
    } else {
        title = g_strdup ("<b>Checking the last copy…</b>");
        eta = g_strdup ("");
    }
    gtk_label_set_markup (GTK_LABEL (app.run_title), title);
    gtk_label_set_text (GTK_LABEL (app.run_eta), eta);

    g_autofree char *saved_text = g_format_size (saved);
    const char *originals = gtk_switch_get_active (GTK_SWITCH (app.trash))
                                ? "riginals go to Trash after a check" : "riginals are untouched";
    g_autofree char *sub = saved == 0 ? g_strdup_printf ("O%s", originals)
                           : g_strdup_printf ("Saved %s so far · o%s", saved_text, originals);
    gtk_label_set_text (GTK_LABEL (app.run_sub), sub);
}

static void
update_summary (void)
{
    guint compressed = 0, efficient = 0, unreadable = 0;
    guint64 before = 0, after = 0;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->out != NULL) {
            compressed++;
            before += it->size;
            after += it->out_size;
        } else if (it->state == ST_DONE) {
            efficient++;
        } else if (it->probed && it->probe == NULL) {
            unreadable++;
        }
    }
    g_autofree char *saved = g_format_size (before - after);
    g_autofree char *title = compressed > 0 ? g_strdup_printf ("You saved %s", saved)
                                            : g_strdup ("Nothing to compress");
    gtk_label_set_text (GTK_LABEL (app.sum_title), title);

    GString *s = g_string_new (NULL);
    g_string_printf (s, "%u video%s compressed", compressed, compressed == 1 ? "" : "s");
    if (app.batch_t0 > 0 && app.batch_end > app.batch_t0) {
        guint min = (guint) ((app.batch_end - app.batch_t0) / G_USEC_PER_SEC / 60);
        if (min == 0)
            g_string_append (s, " in under a minute");
        else if (min < 60)
            g_string_append_printf (s, " in %u min", min);
        else
            g_string_append_printf (s, " in %u h %u min", min / 60, min % 60);
    }
    if (efficient > 0)
        g_string_append_printf (s, " · %u already efficient", efficient);
    if (unreadable > 0)
        g_string_append_printf (s, " · %u unreadable", unreadable);
    gtk_label_set_text (GTK_LABEL (app.sum_sub), s->str);
    g_string_free (s, TRUE);

    g_autofree char *b = g_format_size (before), *a = g_format_size (after);
    g_autofree char *pct = before > 0 ? g_strdup_printf ("−%d%%", percent_smaller (before, after))
                                      : g_strdup ("–");
    gtk_label_set_text (GTK_LABEL (app.sum_before), b);
    gtk_label_set_text (GTK_LABEL (app.sum_after), a);
    gtk_label_set_text (GTK_LABEL (app.sum_pct), pct);
}

static void
update_controls (void)
{
    if (app.closing)
        return;
    guint n = app.items != NULL ? app.items->len : 0;
    gboolean probing = app.items != NULL && app.probes_done < n;
    gboolean busy = FALSE, start = FALSE, stop = FALSE, any_done = FALSE;
    gboolean s_start = FALSE, s_pause = FALSE, s_stop = FALSE, s_trash = FALSE;
    guint nsel = 0, ready = 0, skipped = 0;
    guint64 src = 0, est = 0;
    for (guint i = 0; i < n; i++) {
        Item *it = app.items->pdata[i];
        busy |= it->state == ST_QUEUED || it->state == ST_ACTIVE || it->state == ST_CHECKING;
        gboolean skip = skip_in_bulk (it);
        skipped += skip;
        start |= can_start (it) && !skip;
        stop |= can_stop (it);
        any_done |= it->state == ST_DONE || it->state == ST_TRASHED;
        if (checked (it)) {
            nsel++;
            s_start |= can_start (it);
            s_pause |= can_pause (it);
            s_stop |= can_stop (it);
            s_trash |= can_trash (it);
        }
        if (((it->state == ST_NEW && it->probe != NULL) || it->state == ST_HELD) && !skip) {
            VcOptions o = current_options ();
            guint64 e = vc_estimate_size (it->probe, it->size, &o);
            ready++;
            src += it->size;
            est += e > 0 ? e : it->size;
        }
    }
    gboolean finished = n > 0 && !busy && !start && any_done;

    gtk_stack_set_visible_child_name (GTK_STACK (app.pages), app.folder != NULL ? "list" : "empty");
    gtk_widget_set_visible (app.bottom, app.folder != NULL);
    gtk_stack_set_visible_child_name (GTK_STACK (app.bottom_stack),
                                      busy ? "running" : finished ? "finished" : "ready");
    gtk_widget_set_visible (app.controls, !finished);
    gtk_widget_set_visible (app.summary, finished);
    gtk_widget_set_visible (app.choose_btn, app.folder != NULL && !busy && !finished);
    gtk_widget_set_sensitive (app.choose_btn, app.tools_ok && !busy);
    gtk_widget_set_sensitive (app.choose_empty, app.tools_ok);

    gtk_widget_set_sensitive (app.start_btn, app.tools_ok && !probing && start
                                             && (!busy || app.paused));
    gtk_widget_set_sensitive (app.pause_btn, app.job != NULL && !app.paused);
    gtk_widget_set_sensitive (app.stop_btn, stop);
    if (app.job != NULL && app.paused) {
        g_autofree char *name = basename_display (app.job->path);
        g_autofree char *body = g_strdup_printf ("Continue %s from %d%%.", name,
                                                 (int) (app.job->fraction * 100));
        set_tip (app.start_btn, "Resume", "Space", body);
    } else {
        set_tip (app.start_btn, "Start", "Space", "Compress every waiting video, one after another.");
    }
    for (guint p = 0; VC_PRESETS[p] != NULL && p < G_N_ELEMENTS (app.speed_btns); p++)
        gtk_widget_set_sensitive (app.speed_btns[p], !busy);
    gtk_widget_set_sensitive (app.subs, !busy);
    gtk_widget_set_sensitive (app.trash, !busy);
    gtk_widget_set_sensitive (app.fps30, !busy);
    gtk_widget_set_sensitive (app.max_size, !busy);

    g_autofree char *src_text = g_format_size (src), *est_text = g_format_size (est);
    g_autofree char *total = ready == 0 ? g_strdup ("")
                             : g_strdup_printf ("%s → ≈ %s", src_text, est_text);
    g_autofree char *preset = preset_title (app.preset);
    g_autofree char *total_sub = probing ? g_strdup ("Reading videos…")
                                 : ready == 0 && skipped > 0
                                     ? g_strdup_printf ("%u video%s likely won't get smaller · "
                                                        "select to compress anyway", skipped,
                                                        skipped == 1 ? "" : "s")
                                 : ready == 0 ? g_strdup ("Nothing waiting to compress")
                                 : g_strdup_printf ("%u video%s ready · estimated for %s", ready,
                                                    ready == 1 ? "" : "s", preset);
    gtk_label_set_text (GTK_LABEL (app.total), total);
    gtk_label_set_text (GTK_LABEL (app.total_sub), total_sub);

    gtk_widget_set_visible (app.selbar, nsel > 0);
    g_autofree char *sel = g_strdup_printf ("%u selected", nsel);
    gtk_label_set_text (GTK_LABEL (app.sel_label), sel);
    gtk_widget_set_sensitive (app.sel_start, app.tools_ok && !probing && s_start);
    gtk_widget_set_sensitive (app.sel_pause, s_pause);
    gtk_widget_set_sensitive (app.sel_stop, s_stop);
    gtk_widget_set_sensitive (app.sel_trash, s_trash);
    g_autofree char *sel_body = g_strdup_printf ("%u video%s, in list order. Others wait.", nsel,
                                                 nsel == 1 ? "" : "s");
    set_tip (app.sel_start, "Compress selected", NULL, sel_body);

    app.syncing = TRUE;
    gtk_check_button_set_inconsistent (GTK_CHECK_BUTTON (app.select_all), nsel > 0 && nsel < n);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (app.select_all), n > 0 && nsel == n);
    app.syncing = FALSE;
    gtk_widget_set_sensitive (app.select_all, n > 0);

    if (busy)
        update_progress ();
    if (finished)
        update_summary ();
}

static void
toast_hide (gpointer data)
{
    (void) data;
    app.toast_id = 0;
    gtk_revealer_set_reveal_child (GTK_REVEALER (app.toast), FALSE);
}

static void
show_toast (const char *title, const char *detail)
{
    g_autofree char *m = g_markup_printf_escaped ("<b>%s</b>   <span alpha=\"70%%\">%s</span>",
                                                  title, detail);
    gtk_label_set_markup (GTK_LABEL (app.toast_label), m);
    gtk_revealer_set_reveal_child (GTK_REVEALER (app.toast), TRUE);
    if (app.toast_id != 0)
        g_source_remove (app.toast_id);
    app.toast_id = g_timeout_add_seconds_once (5, toast_hide, NULL);
}

/* ---- scanning and probing ---- */

static void
show_probe (Item *it, const char *error)
{
    it->probed = TRUE;
    g_free (it->tip);
    it->tip = g_strdup (error);
    refresh_chips (it);
    render_row (it);
}

static void probe_step (void);

static void
probe_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL, *err_text = NULL;
    g_autoptr (GError) error = NULL;

    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, &err_text, &error)) {
        /* A newer folder replaced this one (or the window closed); its items are gone. */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return;
    }
    Item *it = user_data;
    if (error == NULL && g_subprocess_get_successful (proc))
        it->probe = vc_parse_probe (out, &error);
    else if (error == NULL)
        error = g_error_new (VC_ERROR, 0, "%s", err_text != NULL ? g_strstrip (err_text) : "");
    show_probe (it, error != NULL ? error->message : NULL);
    app.probes_done++;
    probe_step ();
}

/* Up to 4 ffprobes at a time: fast for big folders, without forking hundreds at once. */
static void
probe_step (void)
{
    while (app.probe_next < app.items->len && app.probe_next - app.probes_done < 4) {
        Item *it = app.items->pdata[app.probe_next++];
        g_auto (GStrv) argv = vc_build_ffprobe_argv (it->path);
        g_autoptr (GError) error = NULL;
        GSubprocess *proc = g_subprocess_newv ((const char *const *) argv,
                                               G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                               | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
        if (proc != NULL) {
            g_subprocess_communicate_utf8_async (proc, NULL, app.probe_cancel, probe_done, it);
        } else {
            show_probe (it, error->message);
            app.probes_done++;
        }
    }
    if (app.probes_done == app.items->len) {
        update_speed_guide ();
        update_controls ();
    }
}

static int
compare_items (gconstpointer a, gconstpointer b)
{
    return g_utf8_collate ((*(Item **) a)->path, (*(Item **) b)->path);
}

/* ---- actions ---- */

static void encode_next (void);

static void
pause_job (void)
{
    /* SIGSTOP freezes ffmpeg in place; SIGCONT (Start) resumes it. */
    g_subprocess_send_signal (app.proc, SIGSTOP);
    app.paused = TRUE;
    app.paused_at = g_get_monotonic_time ();
    render_row (app.job);
}

static void
resume_job (void)
{
    g_subprocess_send_signal (app.proc, SIGCONT);
    app.paused = FALSE;
    app.paused_total += g_get_monotonic_time () - app.paused_at;
    render_row (app.job);
}

static void
do_start (Item *one, gboolean sel)
{
    if (!is_busy ()) {   /* a new batch: the progress bar and summary count from here */
        for (guint i = 0; i < app.items->len; i++)
            ((Item *) app.items->pdata[i])->in_batch = FALSE;
        app.batch_t0 = g_get_monotonic_time ();
        app.batch_end = 0;
    }
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel))
            continue;
        if (one != NULL && it->state == ST_QUEUED) {
            it->prio = ++app.prio_seq;
            continue;
        }
        if (!can_start (it))
            continue;
        if (one == NULL && !sel && skip_in_bulk (it)) {
            set_status (it, "No saving · skipped",
                        "Likely no saving. Select it and press Start to compress it anyway.");
            continue;
        }
        if (it == app.job) {
            resume_job ();
            continue;
        }
        g_autofree char *dir = g_path_get_dirname (it->path);
        g_autofree char *out_dir = g_build_filename (dir, "compressed", NULL);
        if (g_mkdir_with_parents (out_dir, 0755) != 0) {
            alert ("Cannot create the compressed folder", g_strerror (errno), NULL);
            break;
        }
        it->state = ST_QUEUED;
        it->in_batch = TRUE;
        it->prio = one != NULL ? ++app.prio_seq : 0;
        set_status (it, NULL, NULL);
    }
    encode_next ();
}

static void
start_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) == 1)
        do_start (app.ask_one, app.ask_sel);
}

#define TRASH_CHECKS "\n\n✓ The compressed copy plays and has the same length\n" \
                     "✓ It is at least 5% smaller than the original\n" \
                     "✓ Nothing is ever deleted permanently"

static void
act_start (Item *one, gboolean sel)
{
    guint n = 0;
    gboolean keep_subs = gtk_switch_get_active (GTK_SWITCH (app.subs));
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_start (it) || it == app.job
            || (one == NULL && !sel && skip_in_bulk (it)))
            continue;
        g_autofree char *out = vc_output_path (it->path, keep_subs, NULL);
        n += !g_file_test (out, G_FILE_TEST_EXISTS);
    }
    if (!gtk_switch_get_active (GTK_SWITCH (app.trash)) || n == 0) {
        do_start (one, sel);
        return;
    }
    app.ask_one = one;
    app.ask_sel = sel;
    g_autofree char *message = g_strdup_printf ("Move up to %u original%s to Trash?", n,
                                                n == 1 ? "" : "s");
    confirm (message, "Each original is moved only after its compressed copy is ready and "
                      "checked. You can restore them from the Trash." TRASH_CHECKS,
             "Compress and Move to Trash", start_confirmed);
}

static void
act_pause (Item *one, gboolean sel)
{
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_pause (it))
            continue;
        if (it == app.job) {
            pause_job ();
        } else {
            it->state = ST_HELD;
            set_status (it, NULL, NULL);
        }
    }
    update_controls ();
}

static void
act_stop (Item *one, gboolean sel)
{
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_stop (it))
            continue;
        if (it->state == ST_QUEUED || it->state == ST_HELD) {
            it->state = ST_NEW;
            it->in_batch = FALSE;
            set_status (it, NULL, NULL);
        } else {
            it->stop = TRUE;   /* checking: verify_done keeps the original */
            if (it == app.job) {
                /* SIGKILL also ends a paused (stopped) ffmpeg. finish_job removes the .part
                 * once ffmpeg is gone and has closed it, then starts the next queued item. */
                g_subprocess_force_exit (app.proc);
                render_row (it);
            }
        }
    }
    update_controls ();
}

/* ---- checking an output, then trashing its original ---- */

static void
keep_original (Item *it, const char *why)
{
    it->state = ST_DONE;
    it->extra = "Original kept";
    set_status (it, NULL, why);
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
        it->extra = "Original in Trash";
        set_status (it, NULL, it->out);
        g_autofree char *name = basename_display (it->path);
        g_autofree char *detail = g_strdup_printf ("%s · restore it from Trash any time", name);
        show_toast ("Original moved to Trash", detail);
    } else {
        it->state = ST_DONE;
        it->extra = "Original kept (trash failed)";
        set_status (it, NULL, error->message);
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
    set_status (it, NULL, NULL);
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

static void
trash_confirmed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) != 1)
        return;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (in_scope (it, NULL, TRUE) && can_trash (it))
            check_then_trash (it);
    }
    update_controls ();
}

/* ---- encoding ---- */

/* So galleries sort the copy by when the video was taken, not by today. Best effort: a wrong
 * date never loses data, and the output check that follows does not depend on it. */
static void
copy_mtime (const char *from, const char *to)
{
    g_autoptr (GFile) src = g_file_new_for_path (from);
    g_autoptr (GFile) dst = g_file_new_for_path (to);
    g_autoptr (GFileInfo) info = g_file_query_info (src, G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                                    G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
                                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info != NULL)
        g_file_set_attributes_from_info (dst, info, G_FILE_QUERY_INFO_NONE, NULL, NULL);
}

static void
finish_job (void)
{
    Item *it = g_steal_pointer (&app.job);
    g_autoptr (GSubprocess) proc = g_steal_pointer (&app.proc);
    g_autofree char *part = g_steal_pointer (&app.part_path);
    g_autofree char *out = g_steal_pointer (&app.out_path);
    g_autofree char *last_err = g_steal_pointer (&app.last_err);
    app.paused = FALSE;
    app.batch_end = g_get_monotonic_time ();
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
        set_status (it, NULL, NULL);
    } else {
        g_autoptr (GFile) src = g_file_new_for_path (part);
        g_autoptr (GFile) dst = g_file_new_for_path (out);
        g_autoptr (GError) error = NULL;
        /* No OVERWRITE flag: an existing output is never replaced. */
        if (!g_file_move (src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
            g_unlink (part);
            set_status (it, "Failed", error->message);
        } else {
            copy_mtime (it->path, out);
            it->state = ST_DONE;
            it->out = g_steal_pointer (&out);
            it->out_size = st.st_size;
            set_status (it, NULL, it->out);
            if (gtk_switch_get_active (GTK_SWITCH (app.trash)))
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
        if (vc_parse_progress_line (line, it->probe->duration, &fraction)) {
            it->fraction = fraction;
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), fraction);
            if (!it->stop) {
                g_autofree char *pct = g_strdup_printf ("%d%%", (int) (fraction * 100));
                gtk_label_set_text (GTK_LABEL (it->pct), pct);
            }
            update_progress ();
        } else if (it->probe->duration <= 0 && g_str_has_prefix (line, "progress=")) {
            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (it->bar));
        }
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

/* Runs in the child before exec: low CPU and idle disk priority, so the desktop wins while you
 * use the laptop and the encode runs at full speed when you don't. The PID stays ffmpeg's. */
static void
lower_priority (gpointer data)
{
    (void) data;
    setpriority (PRIO_PROCESS, 0, 10);
    syscall (SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, 3 << 13 /* IOPRIO_CLASS_IDLE */);
}

static gboolean
start_job (Item *it, char *out, char *part)
{
    VcOptions o = current_options ();
    g_auto (GStrv) argv = vc_build_ffmpeg_argv (it->path, part, it->probe, &o);
    g_autoptr (GError) error = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup (launcher, lower_priority, NULL, NULL);
    GSubprocess *proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) argv, &error);
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
    app.job_t0 = g_get_monotonic_time ();
    app.paused_total = 0;
    it->state = ST_ACTIVE;
    it->stop = FALSE;
    it->fraction = 0;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), 0);
    set_status (it, NULL, NULL);

    GDataInputStream *so = g_data_input_stream_new (g_subprocess_get_stdout_pipe (proc));
    GDataInputStream *se = g_data_input_stream_new (g_subprocess_get_stderr_pipe (proc));
    g_data_input_stream_read_line_async (so, G_PRIORITY_DEFAULT, NULL, on_progress_line, NULL);
    g_data_input_stream_read_line_async (se, G_PRIORITY_DEFAULT, NULL, on_stderr_line, NULL);
    g_subprocess_wait_async (proc, NULL, on_ffmpeg_exit, NULL);
    return TRUE;
}

/* Starts the next queued item unless an encode (even a paused one) is running: the one with
 * the highest "Compress next" priority, otherwise the first in list order. */
static void
encode_next (void)
{
    gboolean keep_subs = gtk_switch_get_active (GTK_SWITCH (app.subs));
    while (app.job == NULL) {
        Item *it = NULL;
        for (guint i = 0; i < app.items->len; i++) {
            Item *c = app.items->pdata[i];
            if (c->state == ST_QUEUED && (it == NULL || c->prio > it->prio))
                it = c;
        }
        if (it == NULL)
            break;
        it->prio = 0;
        char *part = NULL;
        char *out = vc_output_path (it->path, keep_subs, &part);
        GStatBuf st;
        if (g_stat (out, &st) == 0) {
            it->state = ST_DONE;
            g_free (it->out);
            it->out = out;   /* an earlier finished output, so its original can be trashed */
            it->out_size = st.st_size;
            it->extra = "Already compressed";
            set_status (it, NULL, out);
            g_free (part);
            continue;
        }
        start_job (it, out, part);   /* takes out and part */
    }
    update_controls ();
}

/* ---- button handlers ---- */

static void
show_in_files (const char *path, gboolean containing)
{
    g_autoptr (GFile) f = g_file_new_for_path (path);
    g_autoptr (GtkFileLauncher) l = gtk_file_launcher_new (f);
    if (containing)
        gtk_file_launcher_open_containing_folder (l, app.win, NULL, NULL, NULL);
    else
        gtk_file_launcher_launch (l, app.win, NULL, NULL, NULL);
}

static void
on_start_all (void)
{
    if (app.job != NULL && app.paused) {   /* Resume: continue the paused video only */
        resume_job ();
        update_controls ();
    } else {
        act_start (NULL, FALSE);
    }
}

static void
on_pause_all (void)
{
    if (app.job != NULL && !app.paused)
        pause_job ();
    update_controls ();
}

static void on_stop_all (void) { act_stop (NULL, FALSE); }
static void on_sel_start (void) { act_start (NULL, TRUE); }
static void on_sel_pause (void) { act_pause (NULL, TRUE); }
static void on_sel_stop (void) { act_stop (NULL, TRUE); }

static void
on_sel_trash (void)
{
    guint n = 0;
    for (guint i = 0; i < app.items->len; i++)
        n += checked (app.items->pdata[i]) && can_trash (app.items->pdata[i]);
    g_autofree char *message = g_strdup_printf ("Move %u original%s to Trash?", n,
                                                n == 1 ? "" : "s");
    confirm (message, "Each compressed copy is checked first. You can restore the originals "
                      "from the Trash." TRASH_CHECKS, "Move to Trash", trash_confirmed);
}

static void
set_all_checked (gboolean on)
{
    app.syncing = TRUE;
    for (guint i = 0; app.items != NULL && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        gtk_check_button_set_active (GTK_CHECK_BUTTON (it->check), on);
        render_row (it);
    }
    app.syncing = FALSE;
    update_controls ();
}

static void on_sel_clear (void) { set_all_checked (FALSE); }

static void
on_select_all (GtkCheckButton *b, gpointer user_data)
{
    (void) user_data;
    if (!app.syncing)
        set_all_checked (gtk_check_button_get_active (b));
}

static void
on_finished_show (void)
{
    g_autofree char *dir = g_build_filename (app.folder, "compressed", NULL);
    show_in_files (dir, FALSE);
}

static void on_row_play (GtkButton *b, Item *it) { (void) b; act_start (it, FALSE); }
static void on_row_pause (GtkButton *b, Item *it) { (void) b; act_pause (it, FALSE); }
static void on_row_stop (GtkButton *b, Item *it) { (void) b; act_stop (it, FALSE); }
static void on_row_show (GtkButton *b, Item *it) { (void) b; show_in_files (it->out, TRUE); }

/* One finished video: no dialog (the design's undo is the Trash itself), the toast says so. */
static void
on_row_trash (GtkButton *b, Item *it)
{
    (void) b;
    if (can_trash (it)) {
        check_then_trash (it);
        update_controls ();
    }
}

/* ---- row interaction: checkbox, Ctrl/Shift-click, hover, right-click ---- */

static void
on_check_toggled (GtkCheckButton *b, Item *it)
{
    (void) b;
    if (app.syncing)
        return;
    app.anchor = it;
    render_row (it);
    update_controls ();
}

static void
on_hover (GtkEventControllerMotion *m, double x, double y, Item *it)
{
    (void) m; (void) x; (void) y;
    it->hover = TRUE;
    render_actions (it);
}

static void
on_unhover (GtkEventControllerMotion *m, Item *it)
{
    (void) m;
    it->hover = FALSE;
    render_actions (it);
}

typedef void (*RowAction) (GtkButton *, Item *);

static void
on_menu_item (GtkButton *b, Item *it)
{
    RowAction act = g_object_get_data (G_OBJECT (b), "act");
    gtk_popover_popdown (GTK_POPOVER (gtk_widget_get_ancestor (GTK_WIDGET (b), GTK_TYPE_POPOVER)));
    if (act != NULL)
        act (b, it);
    else
        gtk_check_button_set_active (GTK_CHECK_BUTTON (it->check), !checked (it));
}

static void
menu_item (GtkWidget *box, Item *it, const char *icon, const char *label, RowAction act,
           gboolean sensitive)
{
    GtkWidget *b = gtk_button_new ();
    GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append (GTK_BOX (h), gtk_image_new_from_icon_name (icon));
    gtk_box_append (GTK_BOX (h), gtk_label_new (label));
    gtk_button_set_child (GTK_BUTTON (b), h);
    gtk_widget_add_css_class (b, "flat");
    gtk_widget_set_sensitive (b, sensitive);
    g_object_set_data (G_OBJECT (b), "act", act);
    g_signal_connect (b, "clicked", G_CALLBACK (on_menu_item), it);
    gtk_box_append (GTK_BOX (box), b);
}

static void
menu_unparent (gpointer pop)
{
    if (gtk_widget_get_parent (pop) != NULL)
        gtk_widget_unparent (pop);
    g_object_unref (pop);
}

static void
on_menu_closed (GtkPopover *pop, gpointer user_data)
{
    (void) user_data;
    g_idle_add_once (menu_unparent, g_object_ref (pop));   /* not while it is still closing */
}

static void
show_menu (Item *it, double x, double y)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gboolean job = it == app.job;
    menu_item (box, it, "media-playback-start-symbolic",
               job && app.paused ? "Resume" : "Compress Now", on_row_play,
               can_start (it) || it->state == ST_QUEUED);
    menu_item (box, it, "media-playback-pause-symbolic", job ? "Pause" : "Hold", on_row_pause,
               can_pause (it));
    menu_item (box, it, "media-playback-stop-symbolic", job ? "Stop" : "Skip", on_row_stop,
               can_stop (it));
    gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    menu_item (box, it, "folder-open-symbolic", "Show in Files", on_row_show, it->out != NULL);
    menu_item (box, it, "user-trash-symbolic", "Move Original to Trash", on_row_trash,
               can_trash (it));
    gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    menu_item (box, it, "object-select-symbolic", checked (it) ? "Deselect" : "Select", NULL, TRUE);

    GtkWidget *pop = gtk_popover_new ();
    gtk_widget_add_css_class (pop, "menu");
    gtk_popover_set_child (GTK_POPOVER (pop), box);
    gtk_popover_set_has_arrow (GTK_POPOVER (pop), FALSE);
    gtk_widget_set_halign (pop, GTK_ALIGN_START);
    gtk_widget_set_parent (pop, it->box);
    gtk_popover_set_pointing_to (GTK_POPOVER (pop), &(GdkRectangle) { (int) x, (int) y, 1, 1 });
    g_signal_connect (pop, "closed", G_CALLBACK (on_menu_closed), NULL);
    gtk_popover_popup (GTK_POPOVER (pop));
}

static void
on_row_pressed (GtkGestureClick *g, int n_press, double x, double y, Item *it)
{
    (void) n_press;
    guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (g));
    if (button == GDK_BUTTON_SECONDARY) {
        show_menu (it, x, y);
        return;
    }
    GtkWidget *picked = gtk_widget_pick (it->box, x, y, GTK_PICK_DEFAULT);
    if (button != GDK_BUTTON_PRIMARY || picked == NULL
        || gtk_widget_get_ancestor (picked, GTK_TYPE_BUTTON) != NULL
        || gtk_widget_get_ancestor (picked, GTK_TYPE_CHECK_BUTTON) != NULL)
        return;
    GdkModifierType mods = gtk_event_controller_get_current_event_state (
        GTK_EVENT_CONTROLLER (g));
    if ((mods & GDK_SHIFT_MASK) && app.anchor != NULL && app.anchor != it) {
        guint a = 0, b = 0;
        g_ptr_array_find (app.items, app.anchor, &a);
        g_ptr_array_find (app.items, it, &b);
        app.syncing = TRUE;
        for (guint i = MIN (a, b); i <= MAX (a, b); i++) {
            Item *r = app.items->pdata[i];
            gtk_check_button_set_active (GTK_CHECK_BUTTON (r->check), TRUE);
            render_row (r);
        }
        app.syncing = FALSE;
        update_controls ();
    } else if (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        gtk_check_button_set_active (GTK_CHECK_BUTTON (it->check), !checked (it));
    }
}

static GtkWidget *
mini_button (const char *icon, const char *name, GCallback clicked, Item *it)
{
    GtkWidget *b = gtk_button_new_from_icon_name (icon);
    gtk_widget_add_css_class (b, "mini");
    gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
    gtk_widget_set_visible (b, FALSE);
    gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect (b, "clicked", clicked, it);
    return b;
}

static GtkWidget *
make_row (Item *it)
{
    it->check = gtk_check_button_new ();
    gtk_widget_set_valign (it->check, GTK_ALIGN_CENTER);
    gtk_accessible_update_property (GTK_ACCESSIBLE (it->check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Select", -1);
    g_signal_connect (it->check, "toggled", G_CALLBACK (on_check_toggled), it);

    g_autofree char *display = basename_display (it->path);
    g_autofree char *tile_class = g_strdup_printf ("tile-%u", g_str_hash (it->path) % 8);
    GtkWidget *tile = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
    gtk_widget_add_css_class (tile, "tile");
    gtk_widget_add_css_class (tile, tile_class);
    gtk_widget_set_valign (tile, GTK_ALIGN_CENTER);

    GtkWidget *name = gtk_label_new (display);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_widget_add_css_class (name, "name");
    it->meta = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (it->meta), 0);
    gtk_label_set_ellipsize (GTK_LABEL (it->meta), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (it->meta, "caption");
    GtkWidget *names = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (names, TRUE);
    gtk_widget_set_valign (names, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (names), name);
    gtk_box_append (GTK_BOX (names), it->meta);

    it->chips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_size_group_add_widget (app.col_quality, it->chips);

    it->size_lbl = gtk_label_new ("");
    it->pill = gtk_label_new ("");
    gtk_widget_set_valign (it->pill, GTK_ALIGN_CENTER);
    it->bar = gtk_progress_bar_new ();
    gtk_widget_set_size_request (it->bar, 120, -1);
    gtk_widget_set_valign (it->bar, GTK_ALIGN_CENTER);
    it->pct = gtk_label_new ("");
    gtk_widget_set_size_request (it->pct, 40, -1);
    gtk_label_set_xalign (GTK_LABEL (it->pct), 1);
    GtkWidget *status = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign (status, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (status), it->size_lbl);
    gtk_box_append (GTK_BOX (status), it->bar);
    gtk_box_append (GTK_BOX (status), it->pct);
    gtk_box_append (GTK_BOX (status), it->pill);
    GtkWidget *status_col = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (status_col), status);
    gtk_widget_set_hexpand (status, TRUE);
    gtk_widget_set_hexpand (status_col, FALSE);   /* right-align inside the column, don't grow it */
    gtk_size_group_add_widget (app.col_status, status_col);

    it->b_play = mini_button ("media-playback-start-symbolic", "Compress next",
                              G_CALLBACK (on_row_play), it);
    it->b_pause = mini_button ("media-playback-pause-symbolic", "Pause or hold",
                               G_CALLBACK (on_row_pause), it);
    it->b_stop = mini_button ("media-playback-stop-symbolic", "Stop or skip",
                              G_CALLBACK (on_row_stop), it);
    it->b_show = mini_button ("folder-open-symbolic", "Show in Files", G_CALLBACK (on_row_show), it);
    set_tip (it->b_show, "Show in Files", NULL, "Open the compressed folder with this video selected.");
    it->b_trash = mini_button ("user-trash-symbolic", "Move original to Trash",
                               G_CALLBACK (on_row_trash), it);
    gtk_widget_add_css_class (it->b_trash, "danger");
    set_tip (it->b_trash, "Move original to Trash", NULL,
             "Only for finished videos whose copy was checked. Restorable from Trash.");
    GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign (actions, GTK_ALIGN_END);
    GtkWidget *b[] = { it->b_play, it->b_pause, it->b_stop, it->b_show, it->b_trash };
    for (gsize i = 0; i < G_N_ELEMENTS (b); i++)
        gtk_box_append (GTK_BOX (actions), b[i]);
    gtk_size_group_add_widget (app.col_actions, actions);

    it->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class (it->box, "vrow");
    gtk_box_append (GTK_BOX (it->box), it->check);
    gtk_box_append (GTK_BOX (it->box), tile);
    gtk_box_append (GTK_BOX (it->box), names);
    gtk_box_append (GTK_BOX (it->box), it->chips);
    gtk_box_append (GTK_BOX (it->box), status_col);
    gtk_box_append (GTK_BOX (it->box), actions);

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
    g_signal_connect (click, "pressed", G_CALLBACK (on_row_pressed), it);
    gtk_widget_add_controller (it->box, GTK_EVENT_CONTROLLER (click));
    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "enter", G_CALLBACK (on_hover), it);
    g_signal_connect (motion, "leave", G_CALLBACK (on_unhover), it);
    gtk_widget_add_controller (it->box, motion);
    return it->box;
}

/* ---- choosing a folder ---- */

static void
load_folder (const char *folder)
{
    g_cancellable_cancel (app.probe_cancel);
    g_clear_object (&app.probe_cancel);
    app.probe_cancel = g_cancellable_new ();
    gtk_list_box_remove_all (GTK_LIST_BOX (app.list));
    g_clear_pointer (&app.items, g_ptr_array_unref);
    app.items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
    app.probe_next = app.probes_done = 0;
    app.anchor = NULL;
    app.batch_t0 = app.batch_end = 0;
    g_free (app.folder);
    app.folder = g_strdup (folder);

    g_autofree char *base = basename_display (folder);
    g_autofree char *display = g_filename_display_name (folder);
    const char *home = g_get_home_dir ();
    gsize hl = strlen (home);
    g_autofree char *sub = strncmp (display, home, hl) == 0 && (display[hl] == '/' || !display[hl])
                               ? g_strconcat ("~", display + hl, NULL)
                               : g_strdup (display);
    gtk_label_set_text (GTK_LABEL (app.title), base);
    gtk_label_set_text (GTK_LABEL (app.subtitle), sub);

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
        gtk_list_box_append (GTK_LIST_BOX (app.list), make_row (it));
        it->row = gtk_widget_get_parent (it->box);
        render_row (it);
    }
    update_speed_guide ();
    update_controls ();
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
on_choose (void)
{
    g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (d, "Choose a folder of videos");
    gtk_file_dialog_select_folder (d, app.win, NULL, folder_chosen, NULL);
}

static gboolean
on_drop (GtkDropTarget *t, const GValue *value, double x, double y, gpointer user_data)
{
    (void) t; (void) x; (void) y; (void) user_data;
    if (!app.tools_ok || is_busy () || !G_VALUE_HOLDS (value, G_TYPE_FILE))
        return FALSE;
    g_autofree char *path = g_file_get_path (g_value_get_object (value));
    if (path == NULL || !g_file_test (path, G_FILE_TEST_IS_DIR))
        return FALSE;
    load_folder (path);
    return TRUE;
}

/* Estimates depend on the speed and the size cap. */
static void
on_estimate_input (void)
{
    for (guint i = 0; app.items != NULL && i < app.items->len; i++)
        render_row (app.items->pdata[i]);
    update_speed_guide ();
    update_controls ();
}

static void
on_speed (GtkToggleButton *b, gpointer user_data)
{
    if (!gtk_toggle_button_get_active (b))
        return;
    app.preset = GPOINTER_TO_UINT (user_data);
    on_estimate_input ();
}

/* ---- keyboard ---- */

static gboolean
on_space (GtkWidget *w, GVariant *args, gpointer user_data)
{
    (void) w; (void) args; (void) user_data;
    if (app.job != NULL && !app.paused)
        on_pause_all ();
    else if (gtk_widget_get_sensitive (app.start_btn) && gtk_widget_get_visible (app.controls))
        on_start_all ();
    return TRUE;
}

static gboolean
on_escape (GtkWidget *w, GVariant *args, gpointer user_data)
{
    (void) w; (void) args; (void) user_data;
    if (gtk_widget_get_visible (app.selbar))
        set_all_checked (FALSE);   /* first Esc clears the selection, the next one stops */
    else if (app.items != NULL && gtk_widget_get_sensitive (app.stop_btn))
        on_stop_all ();
    return TRUE;
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
    if (app.toast_id != 0)
        g_source_remove (app.toast_id);
    return FALSE;
}

/* ---- startup ---- */

static void
quit_after_alert (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
    gtk_window_destroy (app.win);
}

static void
tools_failed (const char *problem)
{
    if (!app.closing)
        alert ("Video Compressor can't run", problem, quit_after_alert);
}

static void
gpu_test_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    if (!g_subprocess_wait_check_finish (proc, res, NULL) || app.closing)
        return;   /* no working GPU encoder: the GPU speed stays hidden */
    app.gpu_ok = TRUE;
    gtk_widget_set_visible (app.speed_btns[0], TRUE);
    update_speed_guide ();
}

static void
encoders_listed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, NULL)
        || !g_subprocess_get_successful (proc) || strstr (out, " libx265 ") == NULL) {
        tools_failed ("This ffmpeg has no libx265 (H.265) encoder.\n"
                      "Install the full ffmpeg build from RPM Fusion.");
        return;
    }
    if (app.closing)
        return;
    app.tools_ok = TRUE;
    update_controls ();
    /* The list shows hardware encoders even when they can't run, so try one frame. */
    if (strstr (out, " hevc_vaapi ") == NULL)
        return;
    g_auto (GStrv) argv = vc_build_gpu_test_argv ();
    GSubprocess *test = g_subprocess_newv ((const char *const *) argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (test != NULL)
        g_subprocess_wait_check_async (test, NULL, gpu_test_done, NULL);
}

/* Asynchronous so the window paints at once; Choose Folder stays off until it passes. */
static void
check_tools (void)
{
    g_autofree char *ffmpeg = g_find_program_in_path ("ffmpeg");
    g_autofree char *ffprobe = g_find_program_in_path ("ffprobe");
    if (ffmpeg == NULL || ffprobe == NULL) {
        tools_failed ("ffmpeg and ffprobe must be installed and on PATH.\n"
                      "On Fedora: sudo dnf install ffmpeg (from RPM Fusion).");
        return;
    }
    const char *argv[] = { ffmpeg, "-hide_banner", "-encoders", NULL };
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                 | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL)
        tools_failed (error->message);
    else
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, encoders_listed, NULL);
}

static const char CSS[] =
    ".dropzone { border: 2px dashed alpha(@accent_color, 0.45); border-radius: 16px;"
    "  background: alpha(@accent_color, 0.05); }"
    ".drop-icon { background: linear-gradient(135deg, #62a0ea, #1c71d8); color: white;"
    "  border-radius: 20px; min-width: 76px; min-height: 76px; -gtk-icon-size: 34px;"
    "  box-shadow: 0 6px 18px alpha(#1c71d8, 0.35); }"
    ".hero { font-size: 20px; font-weight: 700; }"
    ".feature { border-radius: 999px; padding: 4px 12px; font-size: 12px;"
    "  background: @theme_base_color; border: 1px solid @borders; }"
    ".feature image { color: @accent_color; }"
    ".card { background: @theme_base_color; border: 1px solid @borders; border-radius: 12px; }"
    ".colhead { font-size: 11px; font-weight: 600; letter-spacing: 1px; opacity: 0.55; }"
    ".colhead-row { padding: 8px 16px; border-bottom: 1px solid @borders; }"
    "list.rows { background: transparent; }"
    "list.rows > row { padding: 0; border-bottom: 1px solid alpha(currentColor, 0.07); }"
    "list.rows > row:last-child { border-bottom: none; }"
    "list.rows > row:hover { background: alpha(currentColor, 0.04); }"
    "list.rows > row.active { background: alpha(@theme_selected_bg_color, 0.08); }"
    "list.rows > row.checked { background: alpha(@theme_selected_bg_color, 0.14); }"
    ".vrow { padding: 8px 16px; min-height: 40px; }"
    ".name { font-weight: 600; }"
    ".caption { font-size: 12px; opacity: 0.6; }"
    ".tile { border-radius: 6px; min-width: 44px; min-height: 30px; color: white;"
    "  -gtk-icon-size: 12px; }"
    ".tile-0 { background: linear-gradient(135deg, #ff9a9e, #f5576c); }"
    ".tile-1 { background: linear-gradient(135deg, #a18cd1, #6a5acd); }"
    ".tile-2 { background: linear-gradient(135deg, #43c6ac, #191654); }"
    ".tile-3 { background: linear-gradient(135deg, #e6a57e, #c06c84); }"
    ".tile-4 { background: linear-gradient(135deg, #00c6ff, #0072ff); }"
    ".tile-5 { background: linear-gradient(135deg, #f6d365, #fda085); }"
    ".tile-6 { background: linear-gradient(135deg, #a1c4fd, #6b7fae); }"
    ".tile-7 { background: linear-gradient(135deg, #84fab0, #2f9e7a); }"
    ".chip { border-radius: 5px; padding: 2px 7px; font-size: 11px; font-weight: 600;"
    "  background: alpha(currentColor, 0.09); }"
    ".chip.hdr { background: alpha(#f5a623, 0.2); color: #c47a00; }"
    ".pill { border-radius: 999px; padding: 2px 9px; font-size: 12px; font-weight: 600; }"
    ".pill.estimate { border: 1px dashed alpha(currentColor, 0.45); font-weight: normal; }"
    ".pill.done { background: alpha(@success_color, 0.16); color: @success_color; }"
    ".pill.hold { background: alpha(#f5a623, 0.2); color: #c47a00; }"
    ".pill.error { background: alpha(#e5484d, 0.15); color: #e5484d; }"
    ".pill.waiting, .pill.neutral { font-weight: normal; opacity: 0.7; }"
    "button.mini { min-width: 26px; min-height: 26px; padding: 0; border-radius: 999px;"
    "  background: alpha(currentColor, 0.08); box-shadow: none; border: none; }"
    "button.mini:hover { background: alpha(currentColor, 0.16); }"
    "button.mini.suggested-action { background: @accent_color; color: white; }"
    "button.mini.danger { color: @error_color; }"
    "progressbar trough, progressbar progress { min-height: 5px; border-radius: 3px; }"
    "progressbar.paused progress { background: #f5a623; }"
    "progressbar.time progress { background: #f5a623; }"
    "progressbar.size progress { background: @success_color; }"
    ".bottombar { background: @theme_base_color; border-top: 1px solid @borders;"
    "  padding: 12px 20px; }"
    "button.round { border-radius: 999px; min-width: 38px; min-height: 38px; padding: 0; }"
    "button.round.big { min-width: 48px; min-height: 48px; }"
    ".total { font-weight: 700; }"
    ".overlaybar { background: rgba(30,30,32,0.94); color: white; border-radius: 999px;"
    "  padding: 6px 8px 6px 16px; box-shadow: 0 6px 20px rgba(0,0,0,0.3); }"
    ".overlaybar button { color: white; background: alpha(white, 0.12); border: none;"
    "  box-shadow: none; }"
    ".overlaybar button:disabled { color: alpha(white, 0.35); background: alpha(white, 0.06); }"
    ".overlaybar button.suggested-action { background: @accent_color; }"
    ".toast { background: rgba(30,30,32,0.94); color: white; border-radius: 10px;"
    "  padding: 10px 16px; box-shadow: 0 6px 20px rgba(0,0,0,0.3); }"
    ".bigcheck { background: @success_color; color: white; border-radius: 999px;"
    "  min-width: 46px; min-height: 46px; -gtk-icon-size: 24px; }"
    ".sum-title { font-size: 20px; font-weight: 700; }"
    ".stat { font-size: 17px; font-weight: 700; }"
    ".stat.good { color: @success_color; }"
    "menubutton.info > button { color: @accent_color; background: none; border: none;"
    "  box-shadow: none; min-width: 18px; min-height: 18px; padding: 0; }"
    ".guide { padding: 8px; }"
    ".guide-row { padding: 8px 10px; border-radius: 8px; }"
    ".guide-row.current { background: alpha(@theme_selected_bg_color, 0.14); }"
    ".guide-row.current > label:first-child { color: @accent_color; font-weight: 600; }";

static GtkWidget *
label_button (const char *icon, const char *mnemonic, GCallback clicked)
{
    GtkWidget *b = gtk_button_new ();
    GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l = gtk_label_new_with_mnemonic (mnemonic);
    gtk_label_set_mnemonic_widget (GTK_LABEL (l), b);
    gtk_box_append (GTK_BOX (h), gtk_image_new_from_icon_name (icon));
    gtk_box_append (GTK_BOX (h), l);
    gtk_button_set_child (GTK_BUTTON (b), h);
    g_signal_connect (b, "clicked", clicked, NULL);
    return b;
}

static GtkWidget *
icon_button (const char *icon, const char *name, const char *style, GCallback clicked)
{
    GtkWidget *b = gtk_button_new_from_icon_name (icon);
    gtk_widget_add_css_class (b, style);
    gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect (b, "clicked", clicked, NULL);
    return b;
}

static GtkWidget *
vbox (int spacing)
{
    return gtk_box_new (GTK_ORIENTATION_VERTICAL, spacing);
}

static GtkWidget *
hbox (int spacing)
{
    return gtk_box_new (GTK_ORIENTATION_HORIZONTAL, spacing);
}

static GtkWidget *
styled_label (const char *text, const char *style, float xalign)
{
    GtkWidget *l = gtk_label_new (text);
    if (style != NULL)
        gtk_widget_add_css_class (l, style);
    gtk_label_set_xalign (GTK_LABEL (l), xalign);
    return l;
}

static GtkWidget *
build_empty_page (void)
{
    GtkWidget *icon = gtk_image_new_from_icon_name ("folder-download-symbolic");
    gtk_widget_add_css_class (icon, "drop-icon");
    gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
    GtkWidget *choose = label_button ("folder-symbolic", "_Choose Folder…", G_CALLBACK (on_choose));
    app.choose_empty = choose;
    gtk_widget_add_css_class (choose, "suggested-action");
    gtk_widget_set_halign (choose, GTK_ALIGN_CENTER);
    GtkWidget *features = hbox (8);
    gtk_widget_set_halign (features, GTK_ALIGN_CENTER);
    const char *f[][2] = { { "starred-symbolic", "Quality set automatically" },
                           { "security-high-symbolic", "Originals never changed" },
                           { "alarm-symbolic", "Cancel any time" } };
    for (gsize i = 0; i < G_N_ELEMENTS (f); i++) {
        GtkWidget *c = hbox (6);
        gtk_widget_add_css_class (c, "feature");
        gtk_box_append (GTK_BOX (c), gtk_image_new_from_icon_name (f[i][0]));
        gtk_box_append (GTK_BOX (c), gtk_label_new (f[i][1]));
        gtk_box_append (GTK_BOX (features), c);
    }
    GtkWidget *inner = vbox (12);
    gtk_widget_set_valign (inner, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand (inner, TRUE);
    gtk_box_append (GTK_BOX (inner), icon);
    gtk_box_append (GTK_BOX (inner), styled_label ("Drop a folder of videos here", "hero", 0.5));
    GtkWidget *sub = styled_label ("Every video inside is compressed to H.265, keeping its "
                                   "quality and resolution.", "dim-label", 0.5);
    gtk_label_set_wrap (GTK_LABEL (sub), TRUE);
    gtk_box_append (GTK_BOX (inner), sub);
    gtk_widget_set_margin_bottom (sub, 8);
    gtk_box_append (GTK_BOX (inner), choose);
    gtk_widget_set_margin_top (features, 20);
    gtk_box_append (GTK_BOX (inner), features);
    GtkWidget *zone = vbox (0);
    gtk_widget_add_css_class (zone, "dropzone");
    gtk_widget_set_margin_start (zone, 24);
    gtk_widget_set_margin_end (zone, 24);
    gtk_widget_set_margin_top (zone, 24);
    gtk_widget_set_margin_bottom (zone, 24);
    gtk_box_append (GTK_BOX (zone), inner);
    return zone;
}

static GtkWidget *
stat_box (GtkWidget **value, const char *caption, gboolean good)
{
    *value = styled_label ("", "stat", 1);
    if (good)
        gtk_widget_add_css_class (*value, "good");
    GtkWidget *v = vbox (0);
    gtk_box_append (GTK_BOX (v), *value);
    gtk_box_append (GTK_BOX (v), styled_label (caption, "caption", 1));
    return v;
}

static GtkWidget *
build_list_page (void)
{
    /* finished summary */
    GtkWidget *ok = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_widget_add_css_class (ok, "bigcheck");
    gtk_widget_set_valign (ok, GTK_ALIGN_CENTER);
    app.sum_title = styled_label ("", "sum-title", 0);
    app.sum_sub = styled_label ("", "dim-label", 0);
    GtkWidget *texts = vbox (2);
    gtk_widget_set_hexpand (texts, TRUE);
    gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (texts), app.sum_title);
    gtk_box_append (GTK_BOX (texts), app.sum_sub);
    app.summary = hbox (16);
    gtk_widget_add_css_class (app.summary, "card");
    gtk_widget_set_margin_bottom (app.summary, 14);
    GtkWidget *sum_in = hbox (16);
    gtk_widget_set_hexpand (sum_in, TRUE);
    gtk_widget_set_margin_start (sum_in, 18);
    gtk_widget_set_margin_end (sum_in, 22);
    gtk_widget_set_margin_top (sum_in, 14);
    gtk_widget_set_margin_bottom (sum_in, 14);
    gtk_box_append (GTK_BOX (sum_in), ok);
    gtk_box_append (GTK_BOX (sum_in), texts);
    gtk_box_append (GTK_BOX (sum_in), stat_box (&app.sum_before, "Before", FALSE));
    gtk_box_append (GTK_BOX (sum_in), stat_box (&app.sum_after, "After", FALSE));
    gtk_box_append (GTK_BOX (sum_in), stat_box (&app.sum_pct, "Smaller", TRUE));
    gtk_box_append (GTK_BOX (app.summary), sum_in);
    gtk_widget_set_visible (app.summary, FALSE);

    /* column header, aligned with the rows by size groups */
    app.select_all = gtk_check_button_new ();
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.select_all), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Select all", -1);
    g_signal_connect (app.select_all, "toggled", G_CALLBACK (on_select_all), NULL);
    GtkWidget *tile_space = hbox (0);
    gtk_widget_set_size_request (tile_space, 44, -1);
    GtkWidget *name = styled_label ("NAME", "colhead", 0);
    gtk_widget_set_hexpand (name, TRUE);
    GtkWidget *quality = styled_label ("QUALITY", "colhead", 0);
    gtk_widget_set_size_request (quality, 150, -1);
    gtk_size_group_add_widget (app.col_quality, quality);
    GtkWidget *size = styled_label ("SIZE → ESTIMATED", "colhead", 1);
    gtk_widget_set_size_request (size, 290, -1);
    gtk_size_group_add_widget (app.col_status, size);
    GtkWidget *actions_space = hbox (0);
    gtk_widget_set_size_request (actions_space, 90, -1);
    gtk_size_group_add_widget (app.col_actions, actions_space);
    GtkWidget *head = hbox (14);
    gtk_widget_add_css_class (head, "colhead-row");
    gtk_box_append (GTK_BOX (head), app.select_all);
    gtk_box_append (GTK_BOX (head), tile_space);
    gtk_box_append (GTK_BOX (head), name);
    gtk_box_append (GTK_BOX (head), quality);
    gtk_box_append (GTK_BOX (head), size);
    gtk_box_append (GTK_BOX (head), actions_space);

    app.list = gtk_list_box_new ();
    gtk_widget_add_css_class (app.list, "rows");
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (app.list), GTK_SELECTION_NONE);
    GtkWidget *placeholder = styled_label ("No videos in this folder.", "dim-label", 0.5);
    gtk_widget_set_margin_top (placeholder, 32);
    gtk_widget_set_margin_bottom (placeholder, 32);
    gtk_list_box_set_placeholder (GTK_LIST_BOX (app.list), placeholder);
    GtkWidget *card = vbox (0);
    gtk_widget_add_css_class (card, "card");
    gtk_widget_set_overflow (card, GTK_OVERFLOW_HIDDEN);
    gtk_box_append (GTK_BOX (card), head);
    gtk_box_append (GTK_BOX (card), app.list);

    GtkWidget *content = vbox (0);
    gtk_widget_set_margin_start (content, 20);
    gtk_widget_set_margin_end (content, 20);
    gtk_widget_set_margin_top (content, 18);
    gtk_widget_set_margin_bottom (content, 80);   /* room for the selection bar */
    gtk_box_append (GTK_BOX (content), app.summary);
    gtk_box_append (GTK_BOX (content), card);
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), content);

    /* floating selection bar */
    app.sel_label = styled_label ("", "heading", 0);
    gtk_widget_set_margin_end (app.sel_label, 6);
    app.sel_start = icon_button ("media-playback-start-symbolic", "Compress selected", "round",
                                 G_CALLBACK (on_sel_start));
    gtk_widget_add_css_class (app.sel_start, "suggested-action");
    app.sel_pause = icon_button ("media-playback-pause-symbolic", "Pause or hold selected", "round",
                                G_CALLBACK (on_sel_pause));
    set_tip (app.sel_pause, "Pause / Hold", NULL,
             "Running: pause it. Waiting: hold it, so the queue skips it until you resume.");
    app.sel_stop = icon_button ("media-playback-stop-symbolic", "Stop or skip selected", "round",
                               G_CALLBACK (on_sel_stop));
    set_tip (app.sel_stop, "Stop / Skip", NULL,
             "Running: stop and remove the unfinished file. Waiting: take it out of this batch.");
    app.sel_trash = icon_button ("user-trash-symbolic", "Move selected originals to Trash", "round",
                                G_CALLBACK (on_sel_trash));
    set_tip (app.sel_trash, "Move originals to Trash", NULL,
             "For selected finished videos, after checking each copy. Asks first.");
    GtkWidget *clear = icon_button ("window-close-symbolic", "Clear selection", "round",
                                    G_CALLBACK (on_sel_clear));
    set_tip (clear, "Clear selection", "Esc", "Buttons below act on every video again.");
    app.selbar = hbox (8);
    gtk_widget_add_css_class (app.selbar, "overlaybar");
    gtk_widget_set_halign (app.selbar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (app.selbar, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom (app.selbar, 16);
    GtkWidget *sel_items[] = { app.sel_label, app.sel_start, app.sel_pause, app.sel_stop,
                               app.sel_trash, gtk_separator_new (GTK_ORIENTATION_VERTICAL), clear };
    for (gsize i = 0; i < G_N_ELEMENTS (sel_items); i++)
        gtk_box_append (GTK_BOX (app.selbar), sel_items[i]);
    gtk_widget_set_visible (app.selbar, FALSE);

    /* toast */
    app.toast_label = gtk_label_new ("");
    GtkWidget *t = hbox (10);
    gtk_widget_add_css_class (t, "toast");
    gtk_box_append (GTK_BOX (t), gtk_image_new_from_icon_name ("user-trash-symbolic"));
    gtk_box_append (GTK_BOX (t), app.toast_label);
    app.toast = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (app.toast),
                                      GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_child (GTK_REVEALER (app.toast), t);
    gtk_widget_set_halign (app.toast, GTK_ALIGN_START);
    gtk_widget_set_valign (app.toast, GTK_ALIGN_END);
    gtk_widget_set_margin_start (app.toast, 20);
    gtk_widget_set_margin_bottom (app.toast, 16);
    gtk_widget_set_can_target (app.toast, FALSE);

    GtkWidget *overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (overlay), scroll);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), app.selbar);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), app.toast);
    return overlay;
}

static GtkWidget *
build_bottom_bar (void)
{
    /* ready: speed, switches, totals */
    GtkWidget *speed_label = gtk_label_new_with_mnemonic ("S_peed");
    gtk_widget_add_css_class (speed_label, "caption");
    GtkWidget *guide_head = hbox (16);
    gtk_widget_set_margin_start (guide_head, 10);
    const char *heads[] = { "SPEED", "ENCODE TIME", "FILE SIZE" };
    for (guint i = 0; i < 3; i++) {
        GtkWidget *h = styled_label (heads[i], "colhead", 0);
        gtk_size_group_add_widget (app.guide_cols[i], h);
        gtk_box_append (GTK_BOX (guide_head), h);
    }
    app.guide_rows = vbox (2);
    GtkWidget *guide = vbox (6);
    gtk_widget_add_css_class (guide, "guide");
    gtk_box_append (GTK_BOX (guide), styled_label ("Speed guide", "heading", 0));
    gtk_box_append (GTK_BOX (guide), styled_label ("Time compared with Medium · size compared "
                                                   "with your originals", "caption", 0));
    gtk_widget_set_margin_top (guide_head, 8);
    gtk_box_append (GTK_BOX (guide), guide_head);
    gtk_box_append (GTK_BOX (guide), app.guide_rows);
    GtkWidget *note = styled_label ("Rough estimates for the videos in this folder. At the same "
                                    "quality setting, slower speeds keep more detail; they don't "
                                    "reliably make files smaller.", "caption", 0);
    gtk_label_set_wrap (GTK_LABEL (note), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (note), 50);
    gtk_widget_set_margin_top (note, 8);
    gtk_box_append (GTK_BOX (guide), note);
    GtkWidget *pop = gtk_popover_new ();
    gtk_popover_set_child (GTK_POPOVER (pop), guide);
    GtkWidget *info = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (info), "help-about-symbolic");
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (info), pop);
    gtk_widget_add_css_class (info, "info");
    gtk_widget_set_tooltip_text (info, "Speed guide");
    GtkWidget *speed_head = hbox (2);
    gtk_box_append (GTK_BOX (speed_head), speed_label);
    gtk_box_append (GTK_BOX (speed_head), info);

    GtkWidget *seg = hbox (0);
    gtk_widget_add_css_class (seg, "linked");
    for (guint p = 0; VC_PRESETS[p] != NULL && p < G_N_ELEMENTS (app.speed_btns); p++) {
        g_autofree char *t = preset_title (p);
        GtkWidget *b = gtk_toggle_button_new_with_label (t);
        if (p > 0)
            gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b),
                                         GTK_TOGGLE_BUTTON (app.speed_btns[0]));
        if (strcmp (VC_PRESETS[p], VC_DEFAULT_PRESET) == 0) {
            app.preset = p;
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), TRUE);
        }
        g_signal_connect (b, "toggled", G_CALLBACK (on_speed), GUINT_TO_POINTER (p));
        gtk_widget_set_visible (b, strcmp (VC_PRESETS[p], VC_PRESET_GPU) != 0);   /* gpu_test_done */
        app.speed_btns[p] = b;
        gtk_box_append (GTK_BOX (seg), b);
    }
    gtk_label_set_mnemonic_widget (GTK_LABEL (speed_label), app.speed_btns[app.preset]);
    app.speed_caption = styled_label ("", "caption", 0);
    GtkWidget *speed = vbox (4);
    gtk_box_append (GTK_BOX (speed), speed_head);
    gtk_box_append (GTK_BOX (speed), seg);
    gtk_box_append (GTK_BOX (speed), app.speed_caption);

    GtkWidget *switches = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (switches), 8);
    gtk_grid_set_column_spacing (GTK_GRID (switches), 16);
    gtk_widget_set_valign (switches, GTK_ALIGN_CENTER);
    GtkWidget **sw[] = { &app.subs, &app.trash, &app.fps30 };
    const char *sw_label[] = { "Keep _subtitles", "Move originals to _Trash", "Max _30 fps" };
    const char *sw_tip[] = { "Copy subtitle tracks too. The output becomes .mkv.",
                             "After each copy is checked: same length and at least 5% smaller. "
                             "Asks before starting.",
                             "Save videos above 30 fps (often 60 fps phone clips) at 30 fps: about "
                             "half the encode time. Off keeps every frame." };
    for (guint i = 0; i < 3; i++) {
        *sw[i] = gtk_switch_new ();
        gtk_widget_set_valign (*sw[i], GTK_ALIGN_CENTER);
        GtkWidget *l = gtk_label_new_with_mnemonic (sw_label[i]);
        gtk_label_set_mnemonic_widget (GTK_LABEL (l), *sw[i]);
        gtk_accessible_update_relation (GTK_ACCESSIBLE (*sw[i]), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                        l, NULL, -1);
        GtkWidget *row = hbox (10);
        gtk_widget_set_tooltip_text (row, sw_tip[i]);
        gtk_box_append (GTK_BOX (row), *sw[i]);
        gtk_box_append (GTK_BOX (row), l);
        gtk_grid_attach (GTK_GRID (switches), row, i / 2, i % 2, 1, 1);
    }
    const char *sizes[] = { "Original size", "Max 1080p", "Max 720p", NULL };
    app.max_size = gtk_drop_down_new_from_strings (sizes);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.max_size), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Max size", -1);
    gtk_widget_set_tooltip_text (app.max_size, "Shrink bigger videos so the short side is at most "
                                               "this (a 4K video at 1080p takes about a quarter of "
                                               "the time). Smaller videos are never enlarged.");
    gtk_widget_set_halign (app.max_size, GTK_ALIGN_START);
    g_signal_connect (app.max_size, "notify::selected", G_CALLBACK (on_estimate_input), NULL);
    gtk_grid_attach (GTK_GRID (switches), app.max_size, 1, 1, 1, 1);

    app.total = styled_label ("", "total", 1);
    app.total_sub = styled_label ("", "caption", 1);
    /* totals give way first, so the extra options don't force a wider window */
    gtk_label_set_ellipsize (GTK_LABEL (app.total), PANGO_ELLIPSIZE_START);
    gtk_label_set_ellipsize (GTK_LABEL (app.total_sub), PANGO_ELLIPSIZE_END);
    GtkWidget *totals = vbox (2);
    gtk_widget_set_hexpand (totals, TRUE);
    gtk_widget_set_valign (totals, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (totals), app.total);
    gtk_box_append (GTK_BOX (totals), app.total_sub);

    GtkWidget *ready = hbox (28);
    gtk_box_append (GTK_BOX (ready), speed);
    gtk_box_append (GTK_BOX (ready), switches);
    gtk_box_append (GTK_BOX (ready), totals);

    /* running: overall progress */
    app.run_title = styled_label ("", NULL, 0);
    gtk_label_set_ellipsize (GTK_LABEL (app.run_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand (app.run_title, TRUE);
    app.run_eta = styled_label ("", "dim-label", 1);
    GtkWidget *run_head = hbox (12);
    gtk_box_append (GTK_BOX (run_head), app.run_title);
    gtk_box_append (GTK_BOX (run_head), app.run_eta);
    app.run_bar = gtk_progress_bar_new ();
    app.run_sub = styled_label ("", "caption", 0);
    GtkWidget *running = vbox (6);
    gtk_widget_set_valign (running, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (running), run_head);
    gtk_box_append (GTK_BOX (running), app.run_bar);
    gtk_box_append (GTK_BOX (running), app.run_sub);

    /* finished */
    GtkWidget *again = label_button ("folder-symbolic", "Choose _Another Folder…",
                                     G_CALLBACK (on_choose));
    GtkWidget *show = label_button ("folder-open-symbolic", "_Show in Files",
                                    G_CALLBACK (on_finished_show));
    gtk_widget_add_css_class (show, "suggested-action");
    GtkWidget *finished = hbox (10);
    gtk_widget_set_halign (finished, GTK_ALIGN_END);
    gtk_widget_set_valign (finished, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (finished), again);
    gtk_box_append (GTK_BOX (finished), show);

    app.bottom_stack = gtk_stack_new ();
    gtk_stack_set_hhomogeneous (GTK_STACK (app.bottom_stack), FALSE);
    gtk_stack_set_vhomogeneous (GTK_STACK (app.bottom_stack), FALSE);
    gtk_widget_set_hexpand (app.bottom_stack, TRUE);
    gtk_stack_add_named (GTK_STACK (app.bottom_stack), ready, "ready");
    gtk_stack_add_named (GTK_STACK (app.bottom_stack), running, "running");
    gtk_stack_add_named (GTK_STACK (app.bottom_stack), finished, "finished");

    /* batch controls */
    app.start_btn = icon_button ("media-playback-start-symbolic", "Start", "round",
                                 G_CALLBACK (on_start_all));
    gtk_widget_add_css_class (app.start_btn, "big");
    gtk_widget_add_css_class (app.start_btn, "suggested-action");
    app.pause_btn = icon_button ("media-playback-pause-symbolic", "Pause", "round",
                                 G_CALLBACK (on_pause_all));
    set_tip (app.pause_btn, "Pause", "Space",
             "Pause the current video. Resume continues from the same point.");
    app.stop_btn = icon_button ("media-playback-stop-symbolic", "Stop", "round",
                                G_CALLBACK (on_stop_all));
    set_tip (app.stop_btn, "Stop", "Esc",
             "Stop the batch. The unfinished file is removed; originals stay.");
    app.controls = hbox (10);
    gtk_widget_set_valign (app.controls, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (app.controls), app.start_btn);
    gtk_box_append (GTK_BOX (app.controls), app.pause_btn);
    gtk_box_append (GTK_BOX (app.controls), app.stop_btn);

    app.bottom = hbox (28);
    gtk_widget_add_css_class (app.bottom, "bottombar");
    gtk_box_append (GTK_BOX (app.bottom), app.bottom_stack);
    gtk_box_append (GTK_BOX (app.bottom), app.controls);
    return app.bottom;
}

static void
add_shortcut (GtkEventController *keys, const char *trigger, GtkShortcutFunc func)
{
    gtk_shortcut_controller_add_shortcut (
        GTK_SHORTCUT_CONTROLLER (keys),
        gtk_shortcut_new (gtk_shortcut_trigger_parse_string (trigger),
                          gtk_callback_action_new (func, NULL, NULL)));
}

static void
activate (GtkApplication *gapp, gpointer user_data)
{
    (void) user_data;
    if (app.win != NULL) {
        gtk_window_present (app.win);
        return;
    }
    g_autoptr (GtkCssProvider) css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (css, CSS);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (css),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    app.col_quality = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
    app.col_status = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
    app.col_actions = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
    for (guint i = 0; i < 3; i++)
        app.guide_cols[i] = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

    app.win = GTK_WINDOW (gtk_application_window_new (gapp));
    gtk_window_set_title (app.win, "Video Compressor");
    gtk_window_set_default_size (app.win, 1160, 720);
    g_signal_connect (app.win, "close-request", G_CALLBACK (on_close), NULL);

    app.title = styled_label ("Video Compressor", "heading", 0.5);
    app.subtitle = styled_label ("No folder selected", "caption", 0.5);
    gtk_label_set_ellipsize (GTK_LABEL (app.subtitle), PANGO_ELLIPSIZE_START);
    GtkWidget *title = vbox (0);
    gtk_widget_set_valign (title, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (title), app.title);
    gtk_box_append (GTK_BOX (title), app.subtitle);
    GtkWidget *header = gtk_header_bar_new ();
    gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), title);
    app.choose_btn = label_button ("folder-symbolic", "_Choose Folder…", G_CALLBACK (on_choose));
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app.choose_btn);
    gtk_window_set_titlebar (app.win, header);

    app.pages = gtk_stack_new ();
    gtk_widget_set_vexpand (app.pages, TRUE);
    gtk_stack_add_named (GTK_STACK (app.pages), build_empty_page (), "empty");
    gtk_stack_add_named (GTK_STACK (app.pages), build_list_page (), "list");
    GtkWidget *content_box = vbox (0);
    gtk_box_append (GTK_BOX (content_box), app.pages);
    gtk_box_append (GTK_BOX (content_box), build_bottom_bar ());
    gtk_window_set_child (app.win, content_box);

    GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect (drop, "drop", G_CALLBACK (on_drop), NULL);
    gtk_widget_add_controller (content_box, GTK_EVENT_CONTROLLER (drop));

    GtkEventController *keys = gtk_shortcut_controller_new ();
    add_shortcut (keys, "space", on_space);
    add_shortcut (keys, "Escape", on_escape);
    gtk_widget_add_controller (GTK_WIDGET (app.win), keys);

    update_speed_guide ();
    update_controls ();
    gtk_window_present (app.win);
    check_tools ();
}

int
main (int argc, char **argv)
{
    g_autoptr (GtkApplication) gapp = gtk_application_new ("io.github.dasantha.VideoCompressor",
                                                           G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (gapp, "activate", G_CALLBACK (activate), NULL);
    return g_application_run (G_APPLICATION (gapp), argc, argv);
}
