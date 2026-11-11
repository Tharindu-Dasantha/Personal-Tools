#include "modes.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>
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
    ST_ACTIVE,     /* a running job (max_jobs at once), possibly paused */
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
    /* images: one line per output (same type, then webp); size 0 = "already optimized", and
     * `fresh` FALSE = it existed before this run */
    struct { char ext[8]; guint64 size; gboolean fresh; } res[2];
    guint n_res;
    char *twin_out;      /* images: "twin.jpg.jpg" when another image has this stem, else NULL */
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
    GSubprocess *proc;   /* while active: the current step's process */
    int pending;         /* the current step's stdout EOF + stderr EOF + process exit */
    char *last_err;      /* the current step's last stderr line */
    gboolean paused;
    gint64 t0, paused_at, paused_total;
    /* GPU sample: tried at these Max size / Max fps settings; kbps is 0 when it failed */
    gboolean sampled;
    int sample_side, sample_fps;
    double sample_kbps;
    GtkWidget *row, *box, *check, *name_chip, *meta, *chips, *bar, *pct;
    GtkWidget *tag[2], *val[2], *pill[2];   /* the status column's two lines */
    GtkWidget *b_play, *b_pause, *b_stop, *b_show, *b_trash;
} Item;

typedef struct {
    GtkWindow *win;
    GtkWidget *title, *title_badge, *subtitle, *choose_btn, *files_btn, *pages, *list, *select_all;
    GtkWidget *summary, *sum_icon, *sum_title, *sum_sub, *sum_before, *sum_after, *sum_pct;
    GtkWidget *start_btn, *pause_btn, *stop_btn, *show_btn, *tool_sep;
    GtkWidget *speed_btns[8], *guide_rows, *subs, *trash, *gpu;   /* speed_btns[0] (GPU) is the gpu switch */
    GtkWidget *fps30, *max_size, *empty_icon, *hero, *empty_sub;
    GtkWidget *mode_list, *mode_row[4], *mode_count[4], *mode_lock[4], *mode_dot[4], *tool_state[4];
    GtkWidget *banner, *banner_title, *banner_sub, *banner_code, *size_head;
    GtkWidget *insp_icon, *insp_title, *insp_sub, *insp_note, *insp_stack;
    GtkWidget *web_width, *no_audio, *frames_opts, *fps_btns[8], *frames_start, *frames_end;
    GtkWidget *range_err, *frames_width, *fmt_btns[2], *fmt_caption, *frames_quality, *quality_val;
    GtkWidget *images_opts, *images_width, *img_trash, *sel_clear, *sum_pct_cap;
    GtkWidget *footer, *foot_left, *foot_bar, *foot_right;
    GtkWidget *selbar, *sel_label, *sel_start, *sel_pause, *sel_stop, *sel_trash;
    GtkWidget *toast, *toast_label, *close_dialog;
    GtkSizeGroup *col_status, *col_actions, *guide_cols[3];
    guint fps_idx;       /* index into FRAME_RATES */
    guint speed;         /* the x265 speed button's VC_PRESETS index; app.preset is 0 while Use GPU is on */
    double medium_pps, gpu_pps;   /* this machine's encode speed: learn_speed() */
    gboolean webp;       /* frames format */
    GPtrArray *hidden;   /* names in the source that don't fit this mode */
    guint n_dirs;        /* chosen files: how many folders they come from */
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

    /* running jobs (ST_ACTIVE items), at most max_jobs(); a paused one still counts */
    guint n_jobs;
    guint pulse_id;      /* pulses the bars of steps without progress output */
    VcMode mode;
    guint have;          /* VC_HAVE_* bits from the startup tool check */
} App;

static App app;

static void update_controls (void);
static void sample_next (void);
static void render_row (Item *it);
static void learn_speed (Item *it);

static void
item_free (Item *it)
{
    g_free (it->path);
    g_free (it->out);
    g_free (it->note);
    g_free (it->tip);
    g_free (it->twin_out);
    g_free (it->last_err);
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

/* The mode sidebar, in VcMode order: name, subtitle, icon, CSS class (sets the accent), the empty
 * page's line and the inspector's note. */
static const struct {
    const char *name, *sub, *icon, *css, *empty, *note;
} MODE_INFO[] = {
    { "Compress video", "H.265, same quality", "camera-video-symbolic", "compress",
      "Every video is compressed to H.265, keeping its quality and resolution. Pick a mode on the left "
      "to make web videos, extract frames, or compress images instead.",
      "Saves to <tt>compressed/</tt> next to each video. Originals are never changed." },
    { "Web video", "Safari-ready H.264 MP4", "mt-globe-symbolic", "web",
      "Every video becomes an H.264 MP4 that plays in every browser, Safari and iOS included. Pick a "
      "mode on the left to compress, extract frames, or compress images instead.",
      "Saves to <tt>web/</tt> next to each video. Trash isn't offered: web copies are derivatives, "
      "not replacements." },
    { "Video → frames", "JPG / WebP sequence", "view-grid-symbolic", "frames",
      "Every video becomes a folder of numbered WebP or JPG frames, for scroll animations. Pick a "
      "mode on the left to compress or convert videos, or compress images instead.",
      NULL /* set by the format: update_frames_note */ },
    { "Compress images", "Same type + WebP", "image-x-generic-symbolic", "images",
      "Every image gets a smaller copy in its own format, plus a WebP for the web. Pick a mode on "
      "the left to work with videos instead.",
      "Copies under 5% smaller are dropped. Saves to <tt>compressed/</tt>." },
};
/* The frames FPS buttons, in order; 0 is All (every frame) */
static const int FRAME_RATES[] = { 5, 10, 12, 15, 24, 30, 0 };
#define FRAMES_DEFAULT_RATE 3   /* 15 */
/* The Max width dropdowns, in order; 0 is Original */
static const int WEB_WIDTHS[] = { 0, 3840, 2560, 1920, 1280, 960, 640 };
#define WEB_DEFAULT_WIDTH 3   /* 1920 */


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

/* "Compress", "Convert" or "Extract", for button and tooltip titles */
static const char *
verb (void)
{
    return app.mode == VC_MODE_WEB_VIDEO ? "Convert" : app.mode == VC_MODE_FRAMES ? "Extract" : "Compress";
}

/* "Compressing images", "Converting videos", "Extracting frames from videos" */
static char *
doing (void)
{
    return g_strdup_printf ("%s %ss", app.mode == VC_MODE_WEB_VIDEO ? "Converting"
                                      : app.mode == VC_MODE_FRAMES ? "Extracting frames from"
                                      : "Compressing", noun ());
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
    guint rate = app.fps_idx;
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
        .webp = app.webp,
        .quality = (int) gtk_range_get_value (GTK_RANGE (app.frames_quality)),
    };
}

/* Frames: NULL when Start and End parse and End (when set) is after Start, else the field to fix
 * ("Start" or "End"). Turns a bad field red and shows the hint under the range. */
static const char *
check_range (void)
{
    double start, end;
    gboolean s_ok = vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_start)), &start);
    gboolean e_parsed = vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_end)), &end);
    gboolean e_ok = e_parsed && (end == 0 || !s_ok || end > start);
    GtkWidget *w[] = { app.frames_start, app.frames_end };
    gboolean ok[] = { s_ok, e_ok };
    for (guint i = 0; i < 2; i++) {
        if (ok[i])
            gtk_widget_remove_css_class (w[i], "error");
        else
            gtk_widget_add_css_class (w[i], "error");
    }
    gtk_label_set_text (GTK_LABEL (app.range_err), !s_ok || !e_parsed ? "Use 12, 12.5, 1:05 or 0:01:05"
                                                   : "End must be after Start");
    gtk_widget_set_visible (gtk_widget_get_parent (app.range_err), !s_ok || !e_ok);
    return !s_ok ? "Start" : !e_ok ? "End" : NULL;
}

/* "0:01.5", "1:05", "1:02:03": a frames range end for the row's second line */
static char *
format_clock (double t)
{
    guint tenths = (guint) llround (t * 10), s = tenths / 10, frac = tenths % 10;
    g_autofree char *hm = s >= 3600 ? g_strdup_printf ("%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60)
                                    : g_strdup_printf ("%u:%02u", s / 60, s % 60);
    return frac != 0 ? g_strdup_printf ("%s.%u", hm, frac) : g_steal_pointer (&hm);
}

/* A path for display, with the home folder as ~: "~/Videos/Phone 2026" */
static char *
home_relative (const char *path)
{
    g_autofree char *display = g_filename_display_name (path);
    const char *home = g_get_home_dir ();
    gsize hl = strlen (home);
    return strncmp (display, home, hl) == 0 && (display[hl] == '/' || !display[hl])
               ? g_strconcat ("~", display + hl, NULL)
               : g_steal_pointer (&display);
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

/* Tooltip copy as in the design: a bold title, one line of help, then an optional dimmed hint
 * (the shortcut). */
static void
set_tip (GtkWidget *w, const char *title, const char *key, const char *body)
{
    g_autofree char *m = key ? g_markup_printf_escaped ("<b>%s</b>\n%s\n<span alpha=\"60%%\">%s</span>",
                                                        title, body, key)
                             : g_markup_printf_escaped ("<b>%s</b>\n%s", title, body);
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
           || (it->state == ST_ACTIVE && it->paused);
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
    return it->state == ST_QUEUED || (it->state == ST_ACTIVE && !it->paused);
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

/* One video at a time: x265 and the GPU path already use the whole machine. Images run several at
 * once, since magick and pngquant use about one core each (SPEC-app-shell.md Decision 4). */
static guint
max_jobs (void)
{
    return app.mode == VC_MODE_IMAGES ? CLAMP (g_get_num_processors () / 2, 1, 8) : 1;
}

/* How many running jobs are paused; `first` (may be NULL) gets the first one. */
static guint
paused_jobs (Item **first)
{
    guint n = 0;
    for (guint i = 0; app.n_jobs > 0 && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state == ST_ACTIVE && it->paused && n++ == 0 && first != NULL)
            *first = it;
    }
    return n;
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

/* One line of the status column: a small format tag ("JPG"), a value (markup) and a chip. */
static void
set_line (Item *it, guint i, const char *tag, const char *val, const char *pill, const char *style)
{
    gtk_widget_set_visible (it->tag[i], tag != NULL);
    if (tag != NULL)
        gtk_label_set_text (GTK_LABEL (it->tag[i]), tag);
    gtk_widget_set_visible (it->val[i], val != NULL);
    if (val != NULL)
        gtk_label_set_markup (GTK_LABEL (it->val[i]), val);
    gtk_widget_set_visible (it->pill[i], pill != NULL);
    if (pill != NULL) {
        gtk_label_set_text (GTK_LABEL (it->pill[i]), pill);
        const char *classes[] = { "pill", style, NULL };
        gtk_widget_set_css_classes (it->pill[i], classes);
    }
    if (i == 1)   /* an empty line would still add the column's spacing */
        gtk_widget_set_visible (gtk_widget_get_parent (it->tag[1]), tag != NULL || val != NULL || pill != NULL);
}

static void
render_actions (Item *it)
{
    gboolean job = it->state == ST_ACTIVE;
    gboolean run = it->hover || job || it->state == ST_HELD;
    gtk_widget_set_visible (it->b_play, run && (can_start (it) || it->state == ST_QUEUED));
    gtk_widget_set_visible (it->b_pause, run && can_pause (it));
    gtk_widget_set_visible (it->b_stop, run && can_stop (it));
    gtk_widget_set_visible (it->b_show, it->hover && it->out != NULL);
    gtk_widget_set_visible (it->b_trash, it->hover && can_trash (it));

    if (job && it->paused) {
        g_autofree char *body = g_strdup_printf ("Continue from %d%%.", (int) (it->fraction * 100));
        set_tip (it->b_play, "Resume", NULL, body);
    } else if (app.n_jobs > 0) {   /* queued items only wait while jobs run */
        g_autofree char *t = g_strdup_printf ("%s next", verb ());
        g_autofree char *b = g_strdup_printf ("Move this %s to the front of the queue.", noun ());
        set_tip (it->b_play, t, NULL, b);
    } else {
        g_autofree char *t = g_strdup_printf ("%s this %s", verb (), noun ());
        g_autofree char *b = g_strdup_printf ("%s only this one.", verb ());
        set_tip (it->b_play, t, NULL, b);
    }
    if (job) {
        g_autofree char *t = g_strdup_printf ("Pause this %s", noun ());
        set_tip (it->b_pause, t, NULL,
                 "Resumes from the same point. The rest of the queue waits.");
        set_tip (it->b_stop, "Stop", NULL, "Stop and remove the unfinished file. The original stays.");
    } else {
        set_tip (it->b_pause, "Hold", NULL, "The queue skips it until you press its play button.");
        set_tip (it->b_stop, it->state == ST_CHECKING ? "Stop" : "Skip", NULL,
                 it->state == ST_CHECKING ? "Keep the original instead of moving it to Trash."
                                          : "Take it out of this batch.");
    }
    if (job && !it->paused)
        gtk_widget_add_css_class (it->b_pause, "suggested-action");
    else
        gtk_widget_remove_css_class (it->b_pause, "suggested-action");
}

#define DIM "<span alpha=\"39321\">"   /* 60%, without a % that printf would read */

/* The active row's second line: "1080p · about 40 s left", "47 of 75 frames" or "Step 2 of 2". */
static char *
active_detail (Item *it, const VcOptions *o)
{
    if (it->stop)
        return g_strdup ("Stopping…");
    if (app.mode == VC_MODE_IMAGES)
        return g_strdup_printf ("Step %u of %u", it->step + 1, it->steps != NULL ? it->steps->len : 1);
    if (app.mode == VC_MODE_FRAMES) {
        guint n = vc_frames_estimate (it->probe->duration, it->probe->fps, o);
        return n > 0 ? g_strdup_printf ("%u of %u frames", (guint) (it->fraction * n), n)
                     : g_strdup ("Extracting frames");
    }
    int w = it->probe->width, h = it->probe->height;
    if (app.mode == VC_MODE_WEB_VIDEO && o->max_width > 0 && w > o->max_width) {
        h = (int) ((double) h * o->max_width / w);
        w = o->max_width;
    }
    const char *cls = app.mode == VC_MODE_COMPRESS ? vc_output_class (it->probe, o)->label
                                                   : vc_class_for (w, h)->label;
    double active = (g_get_monotonic_time () - it->t0 - it->paused_total) / 1e6;
    if (it->paused)
        return g_strdup_printf ("%s · paused", cls);
    if (it->fraction < 0.02 || active < 5)
        return g_strdup_printf ("%s · estimating time left", cls);
    g_autofree char *left = vc_format_time_left (active / it->fraction * (1 - it->fraction));
    left[0] = g_ascii_tolower (left[0]);
    return g_strdup_printf ("%s · %s", cls, left);
}

/* "JPG" for photo.jpeg: images show their file type, not the codec */
static char *
file_type (const char *path)
{
    const char *dot = strrchr (path, '.');
    char *t = g_ascii_strup (dot != NULL ? dot + 1 : "", -1);
    const char *alias = strcmp (t, "JPEG") == 0 ? "JPG" : strcmp (t, "TIF") == 0 ? "TIFF" : NULL;
    if (alias == NULL)
        return t;
    g_free (t);
    return g_strdup (alias);
}

static void
render_row (Item *it)
{
    if (app.closing)
        return;
    g_autofree char *size = g_format_size (it->size);
    g_autofree char *v0 = NULL, *p0 = NULL, *v1 = NULL;
    const char *s0 = "neutral", *tip = it->tip;
    gboolean active = it->state == ST_ACTIVE;
    gboolean images_done = FALSE;
    VcOptions o = current_options ();
    guint frames = app.mode == VC_MODE_FRAMES && it->probe != NULL
                       ? vc_frames_estimate (it->probe->duration, it->probe->fps, &o) : 0;

    switch (it->state) {
    case ST_ACTIVE:
        v1 = active_detail (it, &o);
        break;
    case ST_QUEUED:
        v0 = frames > 0 ? g_strdup_printf (DIM "Waiting · ≈ %u frames</span>", frames)
                        : g_strdup (DIM "Waiting</span>");
        break;
    case ST_HELD:
        p0 = g_strdup ("On hold");
        s0 = "hold";
        break;
    case ST_CHECKING:
        v0 = g_strdup (DIM "Checking copy…</span>");
        break;
    case ST_DONE:
    case ST_TRASHED:
        if (it->extra != NULL && g_str_has_prefix (it->extra, "Already")) {
            p0 = g_strdup (it->extra);
        } else if (it->out == NULL) {
            p0 = g_strdup (it->note != NULL ? it->note : "Already efficient");
            tip = tip != NULL ? tip : "A compressed copy would not be at least 5% smaller, so none was kept.";
        } else if (app.mode == VC_MODE_IMAGES) {
            images_done = TRUE;
        } else if (app.mode == VC_MODE_FRAMES) {
            g_autofree char *total = g_format_size (it->out_size);
            v0 = g_strdup_printf ("<b>%u frames</b>", it->n_frames);
            p0 = g_strdup_printf ("✓ %s", total);
            s0 = "done";
        } else {
            g_autofree char *out = g_format_size (it->out_size);
            g_autofree char *change = change_text (it->size, it->out_size);
            v0 = g_markup_printf_escaped (DIM "%s →</span> <b>%s</b>", size, out);
            gboolean smaller = it->out_size <= it->size;
            p0 = smaller ? g_strdup_printf ("✓ %s", change) : g_strdup (change);
            s0 = smaller ? "done" : "bigger";
            if (!smaller)
                tip = "The web copy came out bigger than the original. It's kept anyway: it plays everywhere.";
        }
        break;
    case ST_NEW:
        if (it->probed && it->probe == NULL) {
            p0 = g_strdup ("Unreadable");
            s0 = "error";
        } else if (it->probe == NULL) {
            break;   /* still reading */
        } else if (it->note != NULL) {
            v0 = g_markup_printf_escaped (DIM "%s</span>", size);
            p0 = g_strdup (it->note);
            s0 = strcmp (it->note, "Failed") == 0 ? "error" : "neutral";
        } else if (app.mode == VC_MODE_IMAGES) {
            v0 = g_strdup (DIM "Ready</span>");
        } else if (app.mode == VC_MODE_FRAMES) {
            if (it->probe->duration > 0 && o.start >= it->probe->duration) {
                p0 = g_strdup ("Starts after the end");
                tip = tip != NULL ? tip : "Start is past this video's end, so it will be skipped.";
            } else if (frames > 0) {
                g_autofree char *a = format_clock (o.start);
                g_autofree char *b = format_clock (o.end > 0 ? MIN (o.end, it->probe->duration)
                                                             : it->probe->duration);
                g_autofree char *rate = o.fps > 0 ? g_strdup_printf ("%g fps", o.fps) : g_strdup ("every frame");
                v0 = g_strdup_printf ("≈ <b>%u frame%s</b>", frames, frames == 1 ? "" : "s");
                v1 = g_strdup_printf ("%s → %s · %s", a, b, rate);
                tip = tip != NULL ? tip : "≈ at the chosen FPS and range.";
            }
        } else {
            gboolean measured = FALSE;
            guint64 est = app.mode == VC_MODE_COMPRESS ? item_estimate (it, &o, &measured) : 0;
            if (est == 0) {
                v0 = g_strdup (size);
            } else if (!vc_output_is_worth_keeping (it->size, est)) {
                p0 = g_strdup ("Likely no saving");
                tip = tip != NULL ? tip : "A compressed copy would likely not be 5% smaller. Start skips it "
                                          "unless you select it.";
            } else {
                g_autofree char *est_size = g_format_size (est);
                v0 = g_markup_printf_escaped (DIM "%s → ≈</span> <b>%s</b>", size, est_size);
                p0 = g_strdup_printf ("−%d%%", percent_smaller (it->size, est));
                s0 = "estimate";
                tip = tip != NULL ? tip
                      : measured ? "≈ measured with a short GPU test encode at these settings. "
                                   "The whole video can differ a little."
                                 : "≈ is a rough estimate at the chosen speed. "
                                   "The real size depends on the footage.";
            }
        }
        break;
    }

    if (images_done) {   /* each output on its own line: same type, then webp */
        for (guint i = 0; i < 2; i++) {
            if (i >= it->n_res) {
                set_line (it, i, NULL, NULL, NULL, NULL);
                continue;
            }
            g_autofree char *tag = g_ascii_strup (it->res[i].ext, -1);
            g_autofree char *rsize = g_format_size (it->res[i].size);
            g_autofree char *change = change_text (it->size, it->res[i].size);
            g_autofree char *pill = g_strdup_printf ("✓ %s", change);
            g_autofree char *val = it->res[i].size == 0 ? g_strdup (DIM "already optimized</span>")
                                   : !it->res[i].fresh ? g_strdup (DIM "exists</span>")
                                   : g_strdup_printf ("<b>%s</b>", rsize);
            set_line (it, i, tag, val, it->res[i].size > 0 && it->res[i].fresh ? pill : NULL, "done");
        }
    } else {
        set_line (it, 0, NULL, v0, p0, s0);
        g_autofree char *sub = v1 != NULL ? g_markup_printf_escaped (DIM "%s</span>", v1) : NULL;
        set_line (it, 1, NULL, sub, NULL, NULL);
    }
    for (guint i = 0; i < 2; i++) {
        gtk_widget_set_tooltip_text (it->val[i], tip);
        gtk_widget_set_tooltip_text (it->pill[i], tip);
    }
    gtk_widget_set_visible (it->bar, active);
    gtk_widget_set_visible (it->pct, active);
    if (active) {
        g_autofree char *pct = it->probe->duration <= 0 || app.mode == VC_MODE_IMAGES ? g_strdup ("")
                               : g_strdup_printf ("%d%%", (int) (it->fraction * 100));
        gtk_label_set_text (GTK_LABEL (it->pct), pct);
        if (app.mode != VC_MODE_IMAGES && it->probe->duration > 0)   /* else pulse_bar pulses it */
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), it->fraction);
    }

    /* the meta line, per mode as in the design */
    GString *m = g_string_new (NULL);
    VcProbe *p = it->probe;
    if (p != NULL && app.mode == VC_MODE_IMAGES && it->twin_out != NULL) {
        g_string_printf (m, "→ %s%s · %s", it->twin_out,
                         g_str_has_suffix (it->twin_out, ".webp") ? "" : " + .webp", size);
    } else if (p != NULL && app.mode == VC_MODE_IMAGES) {
        g_autofree char *type = file_type (it->path);
        g_string_printf (m, "%d×%d · %s · %s", p->width, p->height, type, size);
    } else if (p != NULL) {
        g_autofree char *codec = vc_codec_label (p->codec);
        g_autofree char *dur = vc_format_duration (p->duration);
        if (app.mode == VC_MODE_WEB_VIDEO && app.n_dirs > 1) {   /* chosen files from several folders */
            g_autofree char *dir = g_path_get_dirname (it->path);
            g_autofree char *where = home_relative (dir);
            g_string_append_printf (m, "%s · ", where[0] == '~' && where[1] == '/' ? where + 2 : where);
        }
        g_string_append_printf (m, "%s · %s · ", codec, dur);
        if (app.mode == VC_MODE_WEB_VIDEO)
            g_string_append (m, vc_class_for (p->width, p->height)->label);
        else
            g_string_append_printf (m, "%d×%d", p->width, p->height);
        if (app.mode == VC_MODE_FRAMES && p->fps > 0)
            g_string_append_printf (m, fabs (p->fps - round (p->fps)) < 0.01 ? " · %.0f fps" : " · %.2f fps",
                                    p->fps);
    } else {
        g_string_assign (m, it->probed ? "Couldn't read this file" : "Reading…");
    }
    gtk_label_set_text (GTK_LABEL (it->meta), m->str);
    g_string_free (m, TRUE);
    gtk_widget_set_tooltip_text (it->meta, it->probe == NULL ? it->tip : NULL);

    /* the chip after the name: what happened to the original, or that it's a twin */
    const char *extra = it->extra != NULL && !g_str_has_prefix (it->extra, "Already") ? it->extra : NULL;
    gtk_widget_set_visible (it->name_chip, extra != NULL || it->twin_out != NULL);
    if (extra != NULL) {
        const char *cls[] = { "chip", g_str_has_prefix (extra, "Original kept") ? "kept" : "trashed", NULL };
        gtk_widget_set_css_classes (it->name_chip, cls);
        gtk_label_set_text (GTK_LABEL (it->name_chip), extra);
        gtk_widget_set_tooltip_text (it->name_chip, it->tip);
    } else if (it->twin_out != NULL) {
        const char *cls[] = { "chip", "twin", NULL };
        gtk_widget_set_css_classes (it->name_chip, cls);
        gtk_label_set_text (GTK_LABEL (it->name_chip), "∞ Twin");
        g_autofree char *base = basename_display (it->path);
        g_autofree char *body = g_strdup_printf ("%s shares its name with another image, so each copy keeps "
                                                 "the full source name. Nothing is overwritten.", base);
        set_tip (it->name_chip, "Twin names", NULL, body);
    }

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

/* The chip after the meta line: Compress video shows the quality class (or 10-bit), web video and
 * frames say when HDR is turned into SDR. */
static void
refresh_chips (Item *it)
{
    for (GtkWidget *c; (c = gtk_widget_get_first_child (it->chips)) != NULL;)
        gtk_box_remove (GTK_BOX (it->chips), c);
    VcProbe *p = it->probe;
    if (p == NULL || app.mode == VC_MODE_IMAGES)
        return;
    if (app.mode != VC_MODE_COMPRESS) {
        if (vc_probe_is_hdr (p))
            gtk_box_append (GTK_BOX (it->chips), chip ("HDR → SDR", "hdr"));
    } else if (vc_probe_is_10bit (p)) {
        gtk_box_append (GTK_BOX (it->chips), chip (vc_probe_is_hdr (p) ? "10-bit HDR" : "10-bit", "hdr"));
    } else {
        gtk_box_append (GTK_BOX (it->chips), chip (vc_class_for (p->width, p->height)->label, "res"));
    }
}

/* ---- bottom bar, summary, selection bar ---- */

static void
update_speed_guide (void)
{
    if (app.mode != VC_MODE_COMPRESS)
        return;   /* the guide lives in the compress options; load_source rebuilds it on the way back */
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
        if (app.speed_btns[p] != NULL)
            set_tip (app.speed_btns[p], name, NULL, body);
    }
}

/* The footer while a batch runs: "Converting 4 of 6", the batch's bar and the time left. */
static void
update_progress (void)
{
    if (app.items == NULL || app.closing)
        return;
    guint total = 0, done = 0, paused = 0;
    guint64 all = 0, finished = 0, held = 0;
    double progressed = 0, rate = 0;   /* source bytes, and bytes/s summed over the running jobs */
    gboolean early = FALSE;            /* a job too new to time */
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state == ST_ACTIVE) {
            double active = (g_get_monotonic_time () - it->t0 - it->paused_total) / 1e6;
            progressed += it->fraction * it->size;
            rate += active > 0 ? it->fraction * it->size / active : 0;
            early |= it->fraction < 0.02 || active < 5;
            paused += it->paused;
        }
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
    }
    progressed += finished;
    gboolean all_paused = app.n_jobs > 0 && paused == app.n_jobs;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (app.foot_bar), all > 0 ? progressed / all : 0);
    gtk_widget_set_visible (app.foot_bar, TRUE);

    g_autofree char *title = NULL, *eta = NULL;
    if (app.n_jobs > 0) {
        title = g_markup_printf_escaped ("<b>%s %u of %u</b>",
                                         all_paused ? "Paused ·"
                                         : app.mode == VC_MODE_COMPRESS || app.mode == VC_MODE_IMAGES
                                             ? "Compressing"
                                         : app.mode == VC_MODE_FRAMES ? "Extracting" : "Converting",
                                         MIN (done + app.n_jobs, total), total);   /* the last running one */
        eta = all_paused ? g_strdup ("Press play to resume")
              : early || rate <= 0
                  ? g_strdup ("Estimating time left…")
                  : vc_format_time_left ((all - held - progressed) / rate);
    } else {
        title = g_strdup ("<b>Checking the last copy…</b>");
        eta = g_strdup ("");
    }
    gtk_widget_remove_css_class (app.foot_left, "error");
    gtk_label_set_markup (GTK_LABEL (app.foot_left), title);
    gtk_label_set_text (GTK_LABEL (app.foot_right), eta);
}

static void
update_summary (void)
{
    guint compressed = 0, efficient = 0, unreadable = 0, copies = 0;
    guint64 before = 0, after = 0;
    for (guint i = 0; i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->out != NULL) {
            guint64 smallest = it->out_size;   /* images: the smaller of the two copies */
            for (guint r = 0; r < it->n_res; r++) {
                copies += it->res[r].size > 0 && it->res[r].fresh;
                if (it->res[r].size > 0)
                    smallest = MIN (smallest, it->res[r].size);
            }
            compressed++;
            before += it->size;
            after += smallest;
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
    if (app.mode == VC_MODE_IMAGES)
        g_string_printf (s, "%u image%s · %u cop%s", compressed, compressed == 1 ? "" : "s", copies,
                         copies == 1 ? "y" : "ies");
    else
        g_string_printf (s, "%u %s%s %s", compressed, noun (), compressed == 1 ? "" : "s",
                         web ? "converted" : frames ? "extracted" : "compressed");
    if (app.batch_t0 > 0 && app.batch_end > app.batch_t0) {
        guint secs = (guint) ((app.batch_end - app.batch_t0) / G_USEC_PER_SEC), min = secs / 60;
        if (min == 0)
            g_string_append_printf (s, " in %u s", MAX (secs, 1));
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
    /* frames aren't a smaller copy of the video, and a web copy can come out bigger */
    gtk_label_set_text (GTK_LABEL (app.sum_pct_cap), frames ? "vs the videos" : after > before ? "Bigger" : "Smaller");
    gtk_label_set_text (GTK_LABEL (gtk_widget_get_last_child (gtk_widget_get_parent (app.sum_after))),
                        app.mode == VC_MODE_IMAGES ? "Smallest set" : frames ? "Frames" : "After");
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
        g_autofree char *why = doing ();
        app.inhibit = gtk_application_inhibit (gapp, app.win, GTK_APPLICATION_INHIBIT_SUSPEND, why);
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

/* The install command for a missing tool, for the banner's copy button. */
static const char *
fix_command (const char *missing)
{
    return strcmp (missing, "magick") == 0 ? "sudo dnf install ImageMagick"
           : strcmp (missing, "pngquant") == 0 ? "sudo dnf install pngquant"
           : "sudo dnf swap ffmpeg-free ffmpeg --allowerasing";   /* RPM Fusion's full build */
}

static void
set_ready (GtkWidget *label, gboolean ok)
{
    gtk_label_set_text (GTK_LABEL (label), !app.tools_ok ? "Checking…" : ok ? "Ready" : "Missing");
    const char *cls[] = { "tool-state", !app.tools_ok ? NULL : ok ? "ok" : "missing", NULL };
    gtk_widget_set_css_classes (label, cls);
}

static void
update_controls (void)
{
    if (app.closing)
        return;
    guint n = app.items != NULL ? app.items->len : 0;
    gboolean probing = app.items != NULL && app.probes_done < n;
    gboolean busy = FALSE, start = FALSE, stop = FALSE, any_done = FALSE, any_result = FALSE;
    gboolean s_start = FALSE, s_pause = FALSE, s_stop = FALSE, s_trash = FALSE;
    guint nsel = 0, ready = 0, skipped = 0, frames = 0;
    guint64 src = 0, est = 0;
    double secs = 0;
    VcOptions o = current_options ();
    for (guint i = 0; i < n; i++) {
        Item *it = app.items->pdata[i];
        busy |= it->state == ST_QUEUED || it->state == ST_ACTIVE || it->state == ST_CHECKING;
        gboolean skip = skip_in_bulk (it);
        skipped += skip;
        start |= can_start (it) && !skip;
        stop |= can_stop (it);
        any_done |= it->state == ST_DONE || it->state == ST_TRASHED;
        any_result |= it->state != ST_NEW || it->note != NULL;
        if (checked (it)) {
            nsel++;
            s_start |= can_start (it);
            s_pause |= can_pause (it);
            s_stop |= can_stop (it);
            s_trash |= can_trash (it);
        }
        if (((it->state == ST_NEW && it->probe != NULL) || it->state == ST_HELD) && !skip) {
            guint64 e = item_estimate (it, &o, NULL);
            ready++;
            src += it->size;
            est += e > 0 ? e : it->size;
            if (app.mode == VC_MODE_FRAMES)
                frames += vc_frames_estimate (it->probe->duration, it->probe->fps, &o);
            secs += vc_estimate_seconds (it->probe, &o, app.medium_pps, app.gpu_pps);
        }
    }
    gboolean finished = n > 0 && !busy && !start && any_done;
    Item *first_paused = NULL;
    guint n_paused = paused_jobs (&first_paused);
    gboolean paused = app.n_jobs > 0 && n_paused == app.n_jobs;   /* the header offers Resume */
    if (busy != app.was_busy)
        on_busy_changed (busy);

    gboolean loaded = app.items != NULL;
    gtk_stack_set_visible_child_name (GTK_STACK (app.pages), loaded ? "list" : "empty");

    /* header: Choose stays visible but dims while busy; Start/Pause swap while a batch runs; a
     * finished batch offers Show in Files instead */
    gtk_widget_set_sensitive (app.choose_btn, app.tools_ok && !busy);
    gtk_widget_set_sensitive (app.files_btn, app.tools_ok && !busy);
    gtk_widget_set_visible (app.show_btn, finished);
    gtk_widget_set_visible (app.start_btn, !finished && (!busy || paused));
    gtk_widget_set_visible (app.pause_btn, !finished && (!busy || !paused));
    gtk_widget_set_visible (app.stop_btn, !finished);
    if (busy && !paused)
        gtk_widget_add_css_class (app.pause_btn, "suggested-action");
    else
        gtk_widget_remove_css_class (app.pause_btn, "suggested-action");

    /* sidebar: modes lock while busy; a dot marks a mode whose tool is missing */
    for (guint m = 0; m < G_N_ELEMENTS (MODE_INFO); m++) {
        gboolean here = m == app.mode;
        gboolean missing_m = app.tools_ok && vc_mode_missing_tool (m, app.have) != NULL;
        gtk_widget_set_sensitive (app.mode_row[m], !busy || here);
        gtk_widget_set_visible (app.mode_lock[m], busy && !here);
        gtk_widget_set_visible (app.mode_dot[m], missing_m && !(busy && !here));
        gtk_widget_set_visible (app.mode_count[m], here && loaded && !busy && !missing_m && n > 0);
        g_autofree char *count = g_strdup_printf ("%u", n);
        gtk_label_set_text (GTK_LABEL (app.mode_count[m]), count);
    }
    set_ready (app.tool_state[0], app.tools_ok);
    set_ready (app.tool_state[1], (app.have & (VC_HAVE_X265 | VC_HAVE_X264 | VC_HAVE_ZSCALE))
                                  == (VC_HAVE_X265 | VC_HAVE_X264 | VC_HAVE_ZSCALE));
    set_ready (app.tool_state[2], app.have & VC_HAVE_MAGICK);
    set_ready (app.tool_state[3], app.have & VC_HAVE_PNGQUANT);

    /* Shown once the tool check is done; this mode can't start without the tool. */
    const char *missing = app.tools_ok ? vc_mode_missing_tool (app.mode, app.have) : NULL;
    gtk_widget_set_visible (app.banner, missing != NULL);
    if (missing != NULL) {
        gboolean ffmpeg_part = strcmp (missing, "magick") != 0 && strcmp (missing, "pngquant") != 0;
        g_autofree char *t = g_strdup_printf ("%s needs %s%s", MODE_INFO[app.mode].name,
                                              ffmpeg_part ? "ffmpeg with " : "", missing);
        gtk_label_set_text (GTK_LABEL (app.banner_title), t);
        gtk_label_set_text (GTK_LABEL (app.banner_sub), ffmpeg_part
                            ? "This ffmpeg lacks it: install the full build from RPM Fusion. The other modes still work."
                            : "Install it, then come back. The other modes still work.");
        gtk_label_set_text (GTK_LABEL (app.banner_code), fix_command (missing));
    }
    const char *bad_field = check_range ();   /* also keeps the frames inspector's fields current */
    if (app.mode != VC_MODE_FRAMES)
        bad_field = NULL;
    gboolean can_run = app.tools_ok && missing == NULL && !probing && bad_field == NULL;

    gtk_widget_set_sensitive (app.start_btn, can_run && start && (!busy || paused));
    gtk_widget_set_sensitive (app.pause_btn, app.n_jobs > n_paused);
    gtk_widget_set_sensitive (app.stop_btn, stop);
    if (paused) {
        g_autofree char *name = basename_display (first_paused->path);
        g_autofree char *body = n_paused > 1 ? g_strdup_printf ("Continue the %u paused %ss.", n_paused, noun ())
                                : g_strdup_printf ("Continue %s from %d%%.", name,
                                                   (int) (first_paused->fraction * 100));
        set_tip (app.start_btn, "Resume", "Space", body);
    } else {
        g_autofree char *body = g_strdup_printf ("%s every waiting %s.", verb (), noun ());
        g_autofree char *key = max_jobs () > 1 ? g_strdup_printf ("Space · up to %u at once", max_jobs ())
                                               : g_strdup_printf ("Space · one %s at a time", noun ());
        set_tip (app.start_btn, "Start", key, body);
    }
    gboolean gpu_on = gtk_switch_get_active (GTK_SWITCH (app.gpu));
    for (guint p = 1; VC_PRESETS[p] != NULL && p < G_N_ELEMENTS (app.speed_btns); p++)
        gtk_widget_set_sensitive (app.speed_btns[p], !busy && !gpu_on);   /* the GPU has no speeds */
    gtk_widget_set_sensitive (app.gpu, !busy);
    gtk_widget_set_sensitive (app.subs, !busy);
    gtk_widget_set_sensitive (app.trash, !busy);
    gtk_widget_set_sensitive (app.fps30, !busy);
    gtk_widget_set_sensitive (app.max_size, !busy);
    gtk_widget_set_sensitive (app.web_width, !busy);
    gtk_widget_set_sensitive (app.no_audio, !busy);
    gtk_widget_set_sensitive (app.frames_opts, !busy);
    gtk_widget_set_sensitive (app.images_opts, !busy);

    /* the column head says what the status column shows in this mode */
    gtk_label_set_text (GTK_LABEL (app.size_head),
                        app.mode == VC_MODE_COMPRESS ? (any_result ? "SIZE → RESULT" : "SIZE → ESTIMATE")
                        : app.mode == VC_MODE_WEB_VIDEO ? "SIZE → RESULT"
                        : app.mode == VC_MODE_FRAMES ? "FRAMES"
                        : any_result ? "SAME TYPE · WEBP" : "STATUS");

    /* footer: progress while busy, else what Start would do or why it can't */
    gtk_widget_set_visible (app.footer, loaded && !finished);
    if (busy) {
        update_progress ();
    } else {
        g_autofree char *left = NULL, *right = NULL;
        gboolean err = FALSE;
        g_autofree char *src_text = g_format_size (src), *est_text = g_format_size (est);
        g_autofree char *preset = preset_title (app.preset);
        g_autofree char *things = g_strdup_printf ("%u %s%s", ready, noun (), ready == 1 ? "" : "s");
        if (missing != NULL) {
            left = g_markup_printf_escaped ("Start is off until %s is installed", missing);
        } else if (bad_field != NULL) {
            left = g_markup_printf_escaped ("⚠ Fix the %s time to start", bad_field);
            err = TRUE;
        } else if (probing) {
            left = g_markup_printf_escaped ("Reading %ss…", noun ());
        } else if (ready == 0 && skipped > 0) {
            left = g_markup_printf_escaped ("%u video%s likely won't get smaller · select to compress anyway",
                                            skipped, skipped == 1 ? "" : "s");
        } else if (ready == 0) {
            left = g_markup_printf_escaped ("Nothing waiting to %s", app.mode == VC_MODE_WEB_VIDEO ? "convert"
                                            : app.mode == VC_MODE_FRAMES ? "extract" : "compress");
        } else if (app.mode == VC_MODE_COMPRESS) {
            left = g_markup_printf_escaped ("<b>%s ready</b> · %s → ≈ %s", things, src_text, est_text);
        } else {
            left = g_markup_printf_escaped ("<b>%s ready</b> · %s", things, src_text);
        }
        if (app.hidden != NULL && app.hidden->len > 0 && (missing != NULL || app.mode == VC_MODE_IMAGES)) {
            GString *h = g_string_new (NULL);
            for (guint i = 0; i < app.hidden->len && i < 3; i++)
                g_string_append_printf (h, "%s%s", i > 0 ? ", " : "", (char *) app.hidden->pdata[i]);
            if (app.hidden->len > 3)
                g_string_append (h, ", …");
            right = g_strdup_printf ("%u file%s hidden: %s", app.hidden->len, app.hidden->len == 1 ? "" : "s",
                                     h->str);
            g_string_free (h, TRUE);
        } else if (app.mode == VC_MODE_FRAMES && ready > 0) {
            right = g_strdup_printf ("%s · ≈ %u frames", things, frames);
        } else if (app.mode == VC_MODE_COMPRESS && ready > 0 && missing == NULL && secs > 0) {
            guint min = MAX (1, (guint) lround (secs / 60));
            g_autofree char *t = min < 60 ? g_strdup_printf ("%u min", min)
                                 : min % 60 == 0 ? g_strdup_printf ("%u h", min / 60)
                                 : g_strdup_printf ("%u h %u min", min / 60, min % 60);
            right = app.preset == 0 ? g_strdup_printf ("≈ %s on the GPU", t)
                                    : g_strdup_printf ("≈ %s at %s", t, preset);
        }
        if (err)
            gtk_widget_add_css_class (app.foot_left, "error");
        else
            gtk_widget_remove_css_class (app.foot_left, "error");
        gtk_label_set_markup (GTK_LABEL (app.foot_left), left);
        gtk_label_set_text (GTK_LABEL (app.foot_right), right != NULL ? right : "");
        gtk_widget_set_visible (app.foot_bar, FALSE);
    }

    gtk_widget_set_visible (app.selbar, nsel > 0);
    g_autofree char *sel = g_strdup_printf ("%u selected", nsel);
    gtk_label_set_text (GTK_LABEL (app.sel_label), sel);
    gtk_widget_set_sensitive (app.sel_start, can_run && s_start);
    gtk_widget_set_sensitive (app.sel_pause, s_pause);
    gtk_widget_set_sensitive (app.sel_stop, s_stop);
    gtk_widget_set_sensitive (app.sel_trash, s_trash && vc_mode_can_trash (app.mode));
    gtk_widget_set_visible (app.sel_trash, vc_mode_can_trash (app.mode));
    g_autofree char *sel_body = g_strdup_printf ("%u %s%s, in list order. Others wait.", nsel, noun (),
                                                 nsel == 1 ? "" : "s");
    g_autofree char *sel_title = g_strdup_printf ("%s selected", verb ());
    set_tip (app.sel_start, sel_title, NULL, sel_body);
    g_autofree char *trash_body = g_strdup_printf ("For selected finished %ss, after checking each copy. "
                                                   "Asks first.", noun ());
    set_tip (app.sel_trash, "Move originals to Trash", NULL, trash_body);
    g_autofree char *clear_body = g_strdup_printf ("Buttons act on every %s again.", noun ());
    set_tip (app.sel_clear, "Clear selection", "Esc", clear_body);
    g_autofree char *pause_body = g_strdup_printf ("Pause the current %s. Resume continues from the same "
                                                   "point.", noun ());
    set_tip (app.pause_btn, "Pause", "Space", pause_body);

    app.syncing = TRUE;
    gtk_check_button_set_inconsistent (GTK_CHECK_BUTTON (app.select_all), nsel > 0 && nsel < n);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (app.select_all), n > 0 && nsel == n);
    app.syncing = FALSE;
    gtk_widget_set_sensitive (app.select_all, n > 0);

    if (finished)
        update_summary ();
    gtk_widget_set_visible (app.summary, finished);
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
show_toast (const char *icon, const char *title, const char *detail)
{
    gtk_image_set_from_icon_name (GTK_IMAGE (gtk_widget_get_first_child (
        gtk_revealer_get_child (GTK_REVEALER (app.toast)))), icon);
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
    if (it->probe != NULL && app.mode == VC_MODE_IMAGES) {
        /* a twin's outputs keep the whole source name: twin.jpg → twin.jpg.jpg */
        g_autofree char *same = vc_image_outputs (it->path, it->probe, NULL);
        g_autofree char *name = g_path_get_basename (it->path);
        g_autofree char *out = g_path_get_basename (same);
        if (g_str_has_prefix (out, name) && out[strlen (name)] == '.')
            it->twin_out = g_filename_display_name (out);
    }
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
        /* A newer folder replaced this one (or the window closed); its items are gone. Cancelling
         * only stops the reading, so end the ffprobe too (slow on network drives). */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_subprocess_force_exit (proc);
            return;
        }
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
pause_job (Item *it)
{
    /* SIGSTOP freezes ffmpeg in place; SIGCONT (Start) resumes it. */
    g_subprocess_send_signal (it->proc, SIGSTOP);
    it->paused = TRUE;
    it->paused_at = g_get_monotonic_time ();
    render_row (it);
}

static void
resume_job (Item *it)
{
    g_subprocess_send_signal (it->proc, SIGCONT);
    it->paused = FALSE;
    it->paused_total += g_get_monotonic_time () - it->paused_at;
    render_row (it);
}

/* Every running job, paused (TRUE) or resumed. */
static void
pause_all (gboolean pause)
{
    for (guint i = 0; app.n_jobs > 0 && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state == ST_ACTIVE && it->paused != pause)
            pause ? pause_job (it) : resume_job (it);
    }
    if (!pause && app.items != NULL)
        encode_next ();   /* slots that freed while paused */
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
        if (it->state == ST_ACTIVE) {
            resume_job (it);
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
        if (!in_scope (it, one, sel) || !can_start (it) || it->state == ST_ACTIVE
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
        if (it->state == ST_ACTIVE) {
            pause_job (it);
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
            if (it->state == ST_ACTIVE) {
                /* SIGKILL also ends a paused (stopped) ffmpeg. finish_job removes the .part
                 * once ffmpeg is gone and has closed it, then starts the next queued item. */
                g_subprocess_force_exit (it->proc);
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
        show_toast ("user-trash-symbolic", "Original moved to Trash", detail);
    } else {
        it->state = ST_DONE;
        it->extra = "Original kept · trash failed";
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

/* Images: the row's result lines (same type, then webp), from what is on disk now. `out` becomes the
 * same-type copy, else the webp, else NULL. */
static void
image_result (Item *it, GPtrArray *steps)
{
    g_autofree char *webp = NULL;
    g_autofree char *same = vc_image_outputs (it->path, it->probe, &webp);
    const char *outs[] = { same, webp };
    g_clear_pointer (&it->out, g_free);
    it->out_size = 0;
    it->n_res = 0;
    for (guint i = 0; i < G_N_ELEMENTS (outs) && outs[i] != NULL; i++) {
        gboolean planned = FALSE;   /* else it existed before this run */
        for (guint s = 0; s < steps->len; s++)
            planned |= g_strcmp0 (((VcStep *) steps->pdata[s])->output, outs[i]) == 0;
        GStatBuf st;
        gboolean exists = g_stat (outs[i], &st) == 0;
        g_strlcpy (it->res[i].ext, strrchr (outs[i], '.') + 1, sizeof it->res[i].ext);
        it->res[i].size = exists ? (guint64) st.st_size : 0;
        it->res[i].fresh = planned;
        it->n_res = i + 1;
        if (exists && it->out == NULL) {
            it->out = g_strdup (outs[i]);
            it->out_size = st.st_size;
        }
    }
    /* only the same-type copy may replace the original (SPEC-image-compress.md D6) */
    it->webp_only = it->out != NULL && strcmp (it->out, same) != 0;
    if (it->webp_only && trash_on ())
        it->extra = g_str_has_suffix (same, ".png") ? "Original kept · no png copy"
                                                    : "Original kept · no jpg copy";
}

/* Runs when the job's last step has ended (or one failed, or it was stopped): moves the outputs
 * into place, removes every other part, and sets the row. The caller starts the next item. */
static void
finish_job (Item *it, gboolean ok)
{
    if (ok && !it->stop && app.mode == VC_MODE_COMPRESS)
        learn_speed (it);
    g_autoptr (GPtrArray) steps = g_steal_pointer (&it->steps);
    g_autofree char *last_err = g_steal_pointer (&it->last_err);
    it->paused = FALSE;
    app.n_jobs--;
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

static gboolean spawn_step (Item *it);

/* All three events of the current step are in: start the next step, or finish the job. */
static void
step_ended (Item *it)
{
    VcStep *s = it->steps->pdata[it->step];
    g_autoptr (GSubprocess) proc = g_steal_pointer (&it->proc);
    int code = g_subprocess_get_if_exited (proc) ? g_subprocess_get_exit_status (proc) : -1;
    /* Exit 0 must leave the part behind; a soft exit means "no output" and is fine. */
    gboolean ok = (code == 0 && g_file_test (s->part, G_FILE_TEST_EXISTS))
                  || (s->soft_exit != 0 && code == s->soft_exit);
    if (ok && !it->stop && it->step + 1 < it->steps->len) {
        it->step++;
        it->fraction = (double) it->step / it->steps->len;   /* steps without progress output */
        update_progress ();
        if (spawn_step (it))
            return;
        ok = FALSE;
    }
    finish_job (it, ok);
    encode_next ();
}

static void
job_step_done (Item *it)
{
    if (--it->pending == 0 && !app.closing)   /* on_close already removed the parts */
        step_ended (it);
}

static void
on_progress_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GDataInputStream *ds = G_DATA_INPUT_STREAM (source);
    g_autofree char *line = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);
    if (line == NULL) {
        g_object_unref (ds);
        job_step_done (user_data);
        return;
    }
    if (!app.closing) {   /* after close, keep draining to EOF but leave the widgets alone */
        Item *it = user_data;
        VcStep *s = it->steps->pdata[it->step];
        double fraction, dur = s->duration > 0 ? s->duration : it->probe->duration;
        if (s->progress && vc_parse_progress_line (line, dur, &fraction)) {
            it->fraction = (it->step + fraction) / it->steps->len;
            render_row (it);   /* bar, percent and the time left under it */
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
    Item *it = user_data;
    if (line == NULL) {
        g_object_unref (ds);
        job_step_done (it);
        return;
    }
    if (*g_strstrip (line) != '\0') {
        g_free (it->last_err);
        it->last_err = g_utf8_make_valid (line, -1);
    }
    g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_stderr_line,
                                         user_data);
}

static void
on_ffmpeg_exit (GObject *source, GAsyncResult *res, gpointer user_data)
{
    g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
    job_step_done (user_data);
}

/* Pulses the running steps that have no progress output; ends when there are none. */
static gboolean
pulse_bar (gpointer data)
{
    (void) data;
    gboolean any = FALSE;
    for (guint i = 0; !app.closing && app.n_jobs > 0 && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state != ST_ACTIVE || ((VcStep *) it->steps->pdata[it->step])->progress)
            continue;
        any = TRUE;
        if (!it->paused)
            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (it->bar));
    }
    if (!any)
        app.pulse_id = 0;
    return any ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
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

/* Starts the job's current step. FALSE (with it->last_err set) when it can't be started. */
static gboolean
spawn_step (Item *it)
{
    VcStep *s = it->steps->pdata[it->step];
    g_autoptr (GError) error = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup (launcher, lower_priority, NULL, NULL);
    g_clear_pointer (&it->last_err, g_free);
    /* A killed run may have left the part (pngquant won't overwrite one). ffmpeg writes frames
     * into the folder but doesn't create it. */
    if (!vc_remove_part (s->part) || (s->part_is_dir && g_mkdir (s->part, 0755) != 0)) {
        g_autofree char *display = g_filename_display_name (s->part);
        it->last_err = g_strdup_printf ("Cannot create %s", display);
        return FALSE;
    }
    it->proc = g_subprocess_launcher_spawnv (launcher, (const char *const *) s->argv, &error);
    if (it->proc == NULL) {
        it->last_err = g_strdup (error->message);
        return FALSE;
    }
    if (it->paused)   /* paused just as the previous step ended */
        g_subprocess_send_signal (it->proc, SIGSTOP);
    it->pending = 3;
    if (!s->progress && app.pulse_id == 0)
        app.pulse_id = g_timeout_add (100, pulse_bar, NULL);

    GDataInputStream *so = g_data_input_stream_new (g_subprocess_get_stdout_pipe (it->proc));
    GDataInputStream *se = g_data_input_stream_new (g_subprocess_get_stderr_pipe (it->proc));
    g_data_input_stream_read_line_async (so, G_PRIORITY_DEFAULT, NULL, on_progress_line, it);
    g_data_input_stream_read_line_async (se, G_PRIORITY_DEFAULT, NULL, on_stderr_line, it);
    g_subprocess_wait_async (it->proc, NULL, on_ffmpeg_exit, it);
    return TRUE;
}

/* Takes `steps`. The caller starts the next item if this one can't start. */
static void
start_job (Item *it, GPtrArray *steps)
{
    app.n_jobs++;
    it->t0 = g_get_monotonic_time ();
    it->paused = FALSE;
    it->paused_total = 0;
    it->steps = steps;
    it->step = 0;
    it->state = ST_ACTIVE;
    it->stop = FALSE;
    it->fraction = 0;
    it->n_res = 0;
    it->webp_only = FALSE;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (it->bar), 0);
    set_status (it, NULL, NULL);
    if (!spawn_step (it))
        finish_job (it, FALSE);
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

static guint64 sample_bytes;   /* the sample's output so far: only its size matters */

static void
sample_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GError) error = NULL;
    gboolean ok = g_subprocess_wait_finish (G_SUBPROCESS (source), res, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;   /* sample_stop ran: the folder changed or the window closed */
    g_clear_handle_id (&app.sample_timer, g_source_remove);
    Item *it = g_steal_pointer (&app.sample_item);
    guint64 n = sample_bytes;
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

/* Reads and discards the sample's stdout in 1 MiB chunks, counting bytes; at the end, waits for
 * the exit status. */
static void
sample_read (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GError) error = NULL;
    gssize n = g_input_stream_skip_finish (G_INPUT_STREAM (source), res, &error);
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
    if (n > 0) {
        sample_bytes += n;
        g_input_stream_skip_async (G_INPUT_STREAM (source), 1 << 20, G_PRIORITY_LOW, app.sample_cancel,
                                   sample_read, NULL);
    } else {
        g_subprocess_wait_async (app.sample_proc, app.sample_cancel, sample_done, NULL);
    }
}

static void
sample_next (void)
{
    if (!app.gpu_ok || app.closing || app.mode != VC_MODE_COMPRESS || app.items == NULL
        || app.sample_proc != NULL
        || app.probes_done < app.items->len || is_busy ())
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
    sample_bytes = 0;
    g_input_stream_skip_async (g_subprocess_get_stdout_pipe (app.sample_proc), 1 << 20, G_PRIORITY_LOW,
                               app.sample_cancel, sample_read, NULL);
}

/* Starts queued items while fewer than max_jobs() run and none is paused (the rest of the queue
 * waits for a paused one): the one with the highest "Compress next" priority, otherwise the first
 * in list order. */
static void
encode_next (void)
{
    VcOptions o = current_options ();
    while (app.n_jobs < max_jobs () && paused_jobs (NULL) == 0) {
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
    if (paused_jobs (NULL) > 0) {   /* Resume: continue the paused jobs only */
        pause_all (FALSE);
        update_controls ();
    } else {
        act_start (NULL, FALSE);
    }
}

static void
on_pause_all (void)
{
    pause_all (TRUE);
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
    gboolean job = it->state == ST_ACTIVE;
    menu_item (box, it, "media-playback-start-symbolic",
               job && it->paused ? "Resume" : verb (), on_row_play,
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

    /* line 1: name and a chip (Twin, Original in Trash); line 2: meta and its chips */
    GtkWidget *name = gtk_label_new (display);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_widget_add_css_class (name, "name");
    it->name_chip = chip ("", NULL);
    gtk_widget_set_visible (it->name_chip, FALSE);
    GtkWidget *line1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (line1), name);
    gtk_box_append (GTK_BOX (line1), it->name_chip);
    it->meta = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (it->meta), 0);
    gtk_label_set_ellipsize (GTK_LABEL (it->meta), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (it->meta, "meta");
    it->chips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *line2 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append (GTK_BOX (line2), it->meta);
    gtk_box_append (GTK_BOX (line2), it->chips);
    GtkWidget *names = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand (names, TRUE);
    gtk_widget_set_valign (names, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (names), line1);
    gtk_box_append (GTK_BOX (names), line2);

    /* status column: two right-aligned lines; the bar and percent sit in the first */
    GtkWidget *status = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign (status, GTK_ALIGN_CENTER);
    it->bar = gtk_progress_bar_new ();
    gtk_widget_set_size_request (it->bar, 150, -1);
    gtk_widget_set_valign (it->bar, GTK_ALIGN_CENTER);
    it->pct = gtk_label_new ("");
    gtk_widget_add_css_class (it->pct, "pct");
    for (guint i = 0; i < 2; i++) {
        GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign (line, i == 0 ? GTK_ALIGN_FILL : GTK_ALIGN_END);
        if (i == 0) {
            GtkWidget *push = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);   /* keeps what is shown at the right edge */
            gtk_widget_set_hexpand (push, TRUE);
            gtk_box_append (GTK_BOX (line), push);
            gtk_box_append (GTK_BOX (line), it->bar);
            gtk_box_append (GTK_BOX (line), it->pct);
        }
        it->tag[i] = gtk_label_new ("");
        gtk_widget_add_css_class (it->tag[i], "fmt");
        gtk_label_set_xalign (GTK_LABEL (it->tag[i]), 1);
        it->val[i] = gtk_label_new ("");
        gtk_label_set_xalign (GTK_LABEL (it->val[i]), 1);
        it->pill[i] = gtk_label_new ("");
        gtk_widget_set_valign (it->pill[i], GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (line), it->tag[i]);
        gtk_box_append (GTK_BOX (line), it->val[i]);
        gtk_box_append (GTK_BOX (line), it->pill[i]);
        gtk_box_append (GTK_BOX (status), line);
    }
    gtk_widget_set_size_request (status, 200, -1);
    gtk_size_group_add_widget (app.col_status, status);

    it->b_play = mini_button ("media-playback-start-symbolic", "Compress next",
                              G_CALLBACK (on_row_play), it);
    it->b_pause = mini_button ("media-playback-pause-symbolic", "Pause or hold",
                               G_CALLBACK (on_row_pause), it);
    it->b_stop = mini_button ("media-playback-stop-symbolic", "Stop or skip",
                              G_CALLBACK (on_row_stop), it);
    it->b_show = mini_button ("mt-external-symbolic", "Show in Files", G_CALLBACK (on_row_show), it);
    g_autofree char *show_body = g_strdup_printf ("Open the %s folder with this %s selected.",
                                                  vc_mode_out_dir (app.mode), noun ());
    set_tip (it->b_show, "Show in Files", NULL, show_body);
    it->b_trash = mini_button ("user-trash-symbolic", "Move original to Trash",
                               G_CALLBACK (on_row_trash), it);
    gtk_widget_add_css_class (it->b_trash, "danger");
    g_autofree char *trash_body = g_strdup_printf ("Only for finished %ss whose copy was checked. "
                                                   "Restorable from Trash.", noun ());
    set_tip (it->b_trash, "Move original to Trash", NULL, trash_body);
    GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign (actions, GTK_ALIGN_END);
    GtkWidget *b[] = { it->b_play, it->b_pause, it->b_stop, it->b_show, it->b_trash };
    for (gsize i = 0; i < G_N_ELEMENTS (b); i++)
        gtk_box_append (GTK_BOX (actions), b[i]);
    gtk_size_group_add_widget (app.col_actions, actions);

    it->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (it->box, "vrow");
    gtk_box_append (GTK_BOX (it->box), it->check);
    gtk_box_append (GTK_BOX (it->box), tile);
    gtk_box_append (GTK_BOX (it->box), names);
    gtk_box_append (GTK_BOX (it->box), status);
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

/* The header's title, its Folder / Files badge and the line under it. */
static void
update_header (void)
{
    gboolean files = app.files != NULL;
    gtk_widget_set_visible (app.title_badge, app.items != NULL);
    gtk_label_set_text (GTK_LABEL (app.title_badge), files ? "Files" : "Folder");
    if (app.items == NULL) {
        gtk_label_set_text (GTK_LABEL (app.title), "Media Tools");
        gtk_label_set_text (GTK_LABEL (app.subtitle), "No folder or files chosen");
        return;
    }
    guint n = app.items->len, hidden = app.hidden->len;
    GString *sub = g_string_new (NULL);
    if (files) {
        guint chosen = n + hidden;
        g_autofree char *t = g_strdup_printf ("%u chosen file%s", chosen, chosen == 1 ? "" : "s");
        gtk_label_set_text (GTK_LABEL (app.title), t);
        if (app.n_dirs == 1) {
            g_autofree char *dir = g_path_get_dirname (((Item *) app.items->pdata[0])->path);
            g_autofree char *where = home_relative (dir);
            g_string_printf (sub, "From %s · saves to its own %s/", where, vc_mode_out_dir (app.mode));
        } else if (app.n_dirs > 1) {
            g_string_printf (sub, "From %u folders · each saves to its own %s/", app.n_dirs,
                             vc_mode_out_dir (app.mode));
        }
    } else {
        g_autofree char *base = basename_display (app.folder);
        g_autofree char *where = home_relative (app.folder);
        gtk_label_set_text (GTK_LABEL (app.title), base);
        g_string_assign (sub, where);
    }
    if (hidden > 0) {
        if (!files)
            g_string_append_printf (sub, " · %u %s%s", n, noun (), n == 1 ? "" : "s");
        g_string_append_printf (sub, "%s%u file%s hidden (not %ss)", sub->len > 0 ? " · " : "", hidden,
                                hidden == 1 ? "" : "s", noun ());
    }
    gtk_label_set_text (GTK_LABEL (app.subtitle), sub->str);
    g_string_free (sub, TRUE);
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

    g_clear_pointer (&app.hidden, g_ptr_array_unref);
    app.hidden = g_ptr_array_new_with_free_func (g_free);
    app.n_dirs = 0;
    if (app.files != NULL) {
        g_autoptr (GHashTable) dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_autofree char *last_dir = NULL;
        for (char **f = app.files; *f != NULL; f++) {
            g_autofree char *dir = g_path_get_dirname (*f);
            g_autofree char *base = g_path_get_basename (*f);
            if (g_strcmp0 (dir, last_dir) != 0)   /* the dialog lists one folder's files together */
                remove_stale_parts (dir);
            if (vc_mode_accepts (app.mode, base)) {
                add_item (*f);
                g_hash_table_add (dirs, g_strdup (dir));
            } else {
                g_ptr_array_add (app.hidden, g_filename_display_name (base));
            }
            g_free (last_dir);
            last_dir = g_steal_pointer (&dir);
        }
        app.n_dirs = g_hash_table_size (dirs);
    } else {
        remove_stale_parts (folder);
        g_autoptr (GError) error = NULL;
        g_autoptr (GDir) dir = g_dir_open (folder, 0, &error);
        if (dir == NULL) {
            alert ("Cannot open folder", error->message, NULL);
            update_header ();
            update_speed_guide ();
            update_controls ();
            return;
        }
        /* Not recursive, by design; the output folder has no media extension so it never matches. */
        for (const char *name; (name = g_dir_read_name (dir)) != NULL;) {
            g_autofree char *path = g_build_filename (folder, name, NULL);
            if (vc_mode_accepts (app.mode, name))
                add_item (path);
            else if (name[0] != '.' && g_file_test (path, G_FILE_TEST_IS_REGULAR))
                g_ptr_array_add (app.hidden, g_filename_display_name (name));
        }
        g_ptr_array_sort_values (app.hidden, (GCompareFunc) g_utf8_collate);
    }
    update_header ();
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
    app.speed = GPOINTER_TO_UINT (user_data) + 1;   /* the buttons start after GPU */
    if (!gtk_switch_get_active (GTK_SWITCH (app.gpu)))
        app.preset = app.speed;
    on_estimate_input ();
}

static void
update_frames_note (void)
{
    if (app.mode != VC_MODE_FRAMES)
        return;
    g_autofree char *m = g_strdup_printf ("<tt>frames/&lt;name&gt;/frame_0001.%s</tt>\n"
                                          "One folder per video, numbered from 0001.", app.webp ? "webp" : "jpg");
    gtk_label_set_markup (GTK_LABEL (app.insp_note), m);
}

/* Everything that follows the mode without a reload: accent, inspector, empty page. */
static void
apply_mode_ui (void)
{
    for (guint m = 0; m < G_N_ELEMENTS (MODE_INFO); m++)
        gtk_widget_remove_css_class (GTK_WIDGET (app.win), MODE_INFO[m].css);
    gtk_widget_add_css_class (GTK_WIDGET (app.win), MODE_INFO[app.mode].css);
    gtk_stack_set_visible_child_name (GTK_STACK (app.insp_stack), MODE_INFO[app.mode].css);
    gtk_image_set_from_icon_name (GTK_IMAGE (app.insp_icon), MODE_INFO[app.mode].icon);
    gtk_image_set_from_icon_name (GTK_IMAGE (app.empty_icon), MODE_INFO[app.mode].icon);
    gtk_label_set_text (GTK_LABEL (app.insp_title), MODE_INFO[app.mode].name);
    gtk_label_set_text (GTK_LABEL (app.insp_sub), MODE_INFO[app.mode].sub);
    if (MODE_INFO[app.mode].note != NULL)
        gtk_label_set_markup (GTK_LABEL (app.insp_note), MODE_INFO[app.mode].note);
    update_frames_note ();
    gtk_label_set_text (GTK_LABEL (app.empty_sub), MODE_INFO[app.mode].empty);
    g_autofree char *hero = g_strdup_printf ("Drop %ss or a folder", noun ());
    gtk_label_set_text (GTK_LABEL (app.hero), hero);
}

static void
on_gpu (void)
{
    app.preset = gtk_switch_get_active (GTK_SWITCH (app.gpu)) ? 0 : app.speed;
    on_estimate_input ();
}

/* Locked while busy (update_controls). Rows start fresh: the list is rebuilt from the same
 * source with the new mode's filter (SPEC-app-shell.md D3). */
static void
on_mode (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void) box; (void) user_data;
    if (row == NULL || app.syncing)
        return;
    VcMode m = gtk_list_box_row_get_index (row);
    if (app.mode == m)
        return;
    app.mode = m;
    apply_mode_ui ();
    if (app.items != NULL)
        load_source (app.folder, app.files);
    else
        update_controls ();
}

static void
on_fps (GtkToggleButton *b, gpointer user_data)
{
    if (!gtk_toggle_button_get_active (b))
        return;
    app.fps_idx = GPOINTER_TO_UINT (user_data);
    on_estimate_input ();
}

static void
on_format (GtkToggleButton *b, gpointer user_data)
{
    if (!gtk_toggle_button_get_active (b))
        return;
    app.webp = GPOINTER_TO_UINT (user_data) == 0;
    gtk_label_set_text (GTK_LABEL (app.fmt_caption), app.webp ? "Smaller, plays in Safari 14 and later."
                                                              : "Bigger files that open everywhere.");
    update_frames_note ();
}

static void
on_quality (GtkRange *r, gpointer user_data)
{
    (void) user_data;
    g_autofree char *v = g_strdup_printf ("%d", (int) gtk_range_get_value (r));
    gtk_label_set_text (GTK_LABEL (app.quality_val), v);
}

static void
on_copy_fix (void)
{
    gdk_clipboard_set_text (gtk_widget_get_clipboard (app.banner),
                            gtk_label_get_text (GTK_LABEL (app.banner_code)));
    show_toast ("edit-copy-symbolic", "Copied", "Paste it into a terminal");
}

/* ---- keyboard ---- */

static gboolean
on_space (GtkWidget *w, GVariant *args, gpointer user_data)
{
    (void) w; (void) args; (void) user_data;
    if (app.n_jobs > paused_jobs (NULL))
        on_pause_all ();
    else if (gtk_widget_get_sensitive (app.start_btn) && gtk_widget_get_visible (app.start_btn))
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

/* The design's "Keep working in the background?" card: 0 Cancel (Esc), 1 Stop and Quit,
 * 2 Run in Background. */
static void
close_chosen (int choice)
{
    if (app.close_dialog != NULL)
        gtk_window_destroy (GTK_WINDOW (g_steal_pointer (&app.close_dialog)));
    if (choice == 1) {
        stop_for_quit ();
        gtk_window_destroy (app.win);
    } else if (choice == 2 && is_busy ()) {
        pause_all (FALSE);
        /* A hidden window still holds the GtkApplication, so the process and its encodes go on;
         * launching the app again or clicking a notification shows the window. */
        gtk_widget_set_visible (GTK_WIDGET (app.win), FALSE);
        g_autofree char *title = doing ();
        notify ("background", title, "You'll get a notification when it's done. Open Media Tools to see "
                                     "progress.");
    }
}

static void on_close_stop (void) { close_chosen (1); }
static void on_close_background (void) { close_chosen (2); }

/* Esc, or the dialog's own close request */
static gboolean
on_close_cancel (GtkWidget *w, GVariant *args, gpointer user_data)
{
    (void) w; (void) args; (void) user_data;
    close_chosen (0);
    return TRUE;
}

static void
show_close_dialog (void)
{
    GtkWidget *icon = gtk_image_new_from_icon_name ("mt-bolt-symbolic");
    gtk_widget_add_css_class (icon, "brand-icon");
    gtk_widget_add_css_class (icon, "big");
    gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
    GtkWidget *title = gtk_label_new ("Keep working in the background?");
    gtk_widget_add_css_class (title, "dialog-title");
    GtkWidget *body = gtk_label_new ("The window closes and the work goes on. You'll get a notification "
                                     "when it's done.");
    gtk_widget_add_css_class (body, "dialog-body");
    gtk_label_set_wrap (GTK_LABEL (body), TRUE);
    gtk_label_set_justify (GTK_LABEL (body), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars (GTK_LABEL (body), 40);
    GtkWidget *stop = gtk_button_new_with_mnemonic ("_Stop and Quit");
    gtk_widget_add_css_class (stop, "destructive");
    g_signal_connect_swapped (stop, "clicked", G_CALLBACK (on_close_stop), NULL);
    GtkWidget *bg = gtk_button_new_with_mnemonic ("_Run in Background");
    gtk_widget_add_css_class (bg, "suggested-action");
    g_signal_connect_swapped (bg, "clicked", G_CALLBACK (on_close_background), NULL);
    GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
    gtk_widget_set_margin_top (buttons, 8);
    gtk_box_append (GTK_BOX (buttons), stop);
    gtk_box_append (GTK_BOX (buttons), bg);
    GtkWidget *card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (card, "close-card");
    gtk_box_append (GTK_BOX (card), icon);
    gtk_box_append (GTK_BOX (card), title);
    gtk_box_append (GTK_BOX (card), body);
    gtk_box_append (GTK_BOX (card), buttons);

    GtkWindow *d = GTK_WINDOW (gtk_window_new ());
    app.close_dialog = GTK_WIDGET (d);
    gtk_widget_add_css_class (GTK_WIDGET (d), "close-dialog");
    gtk_widget_add_css_class (GTK_WIDGET (d), MODE_INFO[app.mode].css);   /* the accent */
    if (gtk_widget_has_css_class (GTK_WIDGET (app.win), "dark"))
        gtk_widget_add_css_class (GTK_WIDGET (d), "dark");
    gtk_window_set_title (d, "Keep working in the background?");
    gtk_window_set_decorated (d, FALSE);
    gtk_window_set_resizable (d, FALSE);
    gtk_window_set_modal (d, TRUE);
    gtk_window_set_transient_for (d, app.win);
    gtk_window_set_default_widget (d, bg);
    gtk_window_set_child (d, card);
    g_signal_connect (d, "close-request", G_CALLBACK (on_close_cancel), NULL);
    GtkEventController *keys = gtk_shortcut_controller_new ();
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
        gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("Escape"),
                          gtk_callback_action_new (on_close_cancel, NULL, NULL)));
    gtk_widget_add_controller (GTK_WIDGET (d), keys);
    gtk_window_present (d);
    gtk_widget_grab_focus (bg);
}

static gboolean
on_close (GtkWindow *win, gpointer user_data)
{
    (void) win; (void) user_data;
    if (is_busy ()) {
        if (app.close_dialog == NULL)
            show_close_dialog ();
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
    for (guint i = 0; app.n_jobs > 0 && i < app.items->len; i++) {
        Item *it = app.items->pdata[i];
        if (it->state != ST_ACTIVE || it->proc == NULL)
            continue;
        g_subprocess_force_exit (it->proc);   /* SIGKILL works on a paused ffmpeg too */
        g_subprocess_wait (it->proc, NULL, NULL);
        for (guint s = 0; s < it->steps->len; s++)
            vc_remove_part (((VcStep *) it->steps->pdata[s])->part);
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
        alert ("Media Tools can't run", problem, quit_after_alert);
}

static void
gpu_test_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    if (!g_subprocess_wait_check_finish (proc, res, NULL) || app.closing)
        return;   /* no working GPU encoder: the GPU speed stays hidden */
    app.gpu_ok = TRUE;
    gtk_widget_set_visible (gtk_widget_get_parent (app.gpu), TRUE);
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

/* The v3 design (Figma "v3 – Media Tools"): colours are variables, so a mode's accent and the dark
 * appearance each change one place. */
#define DARK_VARS \
    "  --text: #f5f5f7; --text2: #a1a1a6; --text3: #6e6e73; --bg: #1e1e20; --side: #28282b;" \
    "  --insp: #232326; --fill: #323236; --line: #38383c; --card: #2c2c2f; --sel: #1e1e20;" \
    "  --seg-on: #5a5a5f; --shadow: rgba(0,0,0,0.4); --green: #32d74b; --green-bg: #17351f;" \
    "  --amber: #ffb340; --amber-bg: #3a2a12; --red: #ff6961; --red-bg: #3d1c1e; --teal: #5ac8dd;" \
    "  --teal-bg: #123238;"
static const char CSS[] =
    "window.mt, window.close-dialog {"
    "  --accent: #0a84ff; --text: #1d1d1f; --text2: #6e6e73; --text3: #a1a1a6;"
    "  --bg: #ffffff; --side: #f3f3f6; --insp: #fafafc; --fill: #f2f2f5; --line: #e6e6eb;"
    "  --card: #ffffff; --sel: #ffffff; --seg-on: #ffffff; --shadow: rgba(0,0,0,0.08);"
    "  --green: #1f8f4e; --green-bg: #e6f6ec; --amber: #b25e00; --amber-bg: #fff3e0;"
    "  --red: #d70015; --red-bg: #ffe9eb; --teal: #0f7f91; --teal-bg: #e3f6f9;"
    "  color: var(--text); background: var(--bg); font-size: 13px; font-family: Inter, sans-serif; }"
    "tooltip { font-family: Inter, sans-serif; }"
    "@media (prefers-color-scheme: dark) { window.mt, window.close-dialog {" DARK_VARS "} }"
    /* a dark theme chosen the older way (prefer-dark-theme) doesn't set the media query: sync_dark */
    "window.mt.dark, window.close-dialog.dark {" DARK_VARS "}"
    "list.rows, list.modes { color: var(--text); }"
    /* as specific as window.mt, and later, so the mode wins */
    "window.web { --accent: #5e5ce6; } window.frames { --accent: #ff9f0a; } window.images { --accent: #30b0c7; }"

    /* buttons */
    "button.suggested-action { background: var(--accent); color: white; border: none; box-shadow: none;"
    "  border-radius: 8px; font-weight: 600; }"
    "button.suggested-action:hover { background: color-mix(in srgb, var(--accent) 88%, black); }"
    "button.outline { background: var(--card); color: var(--text); border: 1px solid var(--line);"
    "  border-radius: 8px; box-shadow: none; padding: 5px 12px; font-weight: 500; }"
    "button.outline:hover { background: var(--fill); }"
    "button.outline:disabled { color: var(--text3); background: var(--card); }"
    "button.round { min-width: 30px; min-height: 30px; padding: 0; border-radius: 999px; border: none;"
    "  box-shadow: none; background: var(--fill); color: var(--text2); -gtk-icon-size: 12px; }"
    "button.round.big { min-width: 34px; min-height: 34px; -gtk-icon-size: 14px; }"
    "button.round.suggested-action { background: var(--accent); color: white; }"
    "button.round:disabled { color: var(--text3); background: var(--fill); }"
    "button.round.suggested-action:disabled { background: color-mix(in srgb, var(--accent) 35%, var(--bg));"
    "  color: white; }"
    "button.mini { min-width: 26px; min-height: 26px; padding: 0; border-radius: 999px; border: none;"
    "  box-shadow: none; background: var(--fill); color: var(--text2); -gtk-icon-size: 12px; }"
    "button.mini:hover { background: color-mix(in srgb, var(--text) 12%, transparent); }"
    "button.mini.suggested-action { background: var(--accent); color: white; }"
    "button.mini.danger { color: var(--red); }"
    "separator.tool-sep { background: var(--line); min-width: 1px; margin: 14px 4px; }"

    /* sidebar */
    ".sidebar { background: var(--side); border-right: 1px solid var(--line); }"
    ".sidebar windowcontrols { margin: 14px 0 0 16px; }"
    ".sidebar windowcontrols button { min-width: 12px; min-height: 12px; padding: 0; margin: 0 8px 0 0;"
    "  border-radius: 999px; border: none; box-shadow: none; background: #ff5f57; }"
    ".sidebar windowcontrols button.minimize { background: #febc2e; }"
    ".sidebar windowcontrols button.maximize { background: #28c840; }"
    ".sidebar windowcontrols button image { background: none; box-shadow: none; padding: 0; min-width: 0;"
    "  min-height: 0; -gtk-icon-size: 8px; color: rgba(0,0,0,0.55); opacity: 0; }"
    ".sidebar windowcontrols:hover button image { opacity: 1; }"
    ".brand-icon { background: linear-gradient(135deg, #3b82f6, #8b5cf6 55%, #f59e0b); color: white;"
    "  border-radius: 8px; min-width: 30px; min-height: 30px; -gtk-icon-size: 16px; }"
    ".brand-icon.big { min-width: 54px; min-height: 54px; border-radius: 14px; -gtk-icon-size: 26px; }"
    ".brand-title { font-weight: 700; font-size: 14px; }"
    ".brand-sub, .mode-sub { font-size: 11px; color: var(--text2); }"
    ".side-head, .colhead, .sect { font-size: 11px; font-weight: 600; letter-spacing: 0.5px;"
    "  color: var(--text3); }"
    "list.modes { background: transparent; }"
    "list.modes > row { padding: 6px 8px; border-radius: 10px; margin-bottom: 2px; }"
    "list.modes > row:selected { background: var(--sel); color: var(--text); box-shadow: 0 1px 3px var(--shadow); }"
    "list.modes > row:hover:not(:selected) { background: color-mix(in srgb, var(--text) 4%, transparent); }"
    "list.modes > row:disabled { opacity: 0.45; }"
    ".mode-tile { border-radius: 7px; min-width: 28px; min-height: 28px; color: white; -gtk-icon-size: 15px; }"
    ".mode-tile.t-compress { background: #0a84ff; } .mode-tile.t-web { background: #5e5ce6; }"
    ".mode-tile.t-frames { background: #ff9f0a; } .mode-tile.t-images { background: #30b0c7; }"
    ".mode-name { font-weight: 600; }"
    ".mode-count { font-size: 12px; font-weight: 600; color: var(--text2); }"
    ".mode-lock { color: var(--text3); -gtk-icon-size: 11px; }"
    ".mode-dot { background: #ff9f0a; border-radius: 999px; min-width: 8px; min-height: 8px; }"
    ".tools { background: var(--card); border-radius: 10px; padding: 10px 12px;"
    "  box-shadow: 0 1px 2px var(--shadow); }"
    ".tools-title { font-weight: 600; font-size: 12px; }"
    ".tool-name { font-size: 11.5px; color: var(--text2); }"
    ".tool-state { font-size: 11.5px; font-weight: 500; color: var(--text3); }"
    ".tool-state.ok { color: var(--green); } .tool-state.missing { color: var(--amber); font-weight: 600; }"

    /* header and footer */
    "headerbar.top { background: var(--bg); border-bottom: 1px solid var(--line); box-shadow: none;"
    "  min-height: 59px; padding: 0 10px 0 12px; }"
    ".h-title { font-weight: 700; font-size: 15px; }"
    ".h-sub { font-size: 12px; color: var(--text2); }"
    ".badge { background: var(--fill); color: var(--text2); border-radius: 4px; padding: 0 5px;"
    "  font-size: 10.5px; font-weight: 700; }"
    ".footer { border-top: 1px solid var(--line); padding: 0 16px; min-height: 44px; }"
    ".footer label.error { color: var(--red); font-weight: 600; }"
    ".foot-right { color: var(--text2); }"

    /* empty page */
    ".dropzone { border: 1px dashed color-mix(in srgb, var(--text) 25%, transparent); border-radius: 14px; }"
    ".drop-icon { background: linear-gradient(160deg, color-mix(in srgb, var(--accent) 75%, white), var(--accent));"
    "  color: white; border-radius: 18px; min-width: 76px; min-height: 76px; -gtk-icon-size: 34px;"
    "  box-shadow: 0 6px 18px color-mix(in srgb, var(--accent) 35%, transparent); }"
    ".hero { font-size: 22px; font-weight: 700; }"
    ".empty-sub { color: var(--text2); font-size: 14px; }"
    ".feature { background: var(--fill); border-radius: 999px; padding: 4px 10px; font-size: 12px;"
    "  color: var(--text2); }"
    ".feature image { -gtk-icon-size: 11px; }"

    /* list */
    ".colhead-row { padding: 9px 16px; border-bottom: 1px solid var(--line); }"
    "list.rows { background: transparent; }"
    "list.rows > row { padding: 0; border-bottom: 1px solid var(--line); }"
    "list.rows > row:hover { background: color-mix(in srgb, var(--text) 3%, transparent); }"
    "list.rows > row.active { background: color-mix(in srgb, var(--accent) 7%, transparent); }"
    "list.rows > row.checked { background: color-mix(in srgb, var(--accent) 11%, transparent); }"
    ".vrow { padding: 8px 16px; min-height: 38px; }"
    ".name { font-weight: 500; font-size: 13.5px; }"
    ".meta { font-size: 12px; color: var(--text2); }"
    ".tile { border-radius: 8px; min-width: 38px; min-height: 38px; color: white; -gtk-icon-size: 12px; }"
    ".tile-0 { background: linear-gradient(135deg, #ff9a9e, #f5576c); }"
    ".tile-1 { background: linear-gradient(135deg, #a18cd1, #6a5acd); }"
    ".tile-2 { background: linear-gradient(135deg, #43c6ac, #191654); }"
    ".tile-3 { background: linear-gradient(135deg, #e6a57e, #c06c84); }"
    ".tile-4 { background: linear-gradient(135deg, #00c6ff, #0072ff); }"
    ".tile-5 { background: linear-gradient(135deg, #f6d365, #fda085); }"
    ".tile-6 { background: linear-gradient(135deg, #a1c4fd, #6b7fae); }"
    ".tile-7 { background: linear-gradient(135deg, #84fab0, #2f9e7a); }"
    "checkbutton check:checked, checkbutton check:indeterminate { background-color: var(--accent);"
    "  border-color: var(--accent); color: white; }"
    ".pill { border-radius: 6px; padding: 1px 7px; font-size: 11.5px; font-weight: 600; }"
    ".pill.estimate { border: 1px dashed var(--text3); }"
    ".pill.done { background: var(--green-bg); color: var(--green); }"
    ".pill.bigger, .pill.hold { background: var(--amber-bg); color: var(--amber); }"
    ".pill.neutral { background: var(--fill); color: var(--text2); }"
    ".pill.error { background: var(--red-bg); color: var(--red); }"
    ".chip { border-radius: 4px; padding: 0 5px; font-size: 10.5px; font-weight: 700; }"
    ".chip.hdr, .chip.kept { background: var(--amber-bg); color: var(--amber); }"
    ".chip.res, .chip.trashed { background: var(--fill); color: var(--text2); }"
    ".chip.twin { background: var(--teal-bg); color: var(--teal); }"
    ".fmt { font-size: 10px; font-weight: 700; color: var(--text3); }"
    ".pct { font-weight: 700; }"
    "progressbar trough { min-height: 5px; border-radius: 3px; background: var(--fill); border: none; }"
    "progressbar progress { min-height: 5px; border-radius: 3px; background: var(--accent); border: none; margin: 0; }"
    "progressbar.time progress { background: #ff9f0a; }"
    "progressbar.size progress { background: var(--green); }"
    ".summary { border: 1px solid var(--line); border-radius: 12px; background: var(--card);"
    "  padding: 12px 16px; }"
    ".sum-icon { background: var(--accent); color: white; border-radius: 999px; min-width: 40px;"
    "  min-height: 40px; -gtk-icon-size: 20px; }"
    ".sum-title { font-size: 17px; font-weight: 700; }"
    ".sum-sub, .stat-cap { font-size: 12px; color: var(--text2); }"
    ".stat-cap { font-size: 11px; }"
    ".stat { font-size: 16px; font-weight: 700; }"
    ".stat.good { color: var(--green); }"
    ".banner { background: var(--amber-bg); border: 1px solid color-mix(in srgb, var(--amber) 25%, transparent);"
    "  border-radius: 10px; padding: 10px 12px 10px 14px; }"
    ".banner > image { color: var(--amber); -gtk-icon-size: 18px; }"
    ".banner-title { font-weight: 600; }"
    ".banner-sub { font-size: 12px; color: var(--text2); }"
    ".code { background: color-mix(in srgb, var(--card) 75%, transparent); border-radius: 6px;"
    "  padding: 2px 2px 2px 8px; }"
    ".code label { font-family: monospace; font-size: 12px; }"
    ".code button { min-width: 22px; min-height: 22px; padding: 0; background: none; border: none;"
    "  box-shadow: none; -gtk-icon-size: 12px; color: var(--text2); }"

    /* inspector */
    ".inspector { background: var(--insp); border-left: 1px solid var(--line); }"
    ".insp-icon { background: var(--accent); color: white; border-radius: 8px; min-width: 34px;"
    "  min-height: 34px; -gtk-icon-size: 17px; }"
    ".insp-title { font-weight: 700; font-size: 14px; }"
    ".insp-sub, .hint, .opt-sub { font-size: 11.5px; color: var(--text2); }"
    ".opt-title { font-weight: 500; }"
    ".note { background: var(--fill); border-radius: 8px; padding: 9px 10px; font-size: 11.5px;"
    "  color: var(--text2); }"
    ".check-line { font-size: 12px; }"
    ".check-line image { color: var(--text2); -gtk-icon-size: 12px; }"
    ".err-line { color: var(--red); font-size: 11.5px; }"
    ".err-line image { -gtk-icon-size: 12px; }"
    ".seg { background: var(--fill); border-radius: 8px; padding: 2px; }"
    ".seg button { background: none; box-shadow: none; border: none; min-height: 24px; padding: 0 2px;"
    "  border-radius: 6px; font-size: 12px; color: var(--text2); }"
    ".seg button:checked { background: var(--seg-on); color: var(--text); font-weight: 600;"
    "  box-shadow: 0 1px 2px var(--shadow); }"
    ".seg button:disabled { opacity: 0.45; }"
    "menubutton.link > button { color: var(--accent); background: none; box-shadow: none; border: none;"
    "  padding: 0; min-height: 0; font-size: 12px; font-weight: 500; }"
    "menubutton.link image { -gtk-icon-size: 12px; }"
    ".inspector dropdown > button { background: var(--card); border: 1px solid var(--line);"
    "  border-radius: 8px; box-shadow: none; min-height: 30px; padding: 0 10px; color: var(--text); }"
    ".inspector entry { background: var(--card); border: 1px solid var(--line); border-radius: 8px;"
    "  min-height: 30px; box-shadow: none; color: var(--text); }"
    ".inspector entry.error { border: 2px solid var(--red); color: var(--red); }"
    "switch { background: color-mix(in srgb, var(--text) 16%, transparent); border: none; border-radius: 999px;"
    "  padding: 2px; box-shadow: none; }"
    "switch:checked { background: var(--accent); }"
    "switch slider { min-width: 16px; min-height: 16px; margin: 0; background: white; border: none;"
    "  border-radius: 999px; box-shadow: 0 1px 2px rgba(0,0,0,0.25); }"
    "switch image { opacity: 0; }"
    "scale trough { background: var(--fill); min-height: 4px; border-radius: 2px; border: none; }"
    "scale highlight { background: var(--accent); border-radius: 2px; border: none; margin: 0; }"
    "scale slider { background: white; min-width: 20px; min-height: 20px; margin: -8px;"
    "  border-radius: 999px; border: none; box-shadow: 0 1px 3px rgba(0,0,0,0.3); }"

    /* floating: tooltip, toast, selection bar, close dialog */
    "tooltip { background: rgba(29,29,31,0.96); border-radius: 8px; padding: 6px 10px; }"
    "tooltip label { color: white; }"
    ".toast { background: rgba(29,29,31,0.96); color: white; border-radius: 10px; padding: 9px 14px;"
    "  box-shadow: 0 6px 20px rgba(0,0,0,0.3); }"
    ".overlaybar { background: rgba(29,29,31,0.96); color: white; border-radius: 999px;"
    "  padding: 6px 8px 6px 16px; box-shadow: 0 6px 20px rgba(0,0,0,0.3); }"
    ".overlaybar button { color: white; background: alpha(white, 0.12); border: none; box-shadow: none; }"
    ".overlaybar button:disabled { color: alpha(white, 0.35); background: alpha(white, 0.06); }"
    ".overlaybar button.suggested-action { background: var(--accent); }"
    "window.close-dialog { background: var(--card); border-radius: 14px; }"
    ".close-card { padding: 22px 20px 18px; min-width: 300px; }"
    ".dialog-title { font-weight: 700; font-size: 15px; margin-top: 4px; }"
    ".dialog-body { font-size: 12.5px; color: var(--text2); }"
    ".close-card button { border-radius: 8px; min-height: 30px; box-shadow: none; border: none; font-weight: 600; }"
    ".close-card button.destructive { background: var(--fill); color: var(--red); }"
    ".guide { padding: 8px; }"
    ".guide-row { padding: 8px 10px; border-radius: 8px; }"
    ".guide-row.current { background: color-mix(in srgb, var(--accent) 14%, transparent); }"
    ".guide-row.current > label:first-child { color: var(--accent); font-weight: 600; }"
    ".caption { font-size: 12px; opacity: 0.6; }";

static GtkWidget *
label_button (const char *icon, const char *mnemonic, GCallback clicked)
{
    GtkWidget *b = gtk_button_new ();
    GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
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
    gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
    gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect (b, "clicked", clicked, NULL);
    return b;
}

/* The Max width dropdown that web video, frames and images each have, at 1920. */
static GtkWidget *
width_dropdown (void)
{
    const char *widths[] = { "Original", "3840 px", "2560 px", "1920 px", "1280 px", "960 px", "640 px", NULL };
    G_STATIC_ASSERT (G_N_ELEMENTS (widths) == G_N_ELEMENTS (WEB_WIDTHS) + 1);
    GtkWidget *dd = gtk_drop_down_new_from_strings (widths);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (dd), WEB_DEFAULT_WIDTH);
    gtk_accessible_update_property (GTK_ACCESSIBLE (dd), GTK_ACCESSIBLE_PROPERTY_LABEL, "Max width", -1);
    gtk_widget_set_tooltip_text (dd, "Scale wider videos down to this width, keeping the aspect ratio. "
                                     "Narrower videos are never enlarged.");
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
build_sidebar (void)
{
    GtkWidget *controls = gtk_window_controls_new (GTK_PACK_START);
    gtk_window_controls_set_decoration_layout (GTK_WINDOW_CONTROLS (controls), "close,minimize,maximize:");
    gtk_widget_set_halign (controls, GTK_ALIGN_START);
    GtkWidget *brand_icon = gtk_image_new_from_icon_name ("mt-bolt-symbolic");
    gtk_widget_add_css_class (brand_icon, "brand-icon");
    gtk_widget_set_valign (brand_icon, GTK_ALIGN_CENTER);
    GtkWidget *brand_text = vbox (0);
    gtk_box_append (GTK_BOX (brand_text), styled_label ("Media Tools", "brand-title", 0));
    gtk_box_append (GTK_BOX (brand_text), styled_label ("Video & image toolkit", "brand-sub", 0));
    GtkWidget *brand = hbox (10);
    gtk_widget_set_margin_start (brand, 18);
    gtk_widget_set_margin_top (brand, 18);
    gtk_box_append (GTK_BOX (brand), brand_icon);
    gtk_box_append (GTK_BOX (brand), brand_text);
    GtkWidget *top = vbox (0);
    gtk_box_append (GTK_BOX (top), controls);
    gtk_box_append (GTK_BOX (top), brand);
    GtkWidget *handle = gtk_window_handle_new ();   /* drag the window by the sidebar's top */
    gtk_window_handle_set_child (GTK_WINDOW_HANDLE (handle), top);

    GtkWidget *head = styled_label ("MODES", "side-head", 0);
    gtk_widget_set_margin_start (head, 20);
    gtk_widget_set_margin_top (head, 18);
    gtk_widget_set_margin_bottom (head, 6);

    app.mode_list = gtk_list_box_new ();
    gtk_widget_add_css_class (app.mode_list, "modes");
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.mode_list), GTK_ACCESSIBLE_PROPERTY_LABEL, "Mode", -1);
    gtk_widget_set_margin_start (app.mode_list, 10);
    gtk_widget_set_margin_end (app.mode_list, 10);
    for (guint m = 0; m < G_N_ELEMENTS (MODE_INFO); m++) {
        GtkWidget *tile = gtk_image_new_from_icon_name (MODE_INFO[m].icon);
        gtk_widget_add_css_class (tile, "mode-tile");
        g_autofree char *t = g_strdup_printf ("t-%s", MODE_INFO[m].css);
        gtk_widget_add_css_class (tile, t);
        gtk_widget_set_valign (tile, GTK_ALIGN_CENTER);
        GtkWidget *texts = vbox (0);
        gtk_widget_set_hexpand (texts, TRUE);
        gtk_box_append (GTK_BOX (texts), styled_label (MODE_INFO[m].name, "mode-name", 0));
        gtk_box_append (GTK_BOX (texts), styled_label (MODE_INFO[m].sub, "mode-sub", 0));
        app.mode_count[m] = styled_label ("", "mode-count", 1);
        app.mode_lock[m] = gtk_image_new_from_icon_name ("changes-prevent-symbolic");
        gtk_widget_add_css_class (app.mode_lock[m], "mode-lock");
        app.mode_dot[m] = hbox (0);
        gtk_widget_add_css_class (app.mode_dot[m], "mode-dot");
        gtk_widget_set_valign (app.mode_dot[m], GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text (app.mode_dot[m], "A tool this mode needs is missing");
        GtkWidget *row = hbox (10);
        gtk_box_append (GTK_BOX (row), tile);
        gtk_box_append (GTK_BOX (row), texts);
        gtk_box_append (GTK_BOX (row), app.mode_count[m]);
        gtk_box_append (GTK_BOX (row), app.mode_lock[m]);
        gtk_box_append (GTK_BOX (row), app.mode_dot[m]);
        gtk_list_box_append (GTK_LIST_BOX (app.mode_list), row);
        app.mode_row[m] = gtk_widget_get_parent (row);
    }
    app.syncing = TRUE;
    gtk_list_box_select_row (GTK_LIST_BOX (app.mode_list), GTK_LIST_BOX_ROW (app.mode_row[0]));
    app.syncing = FALSE;
    g_signal_connect (app.mode_list, "row-selected", G_CALLBACK (on_mode), NULL);

    GtkWidget *tools = gtk_grid_new ();
    gtk_widget_add_css_class (tools, "tools");
    gtk_grid_set_row_spacing (GTK_GRID (tools), 3);
    gtk_widget_set_margin_start (tools, 10);
    gtk_widget_set_margin_end (tools, 10);
    gtk_widget_set_margin_bottom (tools, 12);
    GtkWidget *tools_title = styled_label ("Tools", "tools-title", 0);
    gtk_widget_set_margin_bottom (tools_title, 4);
    gtk_grid_attach (GTK_GRID (tools), tools_title, 0, 0, 2, 1);
    const char *names[] = { "ffmpeg", "x265 · x264 · zscale", "ImageMagick", "pngquant" };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        GtkWidget *n = styled_label (names[i], "tool-name", 0);
        gtk_widget_set_hexpand (n, TRUE);
        app.tool_state[i] = styled_label ("", "tool-state", 1);
        gtk_grid_attach (GTK_GRID (tools), n, 0, i + 1, 1, 1);
        gtk_grid_attach (GTK_GRID (tools), app.tool_state[i], 1, i + 1, 1, 1);
    }

    GtkWidget *side = vbox (0);
    gtk_widget_add_css_class (side, "sidebar");
    gtk_widget_set_size_request (side, 210, -1);
    gtk_box_append (GTK_BOX (side), handle);
    gtk_box_append (GTK_BOX (side), head);
    gtk_box_append (GTK_BOX (side), app.mode_list);
    GtkWidget *space = vbox (0);
    gtk_widget_set_vexpand (space, TRUE);
    gtk_box_append (GTK_BOX (side), space);
    gtk_box_append (GTK_BOX (side), tools);
    return side;
}

static GtkWidget *
build_header (void)
{
    app.title = styled_label ("Media Tools", "h-title", 0);
    gtk_label_set_ellipsize (GTK_LABEL (app.title), PANGO_ELLIPSIZE_END);
    app.title_badge = styled_label ("Folder", "badge", 0.5);
    gtk_widget_set_valign (app.title_badge, GTK_ALIGN_CENTER);
    GtkWidget *line = hbox (8);
    gtk_box_append (GTK_BOX (line), app.title);
    gtk_box_append (GTK_BOX (line), app.title_badge);
    app.subtitle = styled_label ("No folder or files chosen", "h-sub", 0);
    gtk_label_set_ellipsize (GTK_LABEL (app.subtitle), PANGO_ELLIPSIZE_START);
    GtkWidget *titles = vbox (1);
    gtk_widget_set_valign (titles, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (titles), line);
    gtk_box_append (GTK_BOX (titles), app.subtitle);

    GtkWidget *header = gtk_header_bar_new ();
    gtk_widget_add_css_class (header, "top");
    gtk_header_bar_set_show_title_buttons (GTK_HEADER_BAR (header), FALSE);   /* they're in the sidebar */
    gtk_header_bar_set_title_widget (GTK_HEADER_BAR (header), hbox (0));
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), titles);

    app.files_btn = label_button ("document-open-symbolic", "Choose _Files…", G_CALLBACK (on_choose_files));
    app.choose_btn = label_button ("folder-symbolic", "_Choose Folder…", G_CALLBACK (on_choose));
    gtk_widget_add_css_class (app.files_btn, "outline");
    gtk_widget_add_css_class (app.choose_btn, "outline");
    gtk_widget_set_valign (app.files_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (app.choose_btn, GTK_ALIGN_CENTER);
    app.tool_sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class (app.tool_sep, "tool-sep");
    app.start_btn = icon_button ("media-playback-start-symbolic", "Start", "round", G_CALLBACK (on_start_all));
    gtk_widget_add_css_class (app.start_btn, "big");
    gtk_widget_add_css_class (app.start_btn, "suggested-action");
    app.pause_btn = icon_button ("media-playback-pause-symbolic", "Pause", "round", G_CALLBACK (on_pause_all));
    app.stop_btn = icon_button ("media-playback-stop-symbolic", "Stop", "round", G_CALLBACK (on_stop_all));
    set_tip (app.stop_btn, "Stop", "Esc", "Stop the batch. The unfinished file is removed; originals stay.");
    app.show_btn = label_button ("mt-external-symbolic", "_Show in Files", G_CALLBACK (on_finished_show));
    gtk_widget_add_css_class (app.show_btn, "suggested-action");
    gtk_widget_set_valign (app.show_btn, GTK_ALIGN_CENTER);
    GtkWidget *end[] = { app.stop_btn, app.pause_btn, app.start_btn, app.show_btn, app.tool_sep,
                         app.choose_btn, app.files_btn };
    for (gsize i = 0; i < G_N_ELEMENTS (end); i++)
        gtk_header_bar_pack_end (GTK_HEADER_BAR (header), end[i]);
    return header;
}

static GtkWidget *
build_empty_page (void)
{
    app.empty_icon = gtk_image_new_from_icon_name (MODE_INFO[0].icon);
    gtk_widget_add_css_class (app.empty_icon, "drop-icon");
    gtk_widget_set_halign (app.empty_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (app.empty_icon, 10);
    GtkWidget *choose = label_button ("folder-symbolic", "_Choose Folder…", G_CALLBACK (on_choose));
    gtk_widget_add_css_class (choose, "suggested-action");
    GtkWidget *files = label_button ("document-open-symbolic", "Choose _Files…", G_CALLBACK (on_choose_files));
    gtk_widget_add_css_class (files, "outline");
    g_object_bind_property (app.choose_btn, "sensitive", choose, "sensitive", G_BINDING_SYNC_CREATE);
    g_object_bind_property (app.files_btn, "sensitive", files, "sensitive", G_BINDING_SYNC_CREATE);
    GtkWidget *buttons = hbox (8);
    gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (buttons), choose);
    gtk_box_append (GTK_BOX (buttons), files);
    GtkWidget *features = hbox (8);
    gtk_widget_set_halign (features, GTK_ALIGN_CENTER);
    const char *f[][2] = { { "mt-sliders-symbolic", "Quality set automatically" },
                           { "changes-prevent-symbolic", "Originals never changed" },
                           { "media-playback-stop-symbolic", "Cancel any time" } };
    for (gsize i = 0; i < G_N_ELEMENTS (f); i++) {
        GtkWidget *c = hbox (6);
        gtk_widget_add_css_class (c, "feature");
        gtk_box_append (GTK_BOX (c), gtk_image_new_from_icon_name (f[i][0]));
        gtk_box_append (GTK_BOX (c), gtk_label_new (f[i][1]));
        gtk_box_append (GTK_BOX (features), c);
    }
    GtkWidget *inner = vbox (10);
    gtk_widget_set_valign (inner, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand (inner, TRUE);
    gtk_box_append (GTK_BOX (inner), app.empty_icon);
    app.hero = styled_label ("Drop videos or a folder", "hero", 0.5);
    gtk_box_append (GTK_BOX (inner), app.hero);
    app.empty_sub = styled_label ("", "empty-sub", 0.5);
    gtk_label_set_wrap (GTK_LABEL (app.empty_sub), TRUE);
    gtk_label_set_justify (GTK_LABEL (app.empty_sub), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars (GTK_LABEL (app.empty_sub), 52);
    gtk_widget_set_margin_bottom (app.empty_sub, 8);
    gtk_box_append (GTK_BOX (inner), app.empty_sub);
    gtk_box_append (GTK_BOX (inner), buttons);
    gtk_widget_set_margin_top (features, 18);
    gtk_box_append (GTK_BOX (inner), features);
    GtkWidget *zone = vbox (0);
    gtk_widget_add_css_class (zone, "dropzone");
    gtk_widget_set_margin_start (zone, 22);
    gtk_widget_set_margin_end (zone, 22);
    gtk_widget_set_margin_top (zone, 22);
    gtk_widget_set_margin_bottom (zone, 22);
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
    gtk_box_append (GTK_BOX (v), styled_label (caption, "stat-cap", 1));
    return v;
}

static GtkWidget *
build_list_page (void)
{
    /* finished summary */
    app.sum_icon = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_widget_add_css_class (app.sum_icon, "sum-icon");
    gtk_widget_set_valign (app.sum_icon, GTK_ALIGN_CENTER);
    app.sum_title = styled_label ("", "sum-title", 0);
    app.sum_sub = styled_label ("", "sum-sub", 0);
    gtk_label_set_ellipsize (GTK_LABEL (app.sum_sub), PANGO_ELLIPSIZE_END);
    GtkWidget *texts = vbox (1);
    gtk_widget_set_hexpand (texts, TRUE);
    gtk_widget_set_valign (texts, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (texts), app.sum_title);
    gtk_box_append (GTK_BOX (texts), app.sum_sub);
    app.summary = hbox (24);
    gtk_widget_add_css_class (app.summary, "summary");
    gtk_widget_set_margin_start (app.summary, 16);
    gtk_widget_set_margin_end (app.summary, 16);
    gtk_widget_set_margin_top (app.summary, 12);
    gtk_widget_set_margin_bottom (app.summary, 8);
    GtkWidget *lead = hbox (14);
    gtk_widget_set_hexpand (lead, TRUE);
    gtk_box_append (GTK_BOX (lead), app.sum_icon);
    gtk_box_append (GTK_BOX (lead), texts);
    gtk_box_append (GTK_BOX (app.summary), lead);
    gtk_box_append (GTK_BOX (app.summary), stat_box (&app.sum_before, "Before", FALSE));
    gtk_box_append (GTK_BOX (app.summary), stat_box (&app.sum_after, "After", FALSE));
    gtk_box_append (GTK_BOX (app.summary), stat_box (&app.sum_pct, "Smaller", TRUE));
    app.sum_pct_cap = gtk_widget_get_last_child (gtk_widget_get_last_child (app.summary));
    gtk_widget_set_visible (app.summary, FALSE);

    /* column header, aligned with the rows by size groups */
    app.select_all = gtk_check_button_new ();
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.select_all), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    "Select all", -1);
    g_signal_connect (app.select_all, "toggled", G_CALLBACK (on_select_all), NULL);
    GtkWidget *tile_space = hbox (0);
    gtk_widget_set_size_request (tile_space, 38, -1);
    GtkWidget *name = styled_label ("NAME", "colhead", 0);
    gtk_widget_set_hexpand (name, TRUE);
    app.size_head = styled_label ("SIZE → ESTIMATE", "colhead", 1);
    gtk_size_group_add_widget (app.col_status, app.size_head);
    GtkWidget *actions_space = hbox (0);
    gtk_size_group_add_widget (app.col_actions, actions_space);
    GtkWidget *head = hbox (12);
    gtk_widget_add_css_class (head, "colhead-row");
    gtk_box_append (GTK_BOX (head), app.select_all);
    gtk_box_append (GTK_BOX (head), tile_space);
    gtk_box_append (GTK_BOX (head), name);
    gtk_box_append (GTK_BOX (head), app.size_head);
    gtk_box_append (GTK_BOX (head), actions_space);

    app.list = gtk_list_box_new ();
    gtk_widget_add_css_class (app.list, "rows");
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (app.list), GTK_SELECTION_NONE);
    GtkWidget *placeholder = styled_label ("Nothing here for this mode.", "hint", 0.5);
    gtk_widget_set_margin_top (placeholder, 32);
    gtk_widget_set_margin_bottom (placeholder, 32);
    gtk_list_box_set_placeholder (GTK_LIST_BOX (app.list), placeholder);
    GtkWidget *content = vbox (0);
    gtk_widget_set_margin_bottom (content, 70);   /* room for the selection bar */
    gtk_box_append (GTK_BOX (content), app.list);
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), content);
    gtk_widget_set_vexpand (scroll, TRUE);
    GtkWidget *page = vbox (0);
    gtk_box_append (GTK_BOX (page), app.summary);
    gtk_box_append (GTK_BOX (page), head);
    gtk_box_append (GTK_BOX (page), scroll);

    /* floating selection bar */
    app.sel_label = styled_label ("", "heading", 0);
    gtk_widget_set_margin_end (app.sel_label, 6);
    app.sel_start = icon_button ("media-playback-start-symbolic", "Start selected", "round",
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
    GtkWidget *clear = app.sel_clear = icon_button ("window-close-symbolic", "Clear selection", "round",
                                                    G_CALLBACK (on_sel_clear));   /* tips: update_controls */
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

    GtkWidget *overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (overlay), page);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), app.selbar);
    return overlay;
}

/* Missing tool: what to install, with a copy button (shown in that mode only). */
static GtkWidget *
build_banner (void)
{
    app.banner_title = styled_label ("", "banner-title", 0);
    app.banner_sub = styled_label ("", "banner-sub", 0);
    gtk_label_set_wrap (GTK_LABEL (app.banner_sub), TRUE);
    GtkWidget *texts = vbox (1);
    gtk_widget_set_hexpand (texts, TRUE);
    gtk_box_append (GTK_BOX (texts), app.banner_title);
    gtk_box_append (GTK_BOX (texts), app.banner_sub);
    app.banner_code = gtk_label_new ("");
    gtk_label_set_selectable (GTK_LABEL (app.banner_code), TRUE);
    GtkWidget *copy = icon_button ("edit-copy-symbolic", "Copy the command", "flat", G_CALLBACK (on_copy_fix));
    gtk_widget_set_tooltip_text (copy, "Copy");
    GtkWidget *code = hbox (4);
    gtk_widget_add_css_class (code, "code");
    gtk_widget_set_valign (code, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (code), app.banner_code);
    gtk_box_append (GTK_BOX (code), copy);
    GtkWidget *icon = gtk_image_new_from_icon_name ("mt-wrench-symbolic");
    gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
    app.banner = hbox (12);
    gtk_widget_add_css_class (app.banner, "banner");
    gtk_widget_set_margin_start (app.banner, 16);
    gtk_widget_set_margin_end (app.banner, 16);
    gtk_widget_set_margin_top (app.banner, 12);
    gtk_widget_set_margin_bottom (app.banner, 4);
    gtk_box_append (GTK_BOX (app.banner), icon);
    gtk_box_append (GTK_BOX (app.banner), texts);
    gtk_box_append (GTK_BOX (app.banner), code);
    gtk_widget_set_visible (app.banner, FALSE);
    return app.banner;
}

static GtkWidget *
build_footer (void)
{
    app.foot_left = styled_label ("", NULL, 0);
    gtk_label_set_ellipsize (GTK_LABEL (app.foot_left), PANGO_ELLIPSIZE_END);
    app.foot_bar = gtk_progress_bar_new ();
    gtk_widget_set_size_request (app.foot_bar, 260, -1);
    gtk_widget_set_valign (app.foot_bar, GTK_ALIGN_CENTER);
    app.foot_right = styled_label ("", "foot-right", 1);
    gtk_widget_set_hexpand (app.foot_right, TRUE);
    gtk_label_set_ellipsize (GTK_LABEL (app.foot_right), PANGO_ELLIPSIZE_START);
    app.footer = hbox (14);
    gtk_widget_add_css_class (app.footer, "footer");
    gtk_box_append (GTK_BOX (app.footer), app.foot_left);
    gtk_box_append (GTK_BOX (app.footer), app.foot_bar);
    gtk_box_append (GTK_BOX (app.footer), app.foot_right);
    return app.footer;
}

/* ---- inspector ---- */

static GtkWidget *
section (GtkWidget *box, const char *title)
{
    GtkWidget *l = styled_label (title, "sect", 0);
    gtk_widget_set_margin_top (l, 16);
    gtk_widget_set_margin_bottom (l, 6);
    gtk_box_append (GTK_BOX (box), l);
    return l;
}

static GtkWidget *
hint (GtkWidget *box, const char *text)
{
    GtkWidget *l = styled_label (text, "hint", 0);
    gtk_label_set_wrap (GTK_LABEL (l), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (l), 20);   /* wraps to the inspector instead of widening it */
    gtk_widget_set_margin_top (l, 6);
    gtk_box_append (GTK_BOX (box), l);
    return l;
}

/* A switch with its title and a line of help, as the design's option rows. */
static GtkWidget *
option_row (GtkWidget *box, const char *title, const char *sub, const char *tip)
{
    GtkWidget *sw = gtk_switch_new ();
    gtk_widget_set_valign (sw, GTK_ALIGN_START);
    GtkWidget *l = gtk_label_new_with_mnemonic (title);
    gtk_widget_add_css_class (l, "opt-title");
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (l), sw);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (sw), GTK_ACCESSIBLE_RELATION_LABELLED_BY, l, NULL, -1);
    GtkWidget *s = styled_label (sub, "opt-sub", 0);
    gtk_label_set_wrap (GTK_LABEL (s), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (s), 20);
    GtkWidget *texts = vbox (1);
    gtk_widget_set_hexpand (texts, TRUE);
    gtk_box_append (GTK_BOX (texts), l);
    gtk_box_append (GTK_BOX (texts), s);
    GtkWidget *row = hbox (10);
    gtk_widget_set_margin_bottom (row, 10);
    gtk_widget_set_tooltip_text (row, tip);
    gtk_box_append (GTK_BOX (row), texts);
    gtk_box_append (GTK_BOX (row), sw);
    gtk_box_append (GTK_BOX (box), row);
    return sw;
}

/* A segmented control of toggle buttons; returns the box, fills `out` with the buttons. */
static GtkWidget *
segmented (const char *const *labels, guint n, guint active, GtkWidget **out, GCallback toggled)
{
    GtkWidget *seg = hbox (0);
    gtk_widget_add_css_class (seg, "seg");
    for (guint i = 0; i < n; i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label (labels[i]);
        if (i > 0)
            gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b), GTK_TOGGLE_BUTTON (out[0]));
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), i == active);
        gtk_widget_set_hexpand (b, TRUE);   /* natural widths plus an equal share: six speeds fit */
        g_signal_connect (b, "toggled", toggled, GUINT_TO_POINTER (i));
        out[i] = b;
        gtk_box_append (GTK_BOX (seg), b);
    }
    return seg;
}

static void
check_line (GtkWidget *box, const char *icon, const char *text)
{
    GtkWidget *row = hbox (8);
    gtk_widget_add_css_class (row, "check-line");
    gtk_widget_set_margin_bottom (row, 5);
    gtk_box_append (GTK_BOX (row), gtk_image_new_from_icon_name (icon));
    gtk_box_append (GTK_BOX (row), styled_label (text, NULL, 0));
    gtk_box_append (GTK_BOX (box), row);
}

static GtkWidget *
build_speed_guide (void)
{
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
    GtkWidget *link = hbox (6);
    gtk_box_append (GTK_BOX (link), gtk_image_new_from_icon_name ("help-about-symbolic"));
    gtk_box_append (GTK_BOX (link), gtk_label_new ("Speed guide · time vs size"));
    GtkWidget *info = gtk_menu_button_new ();
    gtk_menu_button_set_child (GTK_MENU_BUTTON (info), link);
    gtk_menu_button_set_always_show_arrow (GTK_MENU_BUTTON (info), FALSE);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (info), pop);
    gtk_widget_add_css_class (info, "link");
    gtk_widget_set_halign (info, GTK_ALIGN_START);
    gtk_widget_set_margin_top (info, 8);
    return info;
}

static GtkWidget *
build_compress_opts (void)
{
    GtkWidget *box = vbox (0);
    section (box, "SPEED");
    /* the x265 speeds, as in the design; the GPU (VC_PRESETS[0]) is the Use GPU switch below */
    const char *titles[G_N_ELEMENTS (app.speed_btns)];
    guint np = 1;
    g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
    for (; VC_PRESETS[np] != NULL && np < G_N_ELEMENTS (app.speed_btns); np++) {
        g_ptr_array_add (names, preset_title (np));
        titles[np - 1] = names->pdata[np - 1];
        if (strcmp (VC_PRESETS[np], VC_DEFAULT_PRESET) == 0)
            app.speed = np;
    }
    app.preset = app.speed;
    gtk_box_append (GTK_BOX (box), segmented (titles, np - 1, app.speed - 1, &app.speed_btns[1],
                                              G_CALLBACK (on_speed)));
    gtk_box_append (GTK_BOX (box), build_speed_guide ());

    section (box, "MAX SIZE");
    const char *sizes[] = { "Original size", "Max 1080p", "Max 720p", NULL };
    app.max_size = gtk_drop_down_new_from_strings (sizes);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.max_size), GTK_ACCESSIBLE_PROPERTY_LABEL, "Max size", -1);
    gtk_widget_set_tooltip_text (app.max_size, "Shrink bigger videos so the short side is at most this (a 4K "
                                               "video at 1080p takes about a quarter of the time). Smaller "
                                               "videos are never enlarged.");
    g_signal_connect (app.max_size, "notify::selected", G_CALLBACK (on_estimate_input), NULL);
    gtk_box_append (GTK_BOX (box), app.max_size);

    section (box, "OPTIONS");
    app.gpu = option_row (box, "Use _GPU", "Many times faster; files a little bigger.",
                          "Encode on the graphics chip instead of x265. Much faster, the files come out a little "
                          "bigger. Shown only when this computer's GPU encoder works.");
    gtk_widget_set_visible (gtk_widget_get_parent (app.gpu), FALSE);   /* gpu_test_done shows it */
    g_signal_connect (app.gpu, "notify::active", G_CALLBACK (on_gpu), NULL);
    app.subs = option_row (box, "Keep _subtitles", "Copies subtitle tracks. Output becomes .mkv.",
                           "Copy subtitle tracks too. The output becomes .mkv.");
    app.fps30 = option_row (box, "Max _30 fps", "60 fps phone clips saved at 30 fps.",
                            "Save videos above 30 fps (often 60 fps phone clips) at 30 fps: about half the "
                            "encode time. Off keeps every frame.");
    /* a size sample is taken at the frame rate it will be encoded at */
    g_signal_connect (app.fps30, "notify::active", G_CALLBACK (on_estimate_input), NULL);
    app.trash = option_row (box, "Move originals to _Trash", "Only after each copy is checked. Asks first.",
                            "After each copy is checked: same length and at least 5% smaller. Asks before "
                            "starting.");
    return box;
}

static GtkWidget *
build_web_opts (void)
{
    GtkWidget *box = vbox (0);
    section (box, "MAX WIDTH");
    app.web_width = width_dropdown ();
    gtk_box_append (GTK_BOX (box), app.web_width);
    hint (box, "Narrower videos are never enlarged.");
    section (box, "AUDIO");
    app.no_audio = option_row (box, "Remove _audio", "No audio track, for muted autoplay backgrounds.",
                               "No audio track, for muted autoplay backgrounds.");
    section (box, "EVERY FILE GETS");
    check_line (box, "object-select-symbolic", "H.264 MP4 · plays in Safari & iOS");
    check_line (box, "object-select-symbolic", "Fast start · plays while loading");
    check_line (box, "object-select-symbolic", "HDR converted to SDR");
    check_line (box, "object-select-symbolic", "Location & metadata removed");
    return box;
}

static GtkWidget *
build_frames_opts (void)
{
    GtkWidget *box = app.frames_opts = vbox (0);
    section (box, "FRAMES PER SECOND");
    const char *rates[] = { "5", "10", "12", "15", "24", "30", "All" };
    G_STATIC_ASSERT (G_N_ELEMENTS (rates) == G_N_ELEMENTS (FRAME_RATES));
    app.fps_idx = FRAMES_DEFAULT_RATE;
    GtkWidget *seg = segmented (rates, G_N_ELEMENTS (rates), FRAMES_DEFAULT_RATE, app.fps_btns, G_CALLBACK (on_fps));
    gtk_widget_set_tooltip_text (seg, "Frames saved per second of video. All keeps the video's own rate.");
    gtk_box_append (GTK_BOX (box), seg);

    section (box, "RANGE");
    GtkWidget *range = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (range), 8);
    gtk_grid_set_row_spacing (GTK_GRID (range), 4);
    gtk_grid_set_column_homogeneous (GTK_GRID (range), TRUE);
    GtkWidget **times[] = { &app.frames_start, &app.frames_end };
    const char *time_label[] = { "Sta_rt", "_End" };
    const char *time_hint[] = { "0:00", "end" };
    const char *time_tip[] = { "Where to start: empty for the beginning, or 12, 12.5, 1:05, 0:01:05.",
                               "Where to stop: empty for the end. The same range applies to every video." };
    for (guint i = 0; i < 2; i++) {
        *times[i] = gtk_entry_new ();
        gtk_editable_set_width_chars (GTK_EDITABLE (*times[i]), 6);
        gtk_entry_set_placeholder_text (GTK_ENTRY (*times[i]), time_hint[i]);
        gtk_widget_set_tooltip_text (*times[i], time_tip[i]);
        g_signal_connect (*times[i], "changed", G_CALLBACK (on_estimate_input), NULL);
        GtkWidget *l = gtk_label_new_with_mnemonic (time_label[i]);
        gtk_label_set_xalign (GTK_LABEL (l), 0);
        gtk_widget_add_css_class (l, "hint");
        gtk_label_set_mnemonic_widget (GTK_LABEL (l), *times[i]);
        gtk_accessible_update_relation (GTK_ACCESSIBLE (*times[i]), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                        l, NULL, -1);
        gtk_grid_attach (GTK_GRID (range), l, i, 0, 1, 1);
        gtk_grid_attach (GTK_GRID (range), *times[i], i, 1, 1, 1);
    }
    gtk_box_append (GTK_BOX (box), range);
    GtkWidget *err = hbox (6);
    gtk_widget_add_css_class (err, "err-line");
    gtk_widget_set_margin_top (err, 6);
    app.range_err = styled_label ("", NULL, 0);
    gtk_box_append (GTK_BOX (err), gtk_image_new_from_icon_name ("dialog-warning-symbolic"));
    gtk_box_append (GTK_BOX (err), app.range_err);
    gtk_widget_set_visible (err, FALSE);
    gtk_box_append (GTK_BOX (box), err);

    section (box, "MAX WIDTH");
    app.frames_width = width_dropdown ();
    gtk_box_append (GTK_BOX (box), app.frames_width);

    section (box, "FORMAT");
    const char *formats[] = { "WebP", "JPG" };
    app.webp = TRUE;
    gtk_box_append (GTK_BOX (box), segmented (formats, 2, 0, app.fmt_btns, G_CALLBACK (on_format)));
    app.fmt_caption = hint (box, "Smaller, plays in Safari 14 and later.");

    GtkWidget *q_label = gtk_label_new_with_mnemonic ("_Quality");
    gtk_widget_add_css_class (q_label, "opt-title");
    gtk_widget_set_hexpand (q_label, TRUE);
    gtk_label_set_xalign (GTK_LABEL (q_label), 0);
    app.quality_val = styled_label ("80", "hint", 1);
    GtkWidget *q_head = hbox (8);
    gtk_widget_set_margin_top (q_head, 18);
    gtk_box_append (GTK_BOX (q_head), q_label);
    gtk_box_append (GTK_BOX (q_head), app.quality_val);
    gtk_box_append (GTK_BOX (box), q_head);
    app.frames_quality = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
    gtk_range_set_value (GTK_RANGE (app.frames_quality), 80);
    gtk_widget_set_tooltip_text (app.frames_quality, "1–100. Higher keeps more detail in bigger files.");
    gtk_label_set_mnemonic_widget (GTK_LABEL (q_label), app.frames_quality);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (app.frames_quality), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                    q_label, NULL, -1);
    g_signal_connect (app.frames_quality, "value-changed", G_CALLBACK (on_quality), NULL);
    gtk_widget_set_margin_top (app.frames_quality, 6);
    gtk_box_append (GTK_BOX (box), app.frames_quality);
    return box;
}

static void
format_line (GtkWidget *box, const char *badge, const char *text)
{
    GtkWidget *row = hbox (8);
    gtk_widget_set_margin_bottom (row, 6);
    GtkWidget *b = styled_label (badge, "badge", 0.5);
    gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (row), b);
    gtk_box_append (GTK_BOX (row), styled_label (text, "check-line", 0));
    gtk_box_append (GTK_BOX (box), row);
}

static GtkWidget *
build_images_opts (void)
{
    GtkWidget *box = app.images_opts = vbox (0);
    section (box, "MAX WIDTH");
    app.images_width = width_dropdown ();
    gtk_drop_down_set_selected (GTK_DROP_DOWN (app.images_width), 0);
    gtk_widget_set_tooltip_text (app.images_width, "Scale wider images down to this width, keeping the aspect "
                                                   "ratio. Narrower images are never enlarged.");
    gtk_box_append (GTK_BOX (box), app.images_width);
    hint (box, "Wider images scale down, never up.");
    section (box, "YOU GET");
    format_line (box, "JPG", "→ optimized JPG + WebP");
    format_line (box, "PNG", "→ quantized PNG + WebP");
    format_line (box, "WEBP", "→ smaller WebP");
    format_line (box, "BMP·TIFF", "→ PNG or JPG + WebP");
    section (box, "PRIVACY");
    check_line (box, "object-select-symbolic", "GPS & camera data removed");
    check_line (box, "object-select-symbolic", "Colour profile kept");
    section (box, "ORIGINALS");
    app.img_trash = option_row (box, "Move originals to _Trash", "Only when the same-type copy opens at the "
                                "right size.",
                                "After each same-type copy (jpg or png) is checked: it opens, has the expected "
                                "size and is at least 5% smaller. Asks before starting.");
    return box;
}

static GtkWidget *
build_inspector (void)
{
    app.insp_icon = gtk_image_new_from_icon_name (MODE_INFO[0].icon);
    gtk_widget_add_css_class (app.insp_icon, "insp-icon");
    gtk_widget_set_valign (app.insp_icon, GTK_ALIGN_CENTER);
    app.insp_title = styled_label ("", "insp-title", 0);
    app.insp_sub = styled_label ("", "insp-sub", 0);
    GtkWidget *texts = vbox (0);
    gtk_box_append (GTK_BOX (texts), app.insp_title);
    gtk_box_append (GTK_BOX (texts), app.insp_sub);
    GtkWidget *head = hbox (10);
    gtk_box_append (GTK_BOX (head), app.insp_icon);
    gtk_box_append (GTK_BOX (head), texts);

    app.insp_stack = gtk_stack_new ();
    gtk_stack_set_vhomogeneous (GTK_STACK (app.insp_stack), FALSE);
    gtk_stack_add_named (GTK_STACK (app.insp_stack), build_compress_opts (), MODE_INFO[VC_MODE_COMPRESS].css);
    gtk_stack_add_named (GTK_STACK (app.insp_stack), build_web_opts (), MODE_INFO[VC_MODE_WEB_VIDEO].css);
    gtk_stack_add_named (GTK_STACK (app.insp_stack), build_frames_opts (), MODE_INFO[VC_MODE_FRAMES].css);
    gtk_stack_add_named (GTK_STACK (app.insp_stack), build_images_opts (), MODE_INFO[VC_MODE_IMAGES].css);
    gtk_widget_set_vexpand (app.insp_stack, TRUE);

    app.insp_note = styled_label ("", "note", 0);
    gtk_label_set_wrap (GTK_LABEL (app.insp_note), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (app.insp_note), 20);
    gtk_widget_set_margin_top (app.insp_note, 16);

    GtkWidget *box = vbox (0);
    gtk_widget_set_margin_start (box, 18);
    gtk_widget_set_margin_end (box, 18);
    gtk_widget_set_margin_top (box, 18);
    gtk_widget_set_margin_bottom (box, 18);
    gtk_box_append (GTK_BOX (box), head);
    gtk_box_append (GTK_BOX (box), app.insp_stack);
    gtk_box_append (GTK_BOX (box), app.insp_note);
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), FALSE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), box);
    gtk_widget_add_css_class (scroll, "inspector");
    gtk_widget_set_size_request (scroll, 262, -1);
    gtk_widget_set_hexpand (scroll, FALSE);   /* its growing children must not make it take the list's space */
    return scroll;
}

/* The design's font, inside the binary so the app needs no install or internet. Pango loads fonts
 * from files, so each one is copied once into the user's cache. Any failure leaves the system font. */
static void
load_fonts (void)
{
    static const char *const names[] = { "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf",
                                         "Inter-Bold.ttf" };
    g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "media-tools", "fonts", NULL);
    if (g_mkdir_with_parents (dir, 0755) != 0)
        return;
    for (gsize i = 0; i < G_N_ELEMENTS (names); i++) {
        g_autofree char *res = g_strconcat ("/io/github/dasantha/VideoCompressor/fonts/", names[i], NULL);
        g_autoptr (GBytes) data = g_resources_lookup_data (res, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
        g_autofree char *path = g_build_filename (dir, names[i], NULL);
        GStatBuf st;
        if (data == NULL)
            continue;
        gsize n = g_bytes_get_size (data);
        /* g_file_set_contents writes a temp file and renames it: safe with two instances starting */
        if ((g_stat (path, &st) != 0 || (gsize) st.st_size != n)
            && !g_file_set_contents (path, g_bytes_get_data (data, NULL), n, NULL))
            continue;
        pango_font_map_add_font_file (pango_cairo_font_map_get_default (), path, NULL);
    }
}

/* Encode speed, in output pixel-frames per second, learned from finished encodes on this machine
 * and kept in ~/.config/media-tools/speed.ini. Seeded from this laptop's benchmark (i5-1240P,
 * research/handbrake/adopt/16-encode-speed.md), so a new machine's first estimate is rough until
 * one video has finished. */
#define SEED_MEDIUM_PPS 2.8e7

static char *
speed_file (void)
{
    return g_build_filename (g_get_user_config_dir (), "media-tools", "speed.ini", NULL);
}

static void
load_speeds (void)
{
    app.medium_pps = SEED_MEDIUM_PPS;
    app.gpu_pps = 0;   /* vc_estimate_seconds then uses the GPU's time factor */
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_autofree char *f = speed_file ();
    if (!g_key_file_load_from_file (k, f, G_KEY_FILE_NONE, NULL))
        return;
    double x = g_key_file_get_double (k, "speed", "x265-medium", NULL);
    double g = g_key_file_get_double (k, "speed", "gpu", NULL);
    if (x > 0 && isfinite (x))
        app.medium_pps = x;
    if (g > 0 && isfinite (g))
        app.gpu_pps = g;
}

/* After a compress encode ended well: how fast it really went, averaged into what is stored. */
static void
learn_speed (Item *it)
{
    VcOptions o = current_options ();   /* settings are locked while busy: these encoded it */
    double secs = (g_get_monotonic_time () - it->t0 - it->paused_total) / 1e6;
    double pf = vc_output_pixel_frames (it->probe, &o);
    if (secs < 3 || pf <= 0)
        return;   /* too short to time well */
    double t, sz;
    vc_preset_factors (o.preset, &t, &sz);
    gboolean gpu = strcmp (o.preset, VC_PRESET_GPU) == 0;
    double measured = gpu ? pf / secs : pf / secs * t;   /* x265 as its medium-speed equivalent */
    const char *key = gpu ? "gpu" : "x265-medium";
    double *slot = gpu ? &app.gpu_pps : &app.medium_pps;
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_autofree char *f = speed_file ();
    g_key_file_load_from_file (k, f, G_KEY_FILE_KEEP_COMMENTS, NULL);
    /* the first real encode replaces the seed; later ones average in */
    *slot = g_key_file_has_key (k, "speed", key, NULL) ? (*slot + measured) / 2 : measured;
    g_key_file_set_double (k, "speed", key, *slot);
    g_autofree char *dir = g_path_get_dirname (f);
    if (g_mkdir_with_parents (dir, 0755) == 0)
        g_key_file_save_to_file (k, f, NULL);   /* best effort: only estimates depend on it */
}

/* Follows the theme: dark when the colour scheme is dark or the app is asked for a dark theme. */
static void
sync_dark (void)
{
    GtkInterfaceColorScheme scheme;
    gboolean prefer;
    g_object_get (gtk_settings_get_default (), "gtk-interface-color-scheme", &scheme,
                  "gtk-application-prefer-dark-theme", &prefer, NULL);
    GtkWidget *w[] = { GTK_WIDGET (app.win), app.close_dialog };
    for (gsize i = 0; i < G_N_ELEMENTS (w) && w[i] != NULL; i++) {
        if (scheme == GTK_INTERFACE_COLOR_SCHEME_DARK || prefer)
            gtk_widget_add_css_class (w[i], "dark");
        else
            gtk_widget_remove_css_class (w[i], "dark");
    }
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
    load_fonts ();
    load_speeds ();
    g_autoptr (GtkCssProvider) css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (css, CSS);
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (css),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    app.col_status = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
    app.col_actions = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
    for (guint i = 0; i < 3; i++)
        app.guide_cols[i] = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

    app.win = GTK_WINDOW (gtk_application_window_new (gapp));
    gtk_widget_add_css_class (GTK_WIDGET (app.win), "mt");
    gtk_window_set_title (app.win, "Media Tools");
    gtk_window_set_default_size (app.win, 1100, 780);
    g_signal_connect (app.win, "close-request", G_CALLBACK (on_close), NULL);
    /* no title bar: the window controls live in the sidebar, the toolbar in the header */
    GtkWidget *no_titlebar = hbox (0);
    gtk_widget_set_visible (no_titlebar, FALSE);
    gtk_window_set_titlebar (app.win, no_titlebar);

    GtkWidget *header = build_header ();   /* first: the empty page mirrors its buttons */
    app.pages = gtk_stack_new ();
    gtk_widget_set_vexpand (app.pages, TRUE);
    gtk_stack_add_named (GTK_STACK (app.pages), build_empty_page (), "empty");
    gtk_stack_add_named (GTK_STACK (app.pages), build_list_page (), "list");

    GtkWidget *center = vbox (0);
    gtk_widget_set_hexpand (center, TRUE);
    gtk_box_append (GTK_BOX (center), build_banner ());
    gtk_box_append (GTK_BOX (center), app.pages);
    gtk_box_append (GTK_BOX (center), build_footer ());
    GtkWidget *body = hbox (0);
    gtk_widget_set_vexpand (body, TRUE);
    gtk_box_append (GTK_BOX (body), center);
    gtk_box_append (GTK_BOX (body), build_inspector ());
    GtkWidget *right = vbox (0);
    gtk_widget_set_hexpand (right, TRUE);
    gtk_box_append (GTK_BOX (right), header);
    gtk_box_append (GTK_BOX (right), body);

    GtkWidget *toast_box = hbox (10);
    gtk_widget_add_css_class (toast_box, "toast");
    app.toast_label = gtk_label_new ("");
    gtk_box_append (GTK_BOX (toast_box), gtk_image_new_from_icon_name ("user-trash-symbolic"));
    gtk_box_append (GTK_BOX (toast_box), app.toast_label);
    app.toast = gtk_revealer_new ();
    gtk_revealer_set_transition_type (GTK_REVEALER (app.toast), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_child (GTK_REVEALER (app.toast), toast_box);
    gtk_widget_set_halign (app.toast, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (app.toast, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom (app.toast, 58);
    gtk_widget_set_can_target (app.toast, FALSE);
    GtkWidget *center_overlay = gtk_overlay_new ();   /* the toast floats over the list, as in the design */
    gtk_overlay_set_child (GTK_OVERLAY (center_overlay), right);
    gtk_overlay_add_overlay (GTK_OVERLAY (center_overlay), app.toast);

    GtkWidget *root = hbox (0);
    gtk_box_append (GTK_BOX (root), build_sidebar ());
    gtk_box_append (GTK_BOX (root), center_overlay);
    gtk_window_set_child (app.win, root);

    GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect (drop, "drop", G_CALLBACK (on_drop), NULL);
    gtk_widget_add_controller (root, GTK_EVENT_CONTROLLER (drop));

    GtkEventController *keys = gtk_shortcut_controller_new ();
    add_shortcut (keys, "space", on_space);
    add_shortcut (keys, "Escape", on_escape);
    gtk_widget_add_controller (GTK_WIDGET (app.win), keys);

    sync_dark ();
    g_signal_connect_swapped (gtk_settings_get_default (), "notify::gtk-interface-color-scheme",
                              G_CALLBACK (sync_dark), NULL);
    g_signal_connect_swapped (gtk_settings_get_default (), "notify::gtk-application-prefer-dark-theme",
                              G_CALLBACK (sync_dark), NULL);
    apply_mode_ui ();
    update_header ();
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
