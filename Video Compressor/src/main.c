#include "app.h"

static void stop_for_quit (void);

App app;

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

gboolean
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

static void
add_shortcut (GtkEventController *keys, const char *trigger, GtkShortcutFunc func)
{
    gtk_shortcut_controller_add_shortcut (
        GTK_SHORTCUT_CONTROLLER (keys),
        gtk_shortcut_new (gtk_shortcut_trigger_parse_string (trigger),
                          gtk_callback_action_new (func, NULL, NULL)));
}

void
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
    load_appearance ();   /* before build_sidebar: it sets the Appearance control */
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
    g_autoptr (GtkApplication) gapp = gtk_application_new ("io.github.Tharindu_Dasantha.MediaTools",
                                                           G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect (gapp, "activate", G_CALLBACK (activate), NULL);
    return g_application_run (G_APPLICATION (gapp), argc, argv);
}
