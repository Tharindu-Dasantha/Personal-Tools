/* Shared by the UI files: the item and app state, the mode table, and the functions more than
 * one file uses. vc_core (compress.h, modes.h) has no GTK; this is the GTK side. */
#pragma once

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
#include <sys/xattr.h>
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
    GtkWidget *mode_list, *mode_row[4], *mode_count[4], *mode_lock[4], *mode_dot[4];
    GtkWidget *banner, *banner_title, *banner_sub, *banner_code, *size_head;
    GtkWidget *insp_icon, *insp_title, *insp_sub, *insp_note, *insp_stack;
    GtkWidget *web_width, *no_audio, *frames_opts, *fps_btns[8], *frames_start, *frames_end;
    GtkWidget *range, *range_fill, *range_knob[2];   /* frames range slider, mirrors Start/End */
    double range_dur, range_s, range_e;               /* seconds; dur 0 = no video yet */
    guint range_drag;                                 /* 0 Start, 1 End: the knob being dragged */
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
    guint appearance;    /* 0 follows the system, 1 light, 2 dark; kept in settings.ini */
    gboolean closing;    /* window destroyed: late async callbacks must not touch widgets */
    gboolean was_busy;   /* a batch was running at the last update_controls */
    guint inhibit;       /* suspend inhibitor held while a batch runs, or 0 */

    /* running jobs (ST_ACTIVE items), at most max_jobs(); a paused one still counts */
    guint n_jobs;
    guint pulse_id;      /* pulses the bars of steps without progress output */
    VcMode mode;
    guint have;          /* VC_HAVE_* bits from the startup tool check */
} App;


extern App app;

/* The mode sidebar, in VcMode order: name, subtitle, icon, CSS class (sets the accent), the empty
 * page's line and the inspector's note. */
static const struct {
    const char *name, *sub, *icon, *css, *empty, *note;
} MODE_INFO[] = {
    { "Compress video", "H.265, same quality", "mt-video-symbolic", "compress",
      "Every video is compressed to H.265, keeping its quality and resolution. Pick a mode on the left "
      "to make web videos, extract frames, or compress images instead.",
      "Saves to <tt>compressed/</tt> next to each video. Originals are never changed." },
    { "Web video", "Safari-ready H.264 MP4", "mt-globe-symbolic", "web",
      "Every video becomes an H.264 MP4 that plays in every browser, Safari and iOS included. Pick a "
      "mode on the left to compress, extract frames, or compress images instead.",
      "Saves to <tt>web/</tt> next to each video. Trash isn't offered: web copies are derivatives, "
      "not replacements." },
    { "Video → frames", "JPG / WebP sequence", "view-grid-symbolic", "frames",
      "One video becomes a folder of numbered WebP or JPG frames, for scroll animations. Pick a "
      "mode on the left to compress or convert videos, or compress images instead.",
      NULL /* set by the format: update_frames_note */ },
    { "Compress images", "Same type + WebP", "image-x-generic-symbolic", "images",
      "Every image gets a smaller copy in its own format, plus a WebP for the web. Pick a mode on "
      "the left to work with videos instead.",
      "Copies under 5% smaller are dropped. Saves to <tt>compressed/</tt>." },
};
/* The frames FPS buttons, in order */
static const int FRAME_RATES[] = { 5, 10, 12, 15, 24, 30 };
#define FRAMES_DEFAULT_RATE 3   /* 15 */
/* The Max width dropdowns, in order; 0 is Original */
static const int WEB_WIDTHS[] = { 0, 3840, 2560, 1920, 1280, 960, 640 };
#define WEB_DEFAULT_WIDTH 3   /* 1920 */

extern const char CSS[];

/* common.c */
void item_free (Item *it);
void alert (const char *message, const char *detail, GAsyncReadyCallback done);
void confirm (const char *message, const char *detail, const char *accept, GAsyncReadyCallback done);
gboolean trash_on (void);
const char *noun (void);
const char *verb (void);
char *doing (void);
int percent_smaller (guint64 before, guint64 after);
char *change_text (guint64 before, guint64 after);
VcOptions current_options (void);
char *format_clock (double t);
const char *check_range (void);
char *host_path (const char *path);
char *home_relative (const char *path);
char *preset_title (guint i);
char *basename_display (const char *path);
void set_tip (GtkWidget *w, const char *title, const char *key, const char *body);
gboolean checked (Item *it);
gboolean in_scope (Item *it, Item *one, gboolean sel);
gboolean can_start (Item *it);
gboolean sampled_at (Item *it, const VcOptions *o);
guint64 item_estimate (Item *it, const VcOptions *o, gboolean *measured);
gboolean skip_in_bulk (Item *it);
gboolean can_pause (Item *it);
gboolean can_stop (Item *it);
gboolean can_trash (Item *it);
guint max_jobs (void);
guint paused_jobs (Item **first);
gboolean is_busy (void);

/* row.c */
void set_status (Item *it, const char *note, const char *tip);
void render_actions (Item *it);
void render_row (Item *it);
GtkWidget *chip (const char *text, const char *style);
void refresh_chips (Item *it);

/* status.c */
void update_speed_guide (void);
void update_progress (void);
void notify (const char *id, const char *title, const char *body);
void update_controls (void);
void show_toast (const char *icon, const char *title, const char *detail);

/* source.c */
void update_header (void);
void load_source (const char *folder, char *const *files);
void on_choose (void);
void on_choose_files (void);
gboolean on_drop (GtkDropTarget *t, const GValue *value, double x, double y, gpointer user_data);
void on_estimate_input (void);
void on_speed (GtkToggleButton *b, gpointer user_data);
void apply_mode_ui (void);
void on_gpu (void);
void on_mode (GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
void on_fps (GtkToggleButton *b, gpointer user_data);
void on_format (GtkToggleButton *b, gpointer user_data);
void on_quality (GtkRange *r, gpointer user_data);
void on_copy_fix (void);

/* jobs.c */
void pause_all (gboolean pause);
const char *trash_checks (void);
void act_start (Item *one, gboolean sel);
void act_pause (Item *one, gboolean sel);
void act_stop (Item *one, gboolean sel);
void check_then_trash (Item *it);
void trash_confirmed (GObject *source, GAsyncResult *res, gpointer user_data);
gboolean pulse_bar (gpointer data);
void sample_stop (void);
void sample_next (void);
void load_speeds (void);

/* handlers.c */
void on_start_all (void);
void on_pause_all (void);
void on_stop_all (void);
void on_sel_start (void);
void on_sel_pause (void);
void on_sel_stop (void);
void on_sel_trash (void);
void set_all_checked (gboolean on);
void on_sel_clear (void);
void on_select_all (GtkCheckButton *b, gpointer user_data);
void on_finished_show (void);
GtkWidget *make_row (Item *it);

/* main.c */
gboolean on_close (GtkWindow *win, gpointer user_data);
void activate (GtkApplication *gapp, gpointer user_data);

/* tools.c */
void gpu_test_done (GObject *source, GAsyncResult *res, gpointer user_data);
void check_tools (void);

/* style.c */
void load_fonts (void);
void load_appearance (void);
void sync_dark (void);
void on_appearance (GtkToggleButton *b, gpointer user_data);

/* widgets.c */
GtkWidget *width_dropdown (void);
GtkWidget *vbox (int spacing);
GtkWidget *hbox (int spacing);
GtkWidget *styled_label (const char *text, const char *style, float xalign);
GtkWidget *build_sidebar (void);
GtkWidget *build_header (void);
GtkWidget *build_empty_page (void);
GtkWidget *build_list_page (void);
GtkWidget *build_banner (void);
GtkWidget *build_footer (void);

/* inspector.c */
GtkWidget *hint (GtkWidget *box, const char *text);
GtkWidget *segmented (const char *const *labels, guint n, guint active, GtkWidget **out, GCallback toggled);
GtkWidget *build_inspector (void);
