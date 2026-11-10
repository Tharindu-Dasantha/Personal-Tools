#include "modes.h"

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
    guint64 out_size;    /* frames: the folder's total */
    guint n_frames;      /* frames: files in `out` */
    char *detail;        /* images: "jpg 412 kB −58% · webp 288 kB −71%", or NULL */
    gboolean webp_only;  /* images: `out` is the webp; no same-type copy, so the original stays */
    int max_width;       /* images: the Max width the outputs were made at, for the trash check */
    gboolean stop;       /* Stop pressed while encoding or checking */
    char *note;          /* pill text while ST_NEW after a run ("Failed", "Stopped"), or NULL */
    char *tip;           /* tooltip for the size column */
    const char *extra;   /* after the meta line: "Original in Trash", "Original kept" */
    gboolean hover, in_batch;
    guint prio;          /* > 0: "Compress next" was pressed; the highest goes first */
    double fraction;     /* encode progress while active, over all steps */
    GPtrArray *steps;    /* VcStep list while active (from vc_mode_steps), else NULL */
    guint step;          /* the step running now */
    /* GPU sample: tried at these Max size / Max fps settings; kbps is 0 when it failed */
    gboolean sampled;
    int sample_side, sample_fps;
    double sample_kbps;
    GtkWidget *row, *box, *check, *meta, *chips, *size_lbl, *pill, *bar, *pct;
    GtkWidget *b_play, *b_pause, *b_stop, *b_show, *b_trash;
} Item;

typedef struct {
    GtkWindow *win;
    GtkWidget *title, *subtitle, *choose_btn, *files_btn, *pages, *list, *select_all;
    GtkWidget *summary, *sum_title, *sum_sub, *sum_before, *sum_after, *sum_pct;
    GtkWidget *bottom, *bottom_stack, *controls, *start_btn, *pause_btn, *stop_btn;
    GtkWidget *speed_btns[8], *speed_caption, *guide_rows, *subs, *trash, *total, *total_sub;
    GtkWidget *fps30, *max_size, *choose_empty, *files_empty, *empty_sub;
    GtkWidget *mode_dd, *banner, *size_head, *compress_opts, *web_opts, *web_width, *no_audio;
    GtkWidget *frames_opts, *frames_fps, *frames_start, *frames_end, *frames_width, *frames_format;
    GtkWidget *frames_quality, *images_opts, *images_width, *img_trash, *hero;
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

    /* the list source, kept so a mode switch can reload it: one folder, or chosen files */
    char *folder;
    char **files;
    GPtrArray *items;
    GCancellable *probe_cancel;
    guint probe_next, probes_done;   /* ffprobe started / finished, in list order */

    /* the one GPU size sample allowed at a time (sample_next has the limits) */
    GSubprocess *sample_proc;
    Item *sample_item;
    GCancellable *sample_cancel;
    guint sample_timer, samples_taken, sample_fails;
    int sample_side, sample_fps;   /* the settings samples_taken counts for */
    double sample_secs;
    gboolean closing;    /* window destroyed: late async callbacks must not touch widgets */
    gboolean was_busy;   /* a batch was running at the last update_controls */
    guint inhibit;       /* suspend inhibitor held while a batch runs, or 0 */

    /* the one encode allowed at a time (SPEC); a paused one still counts */
    Item *job;
    gboolean paused;
    gint64 job_t0, paused_at, paused_total;
    GSubprocess *proc;
    int pending;         /* the current step's stdout EOF + stderr EOF + process exit */
    char *last_err;
    guint pulse_id;      /* pulses the bar during a step without progress output */
    VcMode mode;
    guint have;          /* VC_HAVE_* bits from the startup tool check */
} App;

static App app;

static void update_controls (void);
static void sample_next (void);
static void render_row (Item *it);

static void
item_free (Item *it)
{
    g_free (it->path);
    g_free (it->out);
    g_free (it->note);
    g_free (it->tip);
    g_free (it->detail);
    vc_probe_free (it->probe);
    g_clear_pointer (&it->steps, g_ptr_array_unref);
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

/* The Mode dropdown, in order; only modes that exist are listed. */
static const VcMode MODES[] = { VC_MODE_COMPRESS, VC_MODE_WEB_VIDEO, VC_MODE_FRAMES, VC_MODE_IMAGES };
static const char *const MODE_NAMES[] = { "Compress video", "Web video (Safari)", "Video → frames",
                                          "Compress images", NULL };
/* The frames FPS dropdown, in order; 0 is Every frame */
static const int FRAME_RATES[] = { 5, 10, 12, 15, 24, 30, 0 };
#define FRAMES_DEFAULT_RATE 3   /* 15 */
/* The web Max width dropdown, in order; 0 is Original */
static const int WEB_WIDTHS[] = { 0, 3840, 2560, 1920, 1280, 960, 640 };
#define WEB_DEFAULT_WIDTH 3   /* 1920 */
G_STATIC_ASSERT (G_N_ELEMENTS (MODES) + 1 == G_N_ELEMENTS (MODE_NAMES));
/* The empty page's line, per Mode dropdown entry */
static const char *const EMPTY_SUB[] = {
    "Every video inside is compressed to H.265, keeping its quality and resolution.",
    "Every video inside becomes an MP4 that plays in every browser, Safari and iOS included.",
    "Every video inside becomes a folder of numbered WebP or JPG frames, for scroll animations.",
    "Every image inside gets a smaller copy in its own format, plus a WebP for the web.",
};
G_STATIC_ASSERT (G_N_ELEMENTS (EMPTY_SUB) == G_N_ELEMENTS (MODES));


/* Compress video and Compress images each have their own Move to Trash switch. */
static gboolean
trash_on (void)
{
    return vc_mode_can_trash (app.mode)
           && gtk_switch_get_active (GTK_SWITCH (app.mode == VC_MODE_IMAGES ? app.img_trash : app.trash));
}

/* "video" or "image", for counts and labels */
static const char *
noun (void)
{
    return app.mode == VC_MODE_IMAGES ? "image" : "video";
}

static int
percent_smaller (guint64 before, guint64 after)
{
    int p = (int) (100 - after * 100.0 / before + 0.5);
    return after > 0 ? MIN (p, 99) : p;   /* a 1.4 kB copy of 320 kB is not "−100%" */
}

/* "−63%" or, for a web copy that came out bigger, "+12%" */
static char *
change_text (guint64 before, guint64 after)
{
    return after <= before ? g_strdup_printf ("−%d%%", percent_smaller (before, after))
                           : g_strdup_printf ("+%d%%", (int) (after * 100.0 / before - 100 + 0.5));
}

/* The encode settings chosen in the bottom bar. */
static VcOptions
current_options (void)
{
    static const int sizes[] = { 0, 1080, 720 };   /* the Max size dropdown, in order */
    guint size = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.max_size));
    GtkWidget *width_dd = app.mode == VC_MODE_FRAMES ? app.frames_width
                          : app.mode == VC_MODE_IMAGES ? app.images_width : app.web_width;
    guint width = gtk_drop_down_get_selected (GTK_DROP_DOWN (width_dd));
    guint rate = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.frames_fps));
    double start, end;   /* 0 when invalid; check_range() greys Start then */
    vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_start)), &start);
    vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_end)), &end);
    return (VcOptions) {
        .preset = current_preset (),
        .keep_subs = gtk_switch_get_active (GTK_SWITCH (app.subs)),
        .max_short_side = size < G_N_ELEMENTS (sizes) ? sizes[size] : 0,
        .max_fps = gtk_switch_get_active (GTK_SWITCH (app.fps30)) ? 30 : 0,
        .max_width = width < G_N_ELEMENTS (WEB_WIDTHS) ? WEB_WIDTHS[width] : 0,
        .no_audio = gtk_switch_get_active (GTK_SWITCH (app.no_audio)),
        .fps = rate < G_N_ELEMENTS (FRAME_RATES) ? FRAME_RATES[rate] : 0,
        .start = start,
        .end = end,
        .webp = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.frames_format)) == 0,
        .quality = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (app.frames_quality)),
    };
}

/* Frames: TRUE when Start and End parse and End (when set) is after Start. Turns a bad field red. */
static gboolean
check_range (void)
{
    double start, end;
    gboolean s_ok = vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_start)), &start);
    gboolean e_ok = vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_end)), &end)
                    && (end == 0 || !s_ok || end > start);
    GtkWidget *w[] = { app.frames_start, app.frames_end };
    gboolean ok[] = { s_ok, e_ok };
    for (guint i = 0; i < 2; i++) {
        if (ok[i])
            gtk_widget_remove_css_class (w[i], "error");
        else
            gtk_widget_add_css_class (w[i], "error");
    }
    return s_ok && e_ok;
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

static gboolean
sampled_at (Item *it, const VcOptions *o)
{
    return it->sampled && it->sample_side == o->max_short_side && it->sample_fps == o->max_fps;
}

/* The size estimate: from the GPU sample taken at these settings when there is one, else the
 * formula. `measured` (may be NULL) says which. */
static guint64
item_estimate (Item *it, const VcOptions *o, gboolean *measured)
{
    gboolean m = it->probe != NULL && sampled_at (it, o) && it->sample_kbps > 0;
    if (measured != NULL)
        *measured = m;
    if (it->probe == NULL)
        return 0;
    return m ? vc_estimate_from_sample (it->probe, it->size, o, it->sample_kbps)
             : vc_estimate_size (it->probe, it->size, o);
}

/* Start with nothing selected leaves videos predicted to save nothing (SPEC D2); selecting one,
 * or its own play button, still compresses it. */
static gboolean
skip_in_bulk (Item *it)
{
    VcOptions o = current_options ();
    return app.mode == VC_MODE_COMPRESS && it->state == ST_NEW && it->probe != NULL
           && !vc_worth_encoding (it->size, item_estimate (it, &o, NULL));
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
    return it->state == ST_DONE && it->out != NULL && !it->webp_only && vc_mode_can_trash (app.mode);
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
            pill = g_strdup (it->note != NULL ? it->note : "Already efficient");
            tip = tip != NULL ? tip : "A compressed copy would not be at least 5% smaller, "
                                      "so none was kept.";
        } else {
            g_autofree char *out = g_format_size (it->out_size);
            size_text = g_markup_printf_escaped ("<span alpha=\"60%%\">%s →</span> <b>%s</b>",
                                                 size, out);
            g_autofree char *change = change_text (it->size, it->out_size);
            pill = g_strdup_printf ("✓ %s", change);
            style = "done";
        }
        if (it->out != NULL && it->detail != NULL) {   /* images: each output on its own */
            g_free (size_text);
            size_text = g_markup_escape_text (it->detail, -1);
        }
        if (it->out != NULL && app.mode == VC_MODE_FRAMES) {
            g_autofree char *total = g_format_size (it->out_size);
            g_free (size_text);
            size_text = g_markup_printf_escaped ("%u frames · <b>%s</b>", it->n_frames, total);
            g_free (pill);
            pill = g_strdup ("✓ Done");
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
        } else if (app.mode == VC_MODE_FRAMES && it->probe != NULL) {
            VcOptions o = current_options ();
            guint n = vc_frames_estimate (it->probe->duration, it->probe->fps, &o);
            size_text = g_strdup (size);
            if (it->probe->duration > 0 && o.start >= it->probe->duration)
                pill = g_strdup ("Starts after the end");
            else if (n > 0)
                pill = g_strdup_printf ("≈ %u frame%s", n, n == 1 ? "" : "s");
            style = "estimate";
            tip = tip != NULL ? tip : "≈ at the chosen FPS and range.";
        } else {
            VcOptions o = current_options ();
            gboolean measured;
            guint64 est = app.mode == VC_MODE_COMPRESS ? item_estimate (it, &o, &measured) : 0;
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
            tip = tip != NULL ? tip
                  : measured ? "≈ measured with a short GPU test encode at these settings. "
                               "The whole video can differ a little."
                  : "≈ is a rough estimate at the chosen speed. "
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
                               : it->probe->duration <= 0 || app.mode == VC_MODE_IMAGES ? g_strdup ("")
                               : g_strdup_printf ("%d%%", (int) (it->fraction * 100));
        gtk_label_set_text (GTK_LABEL (it->pct), pct);
        if (app.paused)
            gtk_widget_add_css_class (it->bar, "paused");
        else
            gtk_widget_remove_css_class (it->bar, "paused");
    }

    GString *m = g_string_new (NULL);
    if (it->probe != NULL && app.mode == VC_MODE_IMAGES) {
        g_autofree char *codec = vc_codec_label (it->probe->codec);
        g_string_printf (m, "%s · %d×%d", codec, it->probe->width, it->probe->height);
    } else if (it->probe != NULL) {
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
    if (p == NULL || app.mode == VC_MODE_IMAGES)   /* video quality classes */
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
            est[p] += item_estimate (it, &o, NULL);
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
        if (app.mode != VC_MODE_FRAMES && it->out != NULL && it->out_size < it->size)   /* a web copy can be bigger */
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
                                         app.paused ? "Paused ·"
                                         : app.mode == VC_MODE_COMPRESS || app.mode == VC_MODE_IMAGES
                                             ? "Compressing"
                                         : app.mode == VC_MODE_FRAMES ? "Extracting" : "Converting",
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
    const char *originals = trash_on () ? "riginals go to Trash after a check" : "riginals are untouched";
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
    gboolean web = app.mode == VC_MODE_WEB_VIDEO, frames = app.mode == VC_MODE_FRAMES;
    g_autofree char *saved = g_format_size (before > after ? before - after : 0);
    g_autofree char *title = frames ? g_strdup_printf ("Frames from %u video%s ready", compressed,
                                                       compressed == 1 ? "" : "s")
                             : web ? g_strdup_printf ("%u web video%s ready", compressed,
                                                    compressed == 1 ? "" : "s")
                             : compressed > 0 ? g_strdup_printf ("You saved %s", saved)
                             : g_strdup ("Nothing to compress");
    gtk_label_set_text (GTK_LABEL (app.sum_title), title);

    GString *s = g_string_new (NULL);
    g_string_printf (s, "%u %s%s %s", compressed, noun (), compressed == 1 ? "" : "s",
                     web ? "converted" : frames ? "extracted" : "compressed");
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
        g_string_append_printf (s, " · %u %s", efficient, frames ? "skipped"
                                : app.mode == VC_MODE_IMAGES ? "already optimized" : "already efficient");
    if (unreadable > 0)
        g_string_append_printf (s, " · %u unreadable", unreadable);
    gtk_label_set_text (GTK_LABEL (app.sum_sub), s->str);
    g_string_free (s, TRUE);

    g_autofree char *b = g_format_size (before), *a = g_format_size (after);
    g_autofree char *pct = before > 0 ? change_text (before, after) : g_strdup ("–");
    gtk_label_set_text (GTK_LABEL (app.sum_before), b);
    gtk_label_set_text (GTK_LABEL (app.sum_after), a);
    gtk_label_set_text (GTK_LABEL (app.sum_pct), pct);
}

static void
notify (const char *id, const char *title, const char *body)
{
    g_autoptr (GNotification) n = g_notification_new (title);
    g_notification_set_body (n, body);
    /* no default action: a click activates the app, and activate() shows the window */
    g_application_send_notification (g_application_get_default (), id, n);
}

/* A batch may run with the window closed to the background or behind other windows: keep the
 * computer from sleeping through it, and say when it ends. */
static void
on_busy_changed (gboolean busy)
{
    GtkApplication *gapp = GTK_APPLICATION (g_application_get_default ());
    app.was_busy = busy;
    if (busy) {
        app.inhibit = gtk_application_inhibit (gapp, app.win, GTK_APPLICATION_INHIBIT_SUSPEND,
                                               "Compressing videos");
        return;
    }
    if (app.inhibit != 0)
        gtk_application_uninhibit (gapp, app.inhibit);
    app.inhibit = 0;
    g_application_withdraw_notification (G_APPLICATION (gapp), "background");
    if (gtk_window_is_active (app.win))
        return;
    guint failed = 0;
    for (guint i = 0; i < app.items->len; i++)
        failed += g_strcmp0 (((Item *) app.items->pdata[i])->note, "Failed") == 0;
    update_summary ();
    const char *sub = gtk_label_get_text (GTK_LABEL (app.sum_sub));
    g_autofree char *body = failed == 0 ? g_strdup (sub)
                                        : g_strdup_printf ("%s · %u failed", sub, failed);
    notify ("done", gtk_label_get_text (GTK_LABEL (app.sum_title)), body);
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
            guint64 e = item_estimate (it, &o, NULL);
            ready++;
            src += it->size;
            est += e > 0 ? e : it->size;
        }
    }
    gboolean finished = n > 0 && !busy && !start && any_done;
    if (busy != app.was_busy)
        on_busy_changed (busy);

    gboolean loaded = app.items != NULL;
    gtk_stack_set_visible_child_name (GTK_STACK (app.pages), loaded ? "list" : "empty");
    gtk_widget_set_visible (app.bottom, loaded);
    gtk_stack_set_visible_child_name (GTK_STACK (app.bottom_stack),
                                      busy ? "running" : finished ? "finished" : "ready");
    gtk_widget_set_visible (app.controls, !finished);
    gtk_widget_set_visible (app.summary, finished);
    gtk_widget_set_visible (app.choose_btn, loaded && !busy && !finished);
    gtk_widget_set_visible (app.files_btn, loaded && !busy && !finished);
    gtk_widget_set_sensitive (app.choose_btn, app.tools_ok && !busy);
    gtk_widget_set_sensitive (app.files_btn, app.tools_ok && !busy);
    gtk_widget_set_sensitive (app.choose_empty, app.tools_ok);
    gtk_widget_set_sensitive (app.files_empty, app.tools_ok);
    gtk_widget_set_sensitive (app.mode_dd, !busy);

    /* Shown once the tool check is done; this mode can't start without the tool. */
    const char *missing = app.tools_ok ? vc_mode_missing_tool (app.mode, app.have) : NULL;
    gtk_widget_set_visible (app.banner, missing != NULL);
    if (missing != NULL) {
        const char *mode_name = MODE_NAMES[gtk_drop_down_get_selected (GTK_DROP_DOWN (app.mode_dd))];
        g_autofree char *b = app.mode == VC_MODE_IMAGES
            ? g_strdup_printf ("%s needs %s: sudo dnf install %s", mode_name, missing,
                               strcmp (missing, "magick") == 0 ? "ImageMagick" : missing)
            : g_strdup_printf ("%s needs ffmpeg with %s, which this ffmpeg doesn't "
                               "have. Install the full ffmpeg build from RPM Fusion.", mode_name, missing);
        gtk_label_set_text (GTK_LABEL (app.banner), b);
    }
    gboolean range_ok = check_range ();
    gboolean can_run = app.tools_ok && missing == NULL && !probing
                       && (app.mode != VC_MODE_FRAMES || range_ok);

    gtk_widget_set_sensitive (app.start_btn, can_run && start
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
    gtk_widget_set_sensitive (app.web_width, !busy);
    gtk_widget_set_sensitive (app.no_audio, !busy);
    gtk_widget_set_sensitive (app.frames_opts, !busy);
    gtk_widget_set_sensitive (app.images_opts, !busy);

    g_autofree char *src_text = g_format_size (src), *est_text = g_format_size (est);
    gboolean est_shown = app.mode == VC_MODE_COMPRESS;
    g_autofree char *total = ready == 0 || !est_shown ? g_strdup ("")
                             : g_strdup_printf ("%s → ≈ %s", src_text, est_text);
    g_autofree char *preset = preset_title (app.preset);
    g_autofree char *total_sub = probing ? g_strdup_printf ("Reading %ss…", noun ())
                                 : ready == 0 && skipped > 0
                                     ? g_strdup_printf ("%u video%s likely won't get smaller · "
                                                        "select to compress anyway", skipped,
                                                        skipped == 1 ? "" : "s")
                                 : ready == 0 ? g_strdup ("Nothing waiting to compress")
                                 : !est_shown ? g_strdup_printf ("%u %s%s ready", ready, noun (),
                                                                 ready == 1 ? "" : "s")
                                 : g_strdup_printf ("%u video%s ready · estimated for %s", ready,
                                                    ready == 1 ? "" : "s", preset);
    gtk_label_set_text (GTK_LABEL (app.total), total);
    gtk_label_set_text (GTK_LABEL (app.total_sub), total_sub);

    gtk_widget_set_visible (app.selbar, nsel > 0);
    g_autofree char *sel = g_strdup_printf ("%u selected", nsel);
    gtk_label_set_text (GTK_LABEL (app.sel_label), sel);
    gtk_widget_set_sensitive (app.sel_start, can_run && s_start);
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
    sample_next ();
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
        g_autofree char *out_dir = g_build_filename (dir, vc_mode_out_dir (app.mode), NULL);
        if (g_mkdir_with_parents (out_dir, 0755) != 0) {
            alert ("Cannot create the output folder", g_strerror (errno), NULL);
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

static const char *
trash_checks (void)
{
    return app.mode == VC_MODE_IMAGES
               ? "\n\n✓ The same-type copy opens and has the expected size\n"
                 "✓ It is at least 5% smaller than the original\n"
                 "✓ Nothing is ever deleted permanently"
               : "\n\n✓ The compressed copy plays and has the same length\n"
                 "✓ It is at least 5% smaller than the original\n"
                 "✓ Nothing is ever deleted permanently";
}

/* The output that replaces the original: the compressed video, or an image's same-type copy. */
static char *
main_output (Item *it, const VcOptions *o)
{
    return app.mode == VC_MODE_IMAGES ? vc_image_outputs (it->path, it->probe, NULL)
                                      : vc_output_path (it->path, o->keep_subs, NULL);
}

static void
act_start (Item *one, gboolean sel)
{
    guint n = 0;
    VcOptions o = current_options ();
    for (guint i = 0; trash_on () && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (!in_scope (it, one, sel) || !can_start (it) || it == app.job
            || (one == NULL && !sel && skip_in_bulk (it)))
            continue;
        g_autofree char *out = main_output (it, &o);
        n += !g_file_test (out, G_FILE_TEST_EXISTS);
    }
    if (n == 0) {
        do_start (one, sel);
        return;
    }
    app.ask_one = one;
    app.ask_sel = sel;
    g_autofree char *message = g_strdup_printf ("Move up to %u original%s to Trash?", n,
                                                n == 1 ? "" : "s");
    g_autofree char *detail = g_strconcat ("Each original is moved only after its compressed copy is "
                                           "ready and checked. You can restore them from the Trash.",
                                           trash_checks (), NULL);
    confirm (message, detail, "Compress and Move to Trash", start_confirmed);
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
    gboolean pass;
    if (app.mode == VC_MODE_IMAGES) {
        int w, h, tw, th, ow = probe != NULL ? probe->width : 0, oh = probe != NULL ? probe->height : 0;
        vc_image_expected_size (it->probe->width, it->probe->height, it->max_width, &w, &h);
        /* ffprobe gives a JPEG's stored size; with EXIF orientation 5-8 the upright copy is transposed */
        vc_image_expected_size (it->probe->height, it->probe->width, it->max_width, &tw, &th);
        pass = vc_image_should_trash (TRUE, TRUE, kept, w, h, ow, oh)
               || vc_image_should_trash (TRUE, TRUE, kept, tw, th, ow, oh);
    } else {
        pass = vc_should_trash_original (TRUE, TRUE, it->probe->duration,
                                         probe != NULL ? probe->duration : 0, kept);
    }
    if (!pass) {
        g_autofree char *why = g_strdup_printf (
            "Output check failed: %s", error != NULL ? error->message
                                       : !kept ? "output missing or not smaller"
                                       : app.mode == VC_MODE_IMAGES ? "size differs" : "duration differs");
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

/* Total bytes of the files in a frames folder, and how many there are. */
static guint64
dir_usage (const char *path, guint *count)
{
    guint64 total = 0;
    *count = 0;
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    for (const char *name; dir != NULL && (name = g_dir_read_name (dir)) != NULL;) {
        g_autofree char *f = g_build_filename (path, name, NULL);
        GStatBuf st;
        if (g_stat (f, &st) == 0) {
            total += st.st_size;
            (*count)++;
        }
    }
    return total;
}

/* Images: the row's "jpg 412 kB −58% · webp 288 kB −71%", from what is on disk now. `out` becomes the
 * same-type copy, else the webp, else NULL. */
static void
image_result (Item *it, GPtrArray *steps)
{
    g_autofree char *webp = NULL;
    g_autofree char *same = vc_image_outputs (it->path, it->probe, &webp);
    const char *outs[] = { same, webp };
    GString *d = g_string_new (NULL);
    g_clear_pointer (&it->out, g_free);
    it->out_size = 0;
    for (guint i = 0; i < G_N_ELEMENTS (outs) && outs[i] != NULL; i++) {
        gboolean planned = FALSE;   /* else it existed before this run */
        for (guint s = 0; s < steps->len; s++)
            planned |= g_strcmp0 (((VcStep *) steps->pdata[s])->output, outs[i]) == 0;
        GStatBuf st;
        gboolean exists = g_stat (outs[i], &st) == 0;
        const char *ext = strrchr (outs[i], '.') + 1;
        if (d->len > 0)
            g_string_append (d, " · ");
        if (!exists) {
            g_string_append_printf (d, "%s already optimized", ext);
        } else if (!planned) {
            g_string_append_printf (d, "%s exists", ext);
        } else {
            g_autofree char *size = g_format_size (st.st_size);
            g_autofree char *change = change_text (it->size, st.st_size);
            g_string_append_printf (d, "%s %s %s", ext, size, change);
        }
        if (exists && it->out == NULL) {
            it->out = g_strdup (outs[i]);
            it->out_size = st.st_size;
        }
    }
    g_free (it->detail);
    it->detail = g_string_free (d, FALSE);
    /* only the same-type copy may replace the original (SPEC-image-compress.md D6) */
    it->webp_only = it->out != NULL && strcmp (it->out, same) != 0;
    if (it->webp_only && trash_on ())
        it->extra = g_str_has_suffix (same, ".png") ? "Original kept (no png copy)"
                                                    : "Original kept (no jpg copy)";
}

/* Runs when the job's last step has ended (or one failed, or it was stopped): moves the outputs
 * into place, removes every other part, and sets the row. The caller starts the next item. */
static void
finish_job (gboolean ok)
{
    Item *it = g_steal_pointer (&app.job);
    g_autoptr (GPtrArray) steps = g_steal_pointer (&it->steps);
    g_autofree char *last_err = g_steal_pointer (&app.last_err);
    g_clear_handle_id (&app.pulse_id, g_source_remove);
    app.paused = FALSE;
    app.batch_end = g_get_monotonic_time ();
    it->state = ST_NEW;

    g_autoptr (GError) error = NULL;
    g_autofree char *kept = NULL;
    guint64 kept_size = 0;
    for (guint i = 0; ok && !it->stop && error == NULL && i < steps->len; i++) {
        VcStep *s = steps->pdata[i];
        GStatBuf st;
        /* A compressed output (each image output on its own) that isn't at least 5% smaller is
         * dropped with the other parts below; web video keeps every output (its point is
         * compatibility, SPEC-web-video.md D2). */
        gboolean shrinks = app.mode == VC_MODE_COMPRESS || app.mode == VC_MODE_IMAGES;
        if (s->output == NULL || g_stat (s->part, &st) != 0
            || (shrinks && !vc_output_is_worth_keeping (it->size, st.st_size)))
            continue;
        g_autoptr (GFile) src = g_file_new_for_path (s->part);
        g_autoptr (GFile) dst = g_file_new_for_path (s->output);
        /* No OVERWRITE flag: an existing output is never replaced. */
        if (g_file_move (src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
            copy_mtime (it->path, s->output);
            g_free (kept);
            kept = g_strdup (s->output);
            kept_size = s->part_is_dir ? dir_usage (s->output, &it->n_frames) : (guint64) st.st_size;
        }
    }
    for (guint i = 0; i < steps->len; i++)   /* moved parts are already gone */
        vc_remove_part (((VcStep *) steps->pdata[i])->part);

    if (it->stop) {
        it->stop = FALSE;
        set_status (it, "Stopped", NULL);
    } else if (!ok || error != NULL) {
        set_status (it, "Failed", error != NULL ? error->message
                                  : last_err != NULL ? last_err : "ffmpeg failed");
    } else if (app.mode == VC_MODE_IMAGES) {
        it->state = ST_DONE;
        image_result (it, steps);
        if (it->out == NULL) {
            set_status (it, "Skipped – already optimized",
                        "No copy was at least 5% smaller than the original, so none was kept.");
        } else if (it->webp_only) {
            set_status (it, NULL, it->out);   /* image_result said why the original stays */
        } else {
            set_status (it, NULL, it->out);
            if (trash_on ())
                check_then_trash (it);
        }
    } else if (kept == NULL) {
        it->state = ST_DONE;   /* no output: nothing to trash, and no point encoding again */
        set_status (it, NULL, NULL);
    } else {
        it->state = ST_DONE;
        g_free (it->out);
        it->out = g_steal_pointer (&kept);
        it->out_size = kept_size;
        set_status (it, NULL, it->out);
        if (trash_on ())
            check_then_trash (it);   /* runs alongside the next encode */
    }
}

static gboolean spawn_step (void);

/* All three events of the current step are in: start the next step, or finish the job. */
static void
step_ended (void)
{
    Item *it = app.job;
    VcStep *s = it->steps->pdata[it->step];
    g_autoptr (GSubprocess) proc = g_steal_pointer (&app.proc);
    int code = g_subprocess_get_if_exited (proc) ? g_subprocess_get_exit_status (proc) : -1;
    /* Exit 0 must leave the part behind; a soft exit means "no output" and is fine. */
    gboolean ok = (code == 0 && g_file_test (s->part, G_FILE_TEST_EXISTS))
                  || (s->soft_exit != 0 && code == s->soft_exit);
    if (ok && !it->stop && it->step + 1 < it->steps->len) {
        it->step++;
        it->fraction = (double) it->step / it->steps->len;   /* steps without progress output */
        update_progress ();
        if (spawn_step ())
            return;
        ok = FALSE;
    }
    finish_job (ok);
    encode_next ();
}

static void
job_step_done (void)
{
    if (--app.pending == 0 && !app.closing)   /* on_close already removed the parts */
        step_ended ();
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
        VcStep *s = it->steps->pdata[it->step];
        double fraction, dur = s->duration > 0 ? s->duration : it->probe->duration;
        if (s->progress && vc_parse_progress_line (line, dur, &fraction)) {
            it->fraction = (it->step + fraction) / it->steps->len;
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), it->fraction);
            if (!it->stop) {
                g_autofree char *pct = g_strdup_printf ("%d%%", (int) (it->fraction * 100));
                gtk_label_set_text (GTK_LABEL (it->pct), pct);
            }
            update_progress ();
        } else if (s->progress && dur <= 0 && g_str_has_prefix (line, "progress=")) {
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

static gboolean
pulse_bar (gpointer data)
{
    (void) data;
    if (app.closing || app.job == NULL || app.paused)
        return G_SOURCE_CONTINUE;   /* finish_job and stop_for_quit remove it */
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (app.job->bar));
    return G_SOURCE_CONTINUE;
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

/* Starts app.job's current step. FALSE (with app.last_err set) when it can't be started. */
static gboolean
spawn_step (void)
{
    Item *it = app.job;
    VcStep *s = it->steps->pdata[it->step];
    g_autoptr (GError) error = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup (launcher, lower_priority, NULL, NULL);
    g_clear_pointer (&app.last_err, g_free);
    /* A killed run may have left the part (pngquant won't overwrite one). ffmpeg writes frames
     * into the folder but doesn't create it. */
    if (!vc_remove_part (s->part) || (s->part_is_dir && g_mkdir (s->part, 0755) != 0)) {
        g_autofree char *display = g_filename_display_name (s->part);
        app.last_err = g_strdup_printf ("Cannot create %s", display);
        return FALSE;
    }
    app.proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) s->argv, &error);
    if (app.proc == NULL) {
        app.last_err = g_strdup (error->message);
        return FALSE;
    }
    if (app.paused)   /* paused just as the previous step ended */
        g_subprocess_send_signal (app.proc, SIGSTOP);
    app.pending = 3;
    if (!s->progress && app.pulse_id == 0)
        app.pulse_id = g_timeout_add (100, pulse_bar, NULL);
    else if (s->progress)
        g_clear_handle_id (&app.pulse_id, g_source_remove);

    GDataInputStream *so = g_data_input_stream_new (g_subprocess_get_stdout_pipe (app.proc));
    GDataInputStream *se = g_data_input_stream_new (g_subprocess_get_stderr_pipe (app.proc));
    g_data_input_stream_read_line_async (so, G_PRIORITY_DEFAULT, NULL, on_progress_line, NULL);
    g_data_input_stream_read_line_async (se, G_PRIORITY_DEFAULT, NULL, on_stderr_line, NULL);
    g_subprocess_wait_async (app.proc, NULL, on_ffmpeg_exit, NULL);
    return TRUE;
}

/* Takes `steps`. The caller starts the next item if this one can't start. */
static void
start_job (Item *it, GPtrArray *steps)
{
    app.job = it;
    app.job_t0 = g_get_monotonic_time ();
    app.paused_total = 0;
    it->steps = steps;
    it->step = 0;
    it->state = ST_ACTIVE;
    it->stop = FALSE;
    it->fraction = 0;
    g_clear_pointer (&it->detail, g_free);
    it->webp_only = FALSE;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), 0);
    set_status (it, NULL, NULL);
    if (!spawn_step ())
        finish_job (FALSE);
}

/* ---- GPU size samples ---- */

/* Limits, so a big folder never costs much: one sample at a time at low priority, only while
 * nothing is queued, encoding or being checked, biggest videos first, at most SAMPLE_MAX per
 * folder and settings (the rest keep the formula), each killed after SAMPLE_TIMEOUT, and none
 * after 3 failures in a row. Changing folder or closing the window cancels the one in flight. */
#define SAMPLE_MAX 24
#define SAMPLE_TIMEOUT 15

static void
sample_stop (void)
{
    g_clear_handle_id (&app.sample_timer, g_source_remove);
    g_cancellable_cancel (app.sample_cancel);   /* sample_done then leaves the item alone */
    g_clear_object (&app.sample_cancel);
    if (app.sample_proc != NULL) {
        g_subprocess_force_exit (app.sample_proc);
        g_subprocess_wait (app.sample_proc, NULL, NULL);
        g_clear_object (&app.sample_proc);
    }
    app.sample_item = NULL;
}

static void
sample_timed_out (gpointer data)
{
    (void) data;
    app.sample_timer = 0;
    if (app.sample_proc != NULL)
        g_subprocess_force_exit (app.sample_proc);   /* sample_done counts it as a failure */
}

static void
sample_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GBytes) out = NULL;
    g_autoptr (GError) error = NULL;
    gboolean ok = g_subprocess_communicate_finish (G_SUBPROCESS (source), res, &out, NULL, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;   /* sample_stop ran: the folder changed or the window closed */
    g_clear_handle_id (&app.sample_timer, g_source_remove);
    Item *it = g_steal_pointer (&app.sample_item);
    gsize n = out != NULL ? g_bytes_get_size (out) : 0;
    ok = ok && g_subprocess_get_successful (G_SUBPROCESS (source)) && n > 0;
    g_clear_object (&app.sample_proc);

    it->sampled = TRUE;
    it->sample_side = app.sample_side;
    it->sample_fps = app.sample_fps;
    it->sample_kbps = ok ? n * 8.0 / 1000.0 / app.sample_secs : 0;
    app.sample_fails = ok ? 0 : app.sample_fails + 1;
    render_row (it);
    update_speed_guide ();
    update_controls ();   /* which starts the next one */
}

static void
sample_next (void)
{
    if (!app.gpu_ok || app.closing || app.mode != VC_MODE_COMPRESS || app.items == NULL
        || app.sample_proc != NULL
        || app.probes_done < app.items->len || app.job != NULL || is_busy ())
        return;
    VcOptions o = current_options ();
    if (o.max_short_side != app.sample_side || o.max_fps != app.sample_fps) {
        app.sample_side = o.max_short_side;   /* new settings: a fresh allowance */
        app.sample_fps = o.max_fps;
        app.samples_taken = app.sample_fails = 0;
    }
    if (app.samples_taken >= SAMPLE_MAX || app.sample_fails >= 3)
        return;
    Item *best = NULL;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state == ST_NEW && it->probe != NULL && it->probe->duration > 0
            && !sampled_at (it, &o) && (best == NULL || it->size > best->size))
            best = it;
    }
    if (best == NULL)
        return;

    g_auto (GStrv) argv = vc_build_sample_argv (best->path, best->probe, &o, &app.sample_secs);
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_child_setup (launcher, lower_priority, NULL, NULL);
    app.samples_taken++;
    app.sample_proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) argv, NULL);
    if (app.sample_proc == NULL) {
        best->sampled = TRUE;   /* kbps 0: it keeps the formula */
        best->sample_side = o.max_short_side;
        best->sample_fps = o.max_fps;
        app.sample_fails++;
        sample_next ();   /* ends after 3 failures */
        return;
    }
    app.sample_item = best;
    if (app.sample_cancel == NULL)
        app.sample_cancel = g_cancellable_new ();
    app.sample_timer = g_timeout_add_seconds_once (SAMPLE_TIMEOUT, sample_timed_out, NULL);
    g_subprocess_communicate_async (app.sample_proc, NULL, app.sample_cancel, sample_done, NULL);
}

/* Starts the next queued item unless an encode (even a paused one) is running: the one with
 * the highest "Compress next" priority, otherwise the first in list order. */
static void
encode_next (void)
{
    VcOptions o = current_options ();
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
        it->max_width = o.max_width;
        g_autofree char *reason = NULL;
        GPtrArray *steps = vc_mode_steps (app.mode, it->path, it->probe, &o, &reason);
        if (steps == NULL && app.mode == VC_MODE_FRAMES) {
            it->state = ST_DONE;
            g_clear_pointer (&it->out, g_free);
            if (g_strcmp0 (reason, "starts after the end") == 0) {
                set_status (it, "Starts after the end", "Start is past this video's end, so it was skipped.");
                continue;
            }
            it->out = vc_frames_output (it->path, NULL);
            it->out_size = dir_usage (it->out, &it->n_frames);
            it->extra = "Already extracted";
            set_status (it, NULL, reason);
            continue;
        }
        if (steps == NULL) {
            it->state = ST_DONE;
            g_free (it->out);
            /* an earlier finished output, so its original can be trashed */
            it->out = app.mode == VC_MODE_WEB_VIDEO ? vc_web_video_output (it->path, NULL)
                                                    : main_output (it, &o);
            GStatBuf st;
            it->out_size = g_stat (it->out, &st) == 0 ? st.st_size : 0;
            it->extra = app.mode == VC_MODE_WEB_VIDEO ? "Already converted" : "Already compressed";
            set_status (it, NULL, reason);
            continue;
        }
        start_job (it, steps);   /* takes steps */
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
    g_autofree char *detail = g_strconcat ("Each compressed copy is checked first. You can restore the "
                                           "originals from the Trash.", trash_checks (), NULL);
    confirm (message, detail, "Move to Trash", trash_confirmed);
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
    g_autofree char *base = app.folder != NULL ? g_strdup (app.folder)
                            : g_path_get_dirname (((Item *) app.items->pdata[0])->path);
    g_autofree char *dir = g_build_filename (base, vc_mode_out_dir (app.mode), NULL);
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
    GtkWidget *tile = gtk_image_new_from_icon_name (app.mode == VC_MODE_IMAGES ? "image-x-generic-symbolic"
                                                    : "media-playback-start-symbolic");
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

/* ---- choosing a folder or files ---- */

/* A run killed from outside (logout, crash) leaves its hidden temp file behind. Only this app
 * writes `.<name>.part.<ext>` there, and only one instance runs, so none is in use now. */
static void
remove_stale_parts (const char *folder)
{
    g_autofree char *cdir_path = g_build_filename (folder, vc_mode_out_dir (app.mode), NULL);
    g_autoptr (GDir) cdir = g_dir_open (cdir_path, 0, NULL);
    for (const char *name; cdir != NULL && (name = g_dir_read_name (cdir)) != NULL;) {
        static const char *const ends[] = { ".part.mp4", ".part.mkv", ".part" /* a frames folder */,
                                            ".part.jpg", ".part.png", ".part.webp" };
        gboolean part = FALSE;
        for (gsize i = 0; i < G_N_ELEMENTS (ends); i++)
            part |= g_str_has_suffix (name, ends[i]);
        if (name[0] == '.' && part) {
            g_autofree char *stale = g_build_filename (cdir_path, name, NULL);
            vc_remove_part (stale);
        }
    }
}

static void
add_item (const char *path)
{
    GStatBuf st;
    if (g_stat (path, &st) != 0 || !S_ISREG (st.st_mode))
        return;
    Item *it = g_new0 (Item, 1);
    it->path = g_strdup (path);
    it->size = st.st_size;
    g_ptr_array_add (app.items, it);
}

/* Lists one folder (`files` NULL) or the chosen files (`folder` NULL), replacing the list. */
static void
load_source (const char *folder, char *const *files)
{
    g_cancellable_cancel (app.probe_cancel);
    g_clear_object (&app.probe_cancel);
    app.probe_cancel = g_cancellable_new ();
    sample_stop ();   /* before the items it points at are freed */
    app.samples_taken = app.sample_fails = 0;
    gtk_list_box_remove_all (GTK_LIST_BOX (app.list));
    g_clear_pointer (&app.items, g_ptr_array_unref);
    app.items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
    app.probe_next = app.probes_done = 0;
    app.anchor = NULL;
    app.batch_t0 = app.batch_end = 0;
    char *new_folder = g_strdup (folder);   /* copies first: a reload passes app.folder/app.files */
    char **new_files = g_strdupv ((char **) files);
    g_free (app.folder);
    g_strfreev (app.files);
    app.folder = new_folder;
    app.files = new_files;
    folder = app.folder;   /* the argument may be the old app.folder, freed just above */

    if (app.files != NULL) {
        guint hidden = 0;
        g_autofree char *last_dir = NULL;
        for (char **f = app.files; *f != NULL; f++) {
            g_autofree char *dir = g_path_get_dirname (*f);
            g_autofree char *base = g_path_get_basename (*f);
            if (g_strcmp0 (dir, last_dir) != 0)   /* the dialog lists one folder's files together */
                remove_stale_parts (dir);
            g_free (last_dir);
            last_dir = g_steal_pointer (&dir);
            if (vc_mode_accepts (app.mode, base))
                add_item (*f);
            else
                hidden++;
        }
        guint n = app.items->len;
        g_autofree char *sub = hidden == 0
            ? g_strdup_printf ("%u file%s", n, n == 1 ? "" : "s")
            : g_strdup_printf ("%u file%s · %u hidden (not %ss)", n, n == 1 ? "" : "s", hidden, noun ());
        gtk_label_set_text (GTK_LABEL (app.title), "Chosen files");
        gtk_label_set_text (GTK_LABEL (app.subtitle), sub);
    } else {
        g_autofree char *base = basename_display (folder);
        g_autofree char *display = g_filename_display_name (folder);
        const char *home = g_get_home_dir ();
        gsize hl = strlen (home);
        g_autofree char *sub = strncmp (display, home, hl) == 0 && (display[hl] == '/' || !display[hl])
                                   ? g_strconcat ("~", display + hl, NULL)
                                   : g_strdup (display);
        gtk_label_set_text (GTK_LABEL (app.title), base);
        gtk_label_set_text (GTK_LABEL (app.subtitle), sub);
        remove_stale_parts (folder);

        g_autoptr (GError) error = NULL;
        g_autoptr (GDir) dir = g_dir_open (folder, 0, &error);
        if (dir == NULL) {
            alert ("Cannot open folder", error->message, NULL);
            update_speed_guide ();
            update_controls ();
            return;
        }
        /* Not recursive, by design; the output folder has no media extension so it never matches. */
        for (const char *name; (name = g_dir_read_name (dir)) != NULL;) {
            if (!vc_mode_accepts (app.mode, name))
                continue;
            g_autofree char *path = g_build_filename (folder, name, NULL);
            add_item (path);
        }
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
        load_source (path, NULL);
}

static void
on_choose (void)
{
    g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
    g_autofree char *title = g_strdup_printf ("Choose a folder of %ss", noun ());
    gtk_file_dialog_set_title (d, title);
    gtk_file_dialog_select_folder (d, app.win, NULL, folder_chosen, NULL);
}

static void
files_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GListModel) list = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source),
                                                                        res, NULL);
    if (list == NULL)
        return;
    g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
    for (guint i = 0; i < g_list_model_get_n_items (list); i++) {
        g_autoptr (GFile) f = g_list_model_get_item (list, i);
        g_autofree char *path = g_file_get_path (f);
        if (path != NULL)
            g_strv_builder_add (b, path);
    }
    g_auto (GStrv) files = g_strv_builder_end (b);
    if (files[0] != NULL)
        load_source (NULL, files);
}

static void
on_choose_files (void)
{
    g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
    g_autoptr (GtkFileFilter) f = gtk_file_filter_new ();
    if (app.mode == VC_MODE_IMAGES) {
        gtk_file_dialog_set_title (d, "Choose images");
        gtk_file_filter_set_name (f, "Images");
        const char *exts[] = { "jpg", "jpeg", "png", "webp", "bmp", "tif", "tiff" };
        for (gsize i = 0; i < G_N_ELEMENTS (exts); i++)
            gtk_file_filter_add_suffix (f, exts[i]);
    } else {
        gtk_file_dialog_set_title (d, "Choose videos");
        gtk_file_filter_set_name (f, "Videos");
        gtk_file_filter_add_mime_type (f, "video/*");
        gtk_file_filter_add_suffix (f, "ts");   /* often typed as a Qt translation file */
    }
    gtk_file_dialog_set_default_filter (d, f);
    gtk_file_dialog_open_multiple (d, app.win, NULL, files_chosen, NULL);
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
    load_source (path, NULL);
    return TRUE;
}

/* Estimates depend on the speed, the size cap and (through samples) the frame-rate cap. */
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

/* Locked while busy (update_controls). Rows start fresh: the list is rebuilt from the same
 * source with the new mode's filter (SPEC-app-shell.md D3). */
static void
on_mode (void)
{
    guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.mode_dd));
    if (i >= G_N_ELEMENTS (MODES) || app.mode == MODES[i])
        return;
    app.mode = MODES[i];
    gtk_widget_set_visible (app.compress_opts, app.mode == VC_MODE_COMPRESS);
    gtk_widget_set_visible (app.web_opts, app.mode == VC_MODE_WEB_VIDEO);
    gtk_widget_set_visible (app.frames_opts, app.mode == VC_MODE_FRAMES);
    gtk_widget_set_visible (app.images_opts, app.mode == VC_MODE_IMAGES);
    gtk_label_set_text (GTK_LABEL (app.empty_sub), EMPTY_SUB[i]);
    g_autofree char *hero = g_strdup_printf ("Drop a folder of %ss here", noun ());
    gtk_label_set_text (GTK_LABEL (app.hero), hero);
    gtk_label_set_text (GTK_LABEL (app.size_head),
                        app.mode == VC_MODE_COMPRESS ? "SIZE → ESTIMATED" : "SIZE");
    if (app.items != NULL)
        load_source (app.folder, app.files);
    else
        update_controls ();
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

static void stop_for_quit (void);

static void
close_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    int choice = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
    if (choice == 1) {
        stop_for_quit ();
        gtk_window_destroy (app.win);
    } else if (choice == 2 && is_busy ()) {
        if (app.paused)
            resume_job ();
        /* A hidden window still holds the GtkApplication, so the process and its encodes go on;
         * launching the app again or clicking a notification shows the window. */
        gtk_widget_set_visible (GTK_WIDGET (app.win), FALSE);
        notify ("background", "Compressing in the background",
                "You'll get a notification when it's done. Open Video Compressor to see progress.");
    }
}

static gboolean
on_close (GtkWindow *win, gpointer user_data)
{
    (void) win; (void) user_data;
    if (is_busy ()) {
        g_autoptr (GtkAlertDialog) d = gtk_alert_dialog_new ("Keep compressing in the background?");
        gtk_alert_dialog_set_detail (d, "The window closes and the videos keep compressing. "
                                        "You'll get a notification when they're done.");
        const char *buttons[] = { "Cancel", "Stop and Quit", "Run in Background", NULL };
        gtk_alert_dialog_set_buttons (d, buttons);
        gtk_alert_dialog_set_cancel_button (d, 0);
        gtk_alert_dialog_set_default_button (d, 2);
        gtk_alert_dialog_choose (d, app.win, NULL, close_chosen, NULL);
        return TRUE;
    }
    stop_for_quit ();
    return FALSE;
}

/* The main loop ends with the window, so clean up synchronously here. */
static void
stop_for_quit (void)
{
    app.closing = TRUE;
    g_clear_handle_id (&app.pulse_id, g_source_remove);
    if (app.proc != NULL) {
        g_subprocess_force_exit (app.proc);   /* SIGKILL works on a paused ffmpeg too */
        g_subprocess_wait (app.proc, NULL, NULL);
        for (guint i = 0; i < app.job->steps->len; i++)
            vc_remove_part (((VcStep *) app.job->steps->pdata[i])->part);
    }
    g_cancellable_cancel (app.probe_cancel);
    sample_stop ();
    if (app.toast_id != 0)
        g_source_remove (app.toast_id);
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
    update_controls ();   /* starts size samples if a folder is already loaded */
}

static void
filters_listed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, NULL)
        || !g_subprocess_get_successful (proc)) {
        tools_failed ("ffmpeg -filters failed. Reinstall ffmpeg.");
        return;
    }
    if (app.closing)
        return;
    if (strstr (out, " zscale ") != NULL)
        app.have |= VC_HAVE_ZSCALE;
    app.tools_ok = TRUE;
    update_controls ();
}

/* Each mode's own tool is checked here (SPEC-app-shell.md D6): a missing one shows the banner in
 * that mode only. */
static void
encoders_listed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    const char *ffmpeg = user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, NULL)
        || !g_subprocess_get_successful (proc)) {
        tools_failed ("ffmpeg -encoders failed. Reinstall ffmpeg.");
        return;
    }
    if (app.closing)
        return;
    static const struct { const char *name; guint bit; } encoders[] = {
        { " libx265 ", VC_HAVE_X265 }, { " libx264 ", VC_HAVE_X264 }, { " libwebp ", VC_HAVE_LIBWEBP },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (encoders); i++)
        if (strstr (out, encoders[i].name) != NULL)
            app.have |= encoders[i].bit;
    const char *argv[] = { ffmpeg, "-hide_banner", "-filters", NULL };
    g_autoptr (GError) error = NULL;
    GSubprocess *filters = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                    | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (filters == NULL) {
        tools_failed (error->message);
        return;
    }
    g_subprocess_communicate_utf8_async (filters, NULL, NULL, filters_listed, NULL);

    /* The list shows hardware encoders even when they can't run, so try one frame. */
    if (strstr (out, " hevc_vaapi ") == NULL)
        return;
    g_auto (GStrv) test_argv = vc_build_gpu_test_argv ();
    GSubprocess *test = g_subprocess_newv ((const char *const *) test_argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (test != NULL)
        g_subprocess_wait_check_async (test, NULL, gpu_test_done, NULL);
}

/* Asynchronous so the window paints at once; Choose Folder stays off until it passes. Only ffmpeg
 * and ffprobe are needed to start: every mode reads files with ffprobe. */
static void
check_tools (void)
{
    static char *ffmpeg;   /* kept for encoders_listed */
    ffmpeg = g_find_program_in_path ("ffmpeg");
    g_autofree char *ffprobe = g_find_program_in_path ("ffprobe");
    if (ffmpeg == NULL || ffprobe == NULL) {
        tools_failed ("ffmpeg and ffprobe must be installed and on PATH.\n"
                      "On Fedora: sudo dnf install ffmpeg (from RPM Fusion).");
        return;
    }
    g_autofree char *magick = g_find_program_in_path ("magick");
    g_autofree char *pngquant = g_find_program_in_path ("pngquant");
    app.have |= (magick != NULL ? VC_HAVE_MAGICK : 0) | (pngquant != NULL ? VC_HAVE_PNGQUANT : 0);
    const char *argv[] = { ffmpeg, "-hide_banner", "-encoders", NULL };
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                 | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL)
        tools_failed (error->message);
    else
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, encoders_listed, ffmpeg);
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
    ".guide-row.current > label:first-child { color: @accent_color; font-weight: 600; }"
    ".banner { background: alpha(#f5a623, 0.2); padding: 10px 20px; font-weight: 600; }";

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

/* The Max width dropdown that web video and frames each have, at 1920. */
static GtkWidget *
width_dropdown (void)
{
    const char *widths[] = { "Original width", "Max 3840 px", "Max 2560 px", "Max 1920 px",
                             "Max 1280 px", "Max 960 px", "Max 640 px", NULL };
    G_STATIC_ASSERT (G_N_ELEMENTS (widths) == G_N_ELEMENTS (WEB_WIDTHS) + 1);
    GtkWidget *dd = gtk_drop_down_new_from_strings (widths);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (dd), WEB_DEFAULT_WIDTH);
    gtk_accessible_update_property (GTK_ACCESSIBLE (dd), GTK_ACCESSIBLE_PROPERTY_LABEL, "Max width", -1);
    gtk_widget_set_tooltip_text (dd, "Scale wider videos down to this width, keeping the aspect ratio. "
                                     "Narrower videos are never enlarged.");
    gtk_widget_set_valign (dd, GTK_ALIGN_CENTER);
    return dd;
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
    app.files_empty = label_button ("document-open-symbolic", "Choose _Files…",
                                    G_CALLBACK (on_choose_files));
    GtkWidget *buttons = hbox (10);
    gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (buttons), choose);
    gtk_box_append (GTK_BOX (buttons), app.files_empty);
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
    app.hero = styled_label ("Drop a folder of videos here", "hero", 0.5);
    gtk_box_append (GTK_BOX (inner), app.hero);
    GtkWidget *sub = app.empty_sub = styled_label (EMPTY_SUB[0], "dim-label", 0.5);
    gtk_label_set_wrap (GTK_LABEL (sub), TRUE);
    gtk_box_append (GTK_BOX (inner), sub);
    gtk_widget_set_margin_bottom (sub, 8);
    gtk_box_append (GTK_BOX (inner), buttons);
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
    GtkWidget *size = app.size_head = styled_label ("SIZE → ESTIMATED", "colhead", 1);
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
    /* a size sample is taken at the frame rate it will be encoded at */
    g_signal_connect (app.fps30, "notify::active", G_CALLBACK (on_estimate_input), NULL);
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

    app.compress_opts = hbox (28);
    gtk_box_append (GTK_BOX (app.compress_opts), speed);
    gtk_box_append (GTK_BOX (app.compress_opts), switches);

    /* Web video: Max width and Remove audio */
    app.web_width = width_dropdown ();
    app.no_audio = gtk_switch_new ();
    gtk_widget_set_valign (app.no_audio, GTK_ALIGN_CENTER);
    GtkWidget *na_label = gtk_label_new_with_mnemonic ("Remove _audio");
    gtk_label_set_mnemonic_widget (GTK_LABEL (na_label), app.no_audio);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (app.no_audio), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                    na_label, NULL, -1);
    GtkWidget *na_row = hbox (10);
    gtk_widget_set_tooltip_text (na_row, "No audio track, for muted autoplay backgrounds.");
    gtk_box_append (GTK_BOX (na_row), app.no_audio);
    gtk_box_append (GTK_BOX (na_row), na_label);
    app.web_opts = hbox (28);
    gtk_box_append (GTK_BOX (app.web_opts), app.web_width);
    gtk_box_append (GTK_BOX (app.web_opts), na_row);
    gtk_widget_set_visible (app.web_opts, FALSE);

    /* Video → frames: FPS, Start, End, Max width, Format, Quality */
    const char *rates[] = { "5 fps", "10 fps", "12 fps", "15 fps", "24 fps", "30 fps", "Every frame", NULL };
    G_STATIC_ASSERT (G_N_ELEMENTS (rates) == G_N_ELEMENTS (FRAME_RATES) + 1);
    app.frames_fps = gtk_drop_down_new_from_strings (rates);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (app.frames_fps), FRAMES_DEFAULT_RATE);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.frames_fps), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Frames per second", -1);
    gtk_widget_set_tooltip_text (app.frames_fps, "Frames saved per second of video. Every frame keeps "
                                                 "the video's own rate.");
    gtk_widget_set_valign (app.frames_fps, GTK_ALIGN_CENTER);
    g_signal_connect (app.frames_fps, "notify::selected", G_CALLBACK (on_estimate_input), NULL);
    GtkWidget **times[] = { &app.frames_start, &app.frames_end };
    const char *time_label[] = { "Sta_rt", "_End" };
    const char *time_hint[] = { "0:00", "end" };
    const char *time_tip[] = { "Where to start: empty for the beginning, or 12, 12.5, 1:05, 0:01:05.",
                               "Where to stop: empty for the end. The same range applies to every video." };
    GtkWidget *range = hbox (8);
    for (guint i = 0; i < 2; i++) {
        *times[i] = gtk_entry_new ();
        gtk_editable_set_width_chars (GTK_EDITABLE (*times[i]), 7);
        gtk_entry_set_placeholder_text (GTK_ENTRY (*times[i]), time_hint[i]);
        gtk_widget_set_valign (*times[i], GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (*times[i], time_tip[i]);
        g_signal_connect (*times[i], "changed", G_CALLBACK (on_estimate_input), NULL);
        GtkWidget *l = gtk_label_new_with_mnemonic (time_label[i]);
        gtk_label_set_mnemonic_widget (GTK_LABEL (l), *times[i]);
        gtk_accessible_update_relation (GTK_ACCESSIBLE (*times[i]), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                        l, NULL, -1);
        gtk_box_append (GTK_BOX (range), l);
        gtk_box_append (GTK_BOX (range), *times[i]);
    }
    app.frames_width = width_dropdown ();
    const char *formats[] = { "WebP", "JPG", NULL };
    app.frames_format = gtk_drop_down_new_from_strings (formats);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.frames_format), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Format", -1);
    gtk_widget_set_tooltip_text (app.frames_format, "WebP is smaller and works in every current browser, "
                                                    "Safari 14 and later included.");
    gtk_widget_set_valign (app.frames_format, GTK_ALIGN_CENTER);
    app.frames_quality = gtk_spin_button_new_with_range (1, 100, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (app.frames_quality), 80);
    gtk_widget_set_valign (app.frames_quality, GTK_ALIGN_CENTER);
    GtkWidget *q_label = gtk_label_new_with_mnemonic ("_Quality");
    gtk_label_set_mnemonic_widget (GTK_LABEL (q_label), app.frames_quality);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (app.frames_quality), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                    q_label, NULL, -1);
    GtkWidget *q_row = hbox (8);
    gtk_widget_set_tooltip_text (q_row, "1–100. Higher keeps more detail in bigger files.");
    gtk_box_append (GTK_BOX (q_row), q_label);
    gtk_box_append (GTK_BOX (q_row), app.frames_quality);
    app.frames_opts = hbox (20);
    gtk_box_append (GTK_BOX (app.frames_opts), app.frames_fps);
    gtk_box_append (GTK_BOX (app.frames_opts), range);
    gtk_box_append (GTK_BOX (app.frames_opts), app.frames_width);
    gtk_box_append (GTK_BOX (app.frames_opts), app.frames_format);
    gtk_box_append (GTK_BOX (app.frames_opts), q_row);
    gtk_widget_set_visible (app.frames_opts, FALSE);

    /* Compress images: Max width (default Original) and its own Move to Trash */
    app.images_width = width_dropdown ();
    gtk_drop_down_set_selected (GTK_DROP_DOWN (app.images_width), 0);
    gtk_widget_set_tooltip_text (app.images_width, "Scale wider images down to this width, keeping the "
                                                   "aspect ratio. Narrower images are never enlarged.");
    app.img_trash = gtk_switch_new ();
    gtk_widget_set_valign (app.img_trash, GTK_ALIGN_CENTER);
    GtkWidget *it_label = gtk_label_new_with_mnemonic ("Move originals to _Trash");
    gtk_label_set_mnemonic_widget (GTK_LABEL (it_label), app.img_trash);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (app.img_trash), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                    it_label, NULL, -1);
    GtkWidget *it_row = hbox (10);
    gtk_widget_set_tooltip_text (it_row, "After each same-type copy (jpg or png) is checked: it opens, has "
                                         "the expected size and is at least 5% smaller. Asks before starting.");
    gtk_box_append (GTK_BOX (it_row), app.img_trash);
    gtk_box_append (GTK_BOX (it_row), it_label);
    app.images_opts = hbox (28);
    gtk_box_append (GTK_BOX (app.images_opts), app.images_width);
    gtk_box_append (GTK_BOX (app.images_opts), it_row);
    gtk_widget_set_visible (app.images_opts, FALSE);

    GtkWidget *ready = hbox (28);
    gtk_box_append (GTK_BOX (ready), app.compress_opts);
    gtk_box_append (GTK_BOX (ready), app.web_opts);
    gtk_box_append (GTK_BOX (ready), app.frames_opts);
    gtk_box_append (GTK_BOX (ready), app.images_opts);
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
    gtk_window_set_title (app.win, "Media Tools");
    gtk_window_set_default_size (app.win, 1260, 720);
    g_signal_connect (app.win, "close-request", G_CALLBACK (on_close), NULL);

    app.title = styled_label ("Media Tools", "heading", 0.5);
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
    app.files_btn = label_button ("document-open-symbolic", "Choose _Files…",
                                  G_CALLBACK (on_choose_files));
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app.files_btn);
    app.mode_dd = gtk_drop_down_new_from_strings (MODE_NAMES);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.mode_dd), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Mode", -1);
    gtk_widget_set_tooltip_text (app.mode_dd, "What to make from your videos");
    g_signal_connect (app.mode_dd, "notify::selected", G_CALLBACK (on_mode), NULL);
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app.mode_dd);
    gtk_window_set_titlebar (app.win, header);

    app.pages = gtk_stack_new ();
    gtk_widget_set_vexpand (app.pages, TRUE);
    gtk_stack_add_named (GTK_STACK (app.pages), build_empty_page (), "empty");
    gtk_stack_add_named (GTK_STACK (app.pages), build_list_page (), "list");
    GtkWidget *content_box = vbox (0);
    app.banner = styled_label ("", "banner", 0);
    gtk_label_set_wrap (GTK_LABEL (app.banner), TRUE);
    gtk_widget_set_visible (app.banner, FALSE);
    gtk_box_append (GTK_BOX (content_box), app.banner);
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
