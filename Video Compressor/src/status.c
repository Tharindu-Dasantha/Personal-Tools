#include "app.h"

/* ---- bottom bar, summary, selection bar ---- */

void
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

        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_add_css_class (row, "guide-row");
        if (p == app.preset)
            gtk_widget_add_css_class (row, "current");
        GtkWidget *n = gtk_label_new (name);
        gtk_label_set_xalign (GTK_LABEL (n), 0);
        gtk_size_group_add_widget (app.guide_cols[0], n);

        GtkWidget *tl = gtk_label_new (ft), *dl = gtk_label_new (delta);
        gtk_widget_add_css_class (tl, "heading");
        gtk_widget_add_css_class (dl, "caption");
        GtkWidget *th = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_append (GTK_BOX (th), tl);
        gtk_box_append (GTK_BOX (th), dl);
        GtkWidget *tb = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (tb), t / tmax);
        gtk_widget_add_css_class (tb, "time");
        GtkWidget *tv = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_box_append (GTK_BOX (tv), th);
        gtk_box_append (GTK_BOX (tv), tb);
        gtk_size_group_add_widget (app.guide_cols[1], tv);

        GtkWidget *sl = gtk_label_new (size_text);
        gtk_label_set_xalign (GTK_LABEL (sl), 0);
        gtk_widget_add_css_class (sl, "heading");
        GtkWidget *sb = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (sb), MAX (smaller, 0) / 100.0);
        gtk_widget_add_css_class (sb, "size");
        GtkWidget *sv = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
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
void
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

void
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

void
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
    gtk_widget_set_sensitive (app.target, !busy);
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

void
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
