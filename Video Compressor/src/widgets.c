#include "app.h"

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
GtkWidget *
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

GtkWidget *
vbox (int spacing)
{
    return gtk_box_new (GTK_ORIENTATION_VERTICAL, spacing);
}

GtkWidget *
hbox (int spacing)
{
    return gtk_box_new (GTK_ORIENTATION_HORIZONTAL, spacing);
}

GtkWidget *
styled_label (const char *text, const char *style, float xalign)
{
    GtkWidget *l = gtk_label_new (text);
    if (style != NULL)
        gtk_widget_add_css_class (l, style);
    gtk_label_set_xalign (GTK_LABEL (l), xalign);
    return l;
}


GtkWidget *
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


    GtkWidget *side = vbox (0);
    gtk_widget_add_css_class (side, "sidebar");
    gtk_widget_set_size_request (side, 210, -1);
    gtk_widget_set_hexpand (side, FALSE);   /* the mode rows' hexpand would otherwise let it take spare width */
    gtk_box_append (GTK_BOX (side), handle);
    gtk_box_append (GTK_BOX (side), head);
    gtk_box_append (GTK_BOX (side), app.mode_list);
    GtkWidget *space = vbox (0);
    gtk_widget_set_vexpand (space, TRUE);
    gtk_box_append (GTK_BOX (side), space);

    const char *looks[] = { "Auto", "Light", "Dark" };
    GtkWidget *look_btns[3];
    GtkWidget *look = segmented (looks, 3, app.appearance, look_btns, G_CALLBACK (on_appearance));
    gtk_accessible_update_property (GTK_ACCESSIBLE (look), GTK_ACCESSIBLE_PROPERTY_LABEL, "Appearance", -1);
    gtk_widget_set_tooltip_text (look_btns[0], "Follow the system appearance");
    GtkWidget *look_head = styled_label ("APPEARANCE", "side-head", 0);
    gtk_widget_set_margin_start (look_head, 10);
    gtk_widget_set_margin_bottom (look_head, 6);
    GtkWidget *look_box = vbox (0);
    gtk_widget_set_margin_start (look_box, 10);
    gtk_widget_set_margin_end (look_box, 10);
    gtk_widget_set_margin_bottom (look_box, 16);
    gtk_box_append (GTK_BOX (look_box), look_head);
    gtk_box_append (GTK_BOX (look_box), look);
    gtk_box_append (GTK_BOX (side), look_box);
    return side;
}

GtkWidget *
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

GtkWidget *
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
    g_object_bind_property (app.choose_btn, "visible", choose, "visible", G_BINDING_SYNC_CREATE);
    g_object_bind_property (app.files_btn, "sensitive", files, "sensitive", G_BINDING_SYNC_CREATE);
    GtkWidget *buttons = hbox (8);
    gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (buttons), choose);
    gtk_box_append (GTK_BOX (buttons), files);
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
    GtkWidget *zone = vbox (0);
    gtk_widget_add_css_class (zone, "dropzone");
    gtk_widget_set_margin_start (zone, 6);
    gtk_widget_set_margin_end (zone, 6);
    gtk_widget_set_margin_top (zone, 6);
    gtk_widget_set_margin_bottom (zone, 6);
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

GtkWidget *
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
GtkWidget *
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

GtkWidget *
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
