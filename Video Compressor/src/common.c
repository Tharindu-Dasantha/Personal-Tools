#include "app.h"

void
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

void
alert (const char *message, const char *detail, GAsyncReadyCallback done)
{
    g_autoptr (GtkAlertDialog) d = gtk_alert_dialog_new ("%s", message);
    gtk_alert_dialog_set_detail (d, detail);
    gtk_alert_dialog_choose (d, app.win, NULL, done, NULL);
}

void
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



/* Compress video and Compress images each have their own Move to Trash switch. */
gboolean
trash_on (void)
{
    return vc_mode_can_trash (app.mode)
           && gtk_switch_get_active (GTK_SWITCH (app.mode == VC_MODE_IMAGES ? app.img_trash : app.trash));
}

/* "video" or "image", for counts and labels */
const char *
noun (void)
{
    return app.mode == VC_MODE_IMAGES ? "image" : "video";
}

/* "Compress", "Convert" or "Extract", for button and tooltip titles */
const char *
verb (void)
{
    return app.mode == VC_MODE_WEB_VIDEO ? "Convert" : app.mode == VC_MODE_FRAMES ? "Extract" : "Compress";
}

/* "Compressing images", "Converting videos", "Extracting frames from videos" */
char *
doing (void)
{
    return g_strdup_printf ("%s %ss", app.mode == VC_MODE_WEB_VIDEO ? "Converting"
                                      : app.mode == VC_MODE_FRAMES ? "Extracting frames from"
                                      : "Compressing", noun ());
}

int
percent_smaller (guint64 before, guint64 after)
{
    int p = (int) (100 - after * 100.0 / before + 0.5);
    return after > 0 ? MIN (p, 99) : p;   /* a 1.4 kB copy of 320 kB is not "−100%" */
}

/* "−63%" or, for a web copy that came out bigger, "+12%" */
char *
change_text (guint64 before, guint64 after)
{
    return after <= before ? g_strdup_printf ("−%d%%", percent_smaller (before, after))
                           : g_strdup_printf ("+%d%%", (int) (after * 100.0 / before - 100 + 0.5));
}

/* The encode settings chosen in the bottom bar. */
VcOptions
current_options (void)
{
    static const int sizes[] = { 0, 1080, 720 };   /* the Max size dropdown, in order */
    static const guint64 targets[] = { 0, 10000000, 25000000, 50000000, 100000000 };   /* Target size */
    guint size = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.max_size));
    guint target = gtk_drop_down_get_selected (GTK_DROP_DOWN (app.target));
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
        .target_bytes = app.mode == VC_MODE_COMPRESS && target < G_N_ELEMENTS (targets) ? targets[target] : 0,
    };
}

/* "0:01.5", "1:05", "1:02:03": a frames range end for the row's second line */
char *
format_clock (double t)
{
    guint tenths = (guint) llround (t * 10), s = tenths / 10, frac = tenths % 10;
    g_autofree char *hm = s >= 3600 ? g_strdup_printf ("%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60)
                                    : g_strdup_printf ("%u:%02u", s / 60, s % 60);
    return frac != 0 ? g_strdup_printf ("%s.%u", hm, frac) : g_steal_pointer (&hm);
}

/* Frames: NULL when Start and End parse and End (when set) is after Start, else the field to fix
 * ("Start" or "End"). Turns a bad field red and shows the hint under the range. */
const char *
check_range (void)
{
    double start, end;
    double dur = app.mode == VC_MODE_FRAMES && app.items != NULL && app.items->len > 0
                 && ((Item *) app.items->pdata[0])->probe != NULL
                 ? ((Item *) app.items->pdata[0])->probe->duration : 0;   /* frames mode holds one video */
    gboolean s_parsed = vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_start)), &start);
    gboolean e_parsed = vc_parse_time (gtk_editable_get_text (GTK_EDITABLE (app.frames_end)), &end);
    gboolean s_in = dur <= 0 || start < dur, e_in = dur <= 0 || end <= dur + 0.05;
    gboolean s_ok = s_parsed && s_in;
    gboolean e_ok = e_parsed && e_in && (end == 0 || !s_ok || end > start);
    GtkWidget *w[] = { app.frames_start, app.frames_end };
    gboolean ok[] = { s_ok, e_ok };
    for (guint i = 0; i < 2; i++) {
        if (ok[i])
            gtk_widget_remove_css_class (w[i], "error");
        else
            gtk_widget_add_css_class (w[i], "error");
    }
    g_autofree char *len = format_clock (dur);
    g_autofree char *past = g_strdup_printf ("The video is %s long", len);
    gtk_label_set_text (GTK_LABEL (app.range_err), !s_parsed || !e_parsed ? "Use 12, 12.5, 1:05 or 0:01:05"
                                                   : !s_in || !e_in ? past : "End must be after Start");
    gtk_widget_set_visible (gtk_widget_get_parent (app.range_err), !s_ok || !e_ok);
    gtk_entry_set_placeholder_text (GTK_ENTRY (app.frames_end), dur > 0 ? len : "end");
    app.range_dur = dur;
    app.range_s = s_ok ? start : 0;
    app.range_e = e_ok && end > 0 ? end : dur;
    gtk_widget_set_sensitive (app.range, dur > 0);
    gtk_widget_queue_allocate (app.range);
    return !s_ok ? "Start" : !e_ok ? "End" : NULL;
}

/* Flatpak: where a file or folder shared through the document portal really is, or NULL for any
 * other path. Inside the sandbox it is /run/user/1000/doc/<id>/…, which means nothing to people. */
char *
host_path (const char *path)
{
    char buf[4096];
    ssize_t n = getxattr (path, "user.document-portal.host-path", buf, sizeof buf - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return g_strdup (buf);
}

/* A path for display, with the home folder as ~: "~/Videos/Phone 2026" */
char *
home_relative (const char *path)
{
    g_autofree char *host = host_path (path);
    g_autofree char *display = g_filename_display_name (host != NULL ? host : path);
    const char *home = g_get_home_dir ();
    gsize hl = strlen (home);
    return strncmp (display, home, hl) == 0 && (display[hl] == '/' || !display[hl])
               ? g_strconcat ("~", display + hl, NULL)
               : g_steal_pointer (&display);
}

char *
preset_title (guint i)
{
    if (strcmp (VC_PRESETS[i], VC_PRESET_GPU) == 0)
        return g_strdup ("GPU");
    char *s = g_strdup (VC_PRESETS[i]);
    s[0] = g_ascii_toupper (s[0]);
    return s;
}

char *
basename_display (const char *path)
{
    g_autofree char *base = g_path_get_basename (path);
    return g_filename_display_name (base);
}

/* Tooltip copy as in the design: a bold title, one line of help, then an optional dimmed hint
 * (the shortcut). */
void
set_tip (GtkWidget *w, const char *title, const char *key, const char *body)
{
    g_autofree char *m = key ? g_markup_printf_escaped ("<b>%s</b>\n%s\n<span alpha=\"60%%\">%s</span>",
                                                        title, body, key)
                             : g_markup_printf_escaped ("<b>%s</b>\n%s", title, body);
    gtk_widget_set_tooltip_markup (w, m);
}

/* ---- which items an action applies to: one row, the checked rows, or all ---- */

gboolean
checked (Item *it)
{
    return gtk_check_button_get_active (GTK_CHECK_BUTTON (it->check));
}

gboolean
in_scope (Item *it, Item *one, gboolean sel)
{
    return one != NULL ? it == one : !sel || checked (it);
}

gboolean
can_start (Item *it)
{
    return (it->state == ST_NEW && it->probe != NULL) || it->state == ST_HELD
           || (it->state == ST_ACTIVE && it->paused);
}

gboolean
sampled_at (Item *it, const VcOptions *o)
{
    return it->sampled && it->sample_side == o->max_short_side && it->sample_fps == o->max_fps;
}

/* The size estimate: from the GPU sample taken at these settings when there is one, else the
 * formula. `measured` (may be NULL) says which. */
guint64
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
gboolean
skip_in_bulk (Item *it)
{
    VcOptions o = current_options ();
    return app.mode == VC_MODE_COMPRESS && it->state == ST_NEW && it->probe != NULL
           && !vc_worth_encoding (it->size, item_estimate (it, &o, NULL));
}

gboolean
can_pause (Item *it)
{
    return it->state == ST_QUEUED || (it->state == ST_ACTIVE && !it->paused);
}

gboolean
can_stop (Item *it)
{
    return it->state == ST_QUEUED || it->state == ST_HELD || it->state == ST_ACTIVE
           || it->state == ST_CHECKING;
}

gboolean
can_trash (Item *it)
{
    return it->state == ST_DONE && it->out != NULL && !it->webp_only && vc_mode_can_trash (app.mode);
}

/* One video at a time: x265 and the GPU path already use the whole machine. Images run several at
 * once, since magick and pngquant use about one core each (SPEC-app-shell.md Decision 4). */
guint
max_jobs (void)
{
    return app.mode == VC_MODE_IMAGES ? CLAMP (g_get_num_processors () / 2, 1, 8) : 1;
}

/* How many running jobs are paused; `first` (may be NULL) gets the first one. */
guint
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

gboolean
is_busy (void)
{
    for (guint i = 0; app.items != NULL && i < app.items->len; i++) {
        State s = ((Item *) app.items->pdata[i])->state;
        if (s == ST_QUEUED || s == ST_ACTIVE || s == ST_CHECKING)
            return TRUE;
    }
    return FALSE;
}
