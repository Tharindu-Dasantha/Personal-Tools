#include "app.h"

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

GtkWidget *
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
    gtk_widget_set_valign (sw, sub ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
    GtkWidget *l = gtk_label_new_with_mnemonic (title);
    gtk_widget_add_css_class (l, "opt-title");
    gtk_label_set_xalign (GTK_LABEL (l), 0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (l), sw);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (sw), GTK_ACCESSIBLE_RELATION_LABELLED_BY, l, NULL, -1);
    GtkWidget *texts = vbox (1);
    gtk_widget_set_hexpand (texts, TRUE);
    gtk_box_append (GTK_BOX (texts), l);
    if (sub != NULL) {   /* NULL: title only */
        GtkWidget *s = styled_label (sub, "opt-sub", 0);
        gtk_label_set_wrap (GTK_LABEL (s), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (s), 20);
        gtk_box_append (GTK_BOX (texts), s);
    }
    GtkWidget *row = hbox (10);
    gtk_widget_set_margin_bottom (row, 10);
    gtk_widget_set_tooltip_text (row, tip);
    gtk_box_append (GTK_BOX (row), texts);
    gtk_box_append (GTK_BOX (row), sw);
    gtk_box_append (GTK_BOX (box), row);
    return sw;
}

/* A segmented control of toggle buttons; returns the box, fills `out` with the buttons. */
GtkWidget *
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

static GtkWidget *
build_speed_guide (void)
{
    GtkWidget *guide_head = hbox (14);
    gtk_widget_set_margin_start (guide_head, 8);
    const char *heads[] = { "SPEED", "ENCODE TIME", "FILE SIZE" };
    for (guint i = 0; i < 3; i++) {
        GtkWidget *h = styled_label (heads[i], "colhead", 0);
        gtk_size_group_add_widget (app.guide_cols[i], h);
        gtk_box_append (GTK_BOX (guide_head), h);
    }
    app.guide_rows = vbox (2);
    GtkWidget *guide = vbox (3);
    gtk_widget_add_css_class (guide, "guide");
    gtk_box_append (GTK_BOX (guide), styled_label ("Speed guide", "heading", 0));
    gtk_box_append (GTK_BOX (guide), styled_label ("Time vs Medium · size vs your originals", "caption", 0));
    gtk_widget_set_margin_top (guide_head, 6);
    gtk_box_append (GTK_BOX (guide), guide_head);
    gtk_box_append (GTK_BOX (guide), app.guide_rows);
    GtkWidget *note = styled_label ("Rough estimates for this folder. Slower speeds keep more "
                                    "detail but don't always make smaller files.", "caption", 0);
    gtk_label_set_wrap (GTK_LABEL (note), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (note), 1);   /* asks for no width: wraps to the table's */
    gtk_widget_set_margin_top (note, 6);
    gtk_box_append (GTK_BOX (guide), note);
    GtkWidget *pop = gtk_popover_new ();
    gtk_popover_set_child (GTK_POPOVER (pop), guide);
    GtkWidget *info = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (info), "help-about-symbolic");
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (info), pop);
    gtk_widget_add_css_class (info, "link");
    gtk_widget_set_tooltip_text (info, "Speed guide: time vs size");
    gtk_accessible_update_property (GTK_ACCESSIBLE (info), GTK_ACCESSIBLE_PROPERTY_LABEL, "Speed guide", -1);
    gtk_widget_set_valign (info, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (info, 16);      /* same margins as the section title, so it centres on it */
    gtk_widget_set_margin_bottom (info, 6);
    return info;
}

static GtkWidget *
build_compress_opts (void)
{
    GtkWidget *box = vbox (0);
    GtkWidget *speed_head = hbox (4);
    section (speed_head, "SPEED");
    gtk_box_append (GTK_BOX (speed_head), build_speed_guide ());
    gtk_box_append (GTK_BOX (box), speed_head);
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

    section (box, "MAX QUALITY");
    const char *sizes[] = { "Original quality", "Max 1080p", "Max 720p", NULL };
    app.max_size = gtk_drop_down_new_from_strings (sizes);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.max_size), GTK_ACCESSIBLE_PROPERTY_LABEL, "Max quality", -1);
    gtk_widget_set_tooltip_text (app.max_size, "Shrink bigger videos so the short side is at most this (a 4K "
                                               "video at 1080p takes about a quarter of the time). Smaller "
                                               "videos are never enlarged.");
    g_signal_connect (app.max_size, "notify::selected", G_CALLBACK (on_estimate_input), NULL);
    gtk_box_append (GTK_BOX (box), app.max_size);

    section (box, "TARGET SIZE");
    const char *targets[] = { "No limit (same quality)", "Under 10 MB", "Under 25 MB", "Under 50 MB",
                              "Under 100 MB", NULL };   /* current_options() has the sizes */
    app.target = gtk_drop_down_new_from_strings (targets);
    gtk_accessible_update_property (GTK_ACCESSIBLE (app.target), GTK_ACCESSIBLE_PROPERTY_LABEL, "Target size", -1);
    gtk_widget_set_tooltip_text (app.target, "Fit each video under this size, for chat apps and email, at the "
                                             "best quality that fits: the copy uses most of the size. Without the "
                                             "GPU it takes two passes, so about twice as long. Videos already "
                                             "smaller are left alone.");
    g_signal_connect (app.target, "notify::selected", G_CALLBACK (on_estimate_input), NULL);
    gtk_box_append (GTK_BOX (box), app.target);

    GtkWidget *adv = vbox (0), *outer = box;
    GtkWidget *exp = gtk_expander_new (NULL);
    gtk_expander_set_label_widget (GTK_EXPANDER (exp), styled_label ("ADVANCED SETTINGS", "sect", 0));
    gtk_expander_set_child (GTK_EXPANDER (exp), adv);
    gtk_widget_set_margin_top (exp, 16);
    gtk_widget_set_margin_top (adv, 10);
    gtk_box_append (GTK_BOX (outer), exp);
    box = adv;
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
    return outer;
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
    app.no_audio = option_row (box, "Remove _audio", NULL,
                               "No audio track, for muted autoplay backgrounds.");
    return box;
}

/* ---- frames range slider: a Start knob and an End knob on one track ---- */
#define RANGE_KNOB 20

static gboolean
range_child_pos (GtkOverlay *o, GtkWidget *child, GdkRectangle *r, gpointer user_data)
{
    (void) user_data;
    int w = gtk_widget_get_width (GTK_WIDGET (o)) - RANGE_KNOB, h = gtk_widget_get_height (GTK_WIDGET (o));
    double d = app.range_dur;
    int xs = d > 0 ? (int) lround (app.range_s / d * w) : 0, xe = d > 0 ? (int) lround (app.range_e / d * w) : w;
    if (child == app.range_fill)
        *r = (GdkRectangle) { xs + RANGE_KNOB / 2, h / 2 - 2, MAX (xe - xs, 0), 4 };
    else
        *r = (GdkRectangle) { child == app.range_knob[0] ? xs : xe, h / 2 - RANGE_KNOB / 2, RANGE_KNOB, RANGE_KNOB };
    return TRUE;
}

/* Writes the dragged knob's time into its field; the field's "changed" moves the knob. */
static void
range_set_x (double x)
{
    int w = MAX (gtk_widget_get_width (app.range) - RANGE_KNOB, 1);
    double d = app.range_dur, t = floor (CLAMP ((x - RANGE_KNOB / 2.0) / w, 0, 1) * d * 10) / 10;   /* 0.1 s */
    g_autofree char *text = NULL;
    if (app.range_drag == 0) {
        t = MIN (t, app.range_e - 0.1);
        text = t > 0 ? format_clock (t) : g_strdup ("");
        gtk_editable_set_text (GTK_EDITABLE (app.frames_start), text);
    } else {
        t = MAX (t, app.range_s + 0.1);
        text = t < d - 0.05 ? format_clock (t) : g_strdup ("");   /* empty = to the end */
        gtk_editable_set_text (GTK_EDITABLE (app.frames_end), text);
    }
}

static void
on_range_begin (GtkGestureDrag *g, double x, double y, gpointer user_data)
{
    (void) y; (void) user_data;
    if (app.range_dur <= 0) {
        gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    int w = gtk_widget_get_width (app.range) - RANGE_KNOB;
    double xs = app.range_s / app.range_dur * w + RANGE_KNOB / 2.0;
    double xe = app.range_e / app.range_dur * w + RANGE_KNOB / 2.0;
    /* the nearer knob; when they touch, the side you grab from */
    app.range_drag = fabs (x - xs) < fabs (x - xe) || (xs == xe && x < xs) ? 0 : 1;
    range_set_x (x);
}

static void
on_range_update (GtkGestureDrag *g, double dx, double dy, gpointer user_data)
{
    (void) dy; (void) user_data;
    double x0, y0;
    if (gtk_gesture_drag_get_start_point (g, &x0, &y0))
        range_set_x (x0 + dx);
}

static GtkWidget *
range_slider (void)
{
    GtkWidget *trough = vbox (0);
    gtk_widget_add_css_class (trough, "range-trough");
    gtk_widget_set_valign (trough, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start (trough, RANGE_KNOB / 2);
    gtk_widget_set_margin_end (trough, RANGE_KNOB / 2);
    GtkWidget *o = app.range = gtk_overlay_new ();
    gtk_widget_add_css_class (o, "range");
    gtk_widget_set_size_request (o, -1, RANGE_KNOB + 4);
    gtk_overlay_set_child (GTK_OVERLAY (o), trough);
    app.range_fill = vbox (0);
    gtk_widget_add_css_class (app.range_fill, "range-fill");
    gtk_overlay_add_overlay (GTK_OVERLAY (o), app.range_fill);
    for (guint i = 0; i < 2; i++) {
        app.range_knob[i] = vbox (0);
        gtk_widget_add_css_class (app.range_knob[i], "range-knob");
        gtk_widget_set_cursor_from_name (app.range_knob[i], "grab");
        gtk_overlay_add_overlay (GTK_OVERLAY (o), app.range_knob[i]);
    }
    g_signal_connect (o, "get-child-position", G_CALLBACK (range_child_pos), NULL);
    GtkGesture *drag = gtk_gesture_drag_new ();
    g_signal_connect (drag, "drag-begin", G_CALLBACK (on_range_begin), NULL);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_range_update), NULL);
    gtk_widget_add_controller (o, GTK_EVENT_CONTROLLER (drag));
    gtk_widget_set_tooltip_text (o, "Drag the two handles to pick the part of the video to use. "
                                    "Type in Start and End to fine-tune.");
    gtk_widget_set_margin_bottom (o, 8);
    gtk_widget_set_sensitive (o, FALSE);   /* check_range enables it once the video is probed */
    return o;
}

static GtkWidget *
build_frames_opts (void)
{
    GtkWidget *box = app.frames_opts = vbox (0);
    section (box, "FRAMES PER SECOND");
    const char *rates[] = { "5", "10", "12", "15", "24", "30" };
    G_STATIC_ASSERT (G_N_ELEMENTS (rates) == G_N_ELEMENTS (FRAME_RATES));
    app.fps_idx = FRAMES_DEFAULT_RATE;
    GtkWidget *seg = segmented (rates, G_N_ELEMENTS (rates), FRAMES_DEFAULT_RATE, app.fps_btns, G_CALLBACK (on_fps));
    gtk_widget_set_tooltip_text (seg, "Frames saved per second of video.");
    gtk_box_append (GTK_BOX (box), seg);

    section (box, "RANGE");
    gtk_box_append (GTK_BOX (box), range_slider ());
    GtkWidget *range = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (range), 8);
    gtk_grid_set_row_spacing (GTK_GRID (range), 4);
    gtk_grid_set_column_homogeneous (GTK_GRID (range), TRUE);
    GtkWidget **times[] = { &app.frames_start, &app.frames_end };
    const char *time_label[] = { "Sta_rt", "_End" };
    const char *time_hint[] = { "0:00", "end" };
    const char *time_tip[] = { "Where to start: empty for the beginning, or 12, 12.5, 1:05, 0:01:05.",
                               "Where to stop: empty for the end of the video." };
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
    app.quality_val = styled_label ("80%", "hint", 1);
    GtkWidget *q_head = hbox (8);
    gtk_widget_set_margin_top (q_head, 18);
    gtk_box_append (GTK_BOX (q_head), q_label);
    gtk_box_append (GTK_BOX (q_head), app.quality_val);
    gtk_box_append (GTK_BOX (box), q_head);
    app.frames_quality = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
    gtk_range_set_value (GTK_RANGE (app.frames_quality), 80);
    gtk_widget_set_tooltip_text (app.frames_quality, "1–100%. Higher keeps more detail in bigger files.");
    gtk_label_set_mnemonic_widget (GTK_LABEL (q_label), app.frames_quality);
    gtk_accessible_update_relation (GTK_ACCESSIBLE (app.frames_quality), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                    q_label, NULL, -1);
    g_signal_connect (app.frames_quality, "value-changed", G_CALLBACK (on_quality), NULL);
    gtk_widget_set_margin_top (app.frames_quality, 6);
    gtk_box_append (GTK_BOX (box), app.frames_quality);
    return box;
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
    section (box, "ORIGINALS");
    app.img_trash = option_row (box, "Move originals to _Trash", "Only when the same-type copy opens at the "
                                "right size.",
                                "After each same-type copy (jpg or png) is checked: it opens, has the expected "
                                "size and is at least 5% smaller. Asks before starting.");
    return box;
}

GtkWidget *
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
