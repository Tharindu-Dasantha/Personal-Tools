#include "app.h"

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

void
on_start_all (void)
{
    if (paused_jobs (NULL) > 0) {   /* Resume: continue the paused jobs only */
        pause_all (FALSE);
        update_controls ();
    } else {
        act_start (NULL, FALSE);
    }
}

void
on_pause_all (void)
{
    pause_all (TRUE);
    update_controls ();
}

void on_stop_all (void) { act_stop (NULL, FALSE); }
void on_sel_start (void) { act_start (NULL, TRUE); }
void on_sel_pause (void) { act_pause (NULL, TRUE); }
void on_sel_stop (void) { act_stop (NULL, TRUE); }

void
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

void
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

void on_sel_clear (void) { set_all_checked (FALSE); }

void
on_select_all (GtkCheckButton *b, gpointer user_data)
{
    (void) user_data;
    if (!app.syncing)
        set_all_checked (gtk_check_button_get_active (b));
}

void
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

GtkWidget *
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
