#include "app.h"

/* ---- one row ---- */

void
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

void
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

void
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

GtkWidget *
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
void
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
