#include "app.h"

static void probe_step (void);

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
void
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
void
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

void
on_choose (void)
{
    g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
    g_autofree char *title = g_strdup_printf ("Choose a folder of %ss", noun ());
    gtk_file_dialog_set_title (d, title);
    gtk_file_dialog_select_folder (d, app.win, NULL, folder_chosen, NULL);
}

/* Flatpak: files picked outside Videos, Pictures and Downloads come one by one through the document
 * portal, and their folder can't be written, so no compressed/ could go beside them. Ask for that
 * folder, then list the same files inside it. */
static char **pending_files;

static void
folder_granted (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GFile) file = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
    g_autofree char *folder = file != NULL ? g_file_get_path (file) : NULL;
    g_auto (GStrv) files = g_steal_pointer (&pending_files);
    if (folder == NULL || files == NULL)
        return;
    /* shortcut: files from another folder are dropped silently; the file dialog picks from one folder */
    g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
    for (char **f = files; *f != NULL; f++) {
        g_autofree char *base = g_path_get_basename (*f);
        g_autofree char *inside = g_build_filename (folder, base, NULL);
        if (g_file_test (inside, G_FILE_TEST_IS_REGULAR))
            g_strv_builder_add (b, inside);
    }
    g_auto (GStrv) found = g_strv_builder_end (b);
    if (found[0] != NULL) {
        load_source (NULL, found);
        return;
    }
    g_autofree char *name = basename_display (files[0]);
    g_autofree char *detail = g_strdup_printf ("Choose the folder that holds “%s”.", name);
    alert ("The files aren't in that folder", detail, NULL);
}

static void
folder_asked (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL) != 1) {
        g_clear_pointer (&pending_files, g_strfreev);
        return;
    }
    g_autoptr (GtkFileDialog) d = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (d, "Choose the folder the files are in");
    g_autofree char *host = pending_files != NULL ? host_path (pending_files[0]) : NULL;
    if (host != NULL) {   /* open the dialog where the files are */
        g_autofree char *dir = g_path_get_dirname (host);
        g_autoptr (GFile) start = g_file_new_for_path (dir);
        gtk_file_dialog_set_initial_folder (d, start);
    }
    gtk_file_dialog_select_folder (d, app.win, NULL, folder_granted, NULL);
}

static void
load_files (char **files)
{
    g_autofree char *root = g_build_filename (g_get_user_runtime_dir (), "doc", NULL);
    for (char **f = files; *f != NULL; f++) {
        if (vc_is_lone_document (*f, root)) {
            g_strfreev (pending_files);
            pending_files = g_strdupv (files);
            g_autofree char *detail = g_strdup_printf (
                "Media Tools can read the files you picked, but to save %s/ next to them it needs their "
                "folder too. Choose the folder they're in.", vc_mode_out_dir (app.mode));
            confirm ("Allow saving next to these files?", detail, "Choose Folder…", folder_asked);
            return;
        }
    }
    load_source (NULL, files);
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
        load_files (files);
}

static void
file_chosen (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GFile) f = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
    g_autofree char *path = f != NULL ? g_file_get_path (f) : NULL;
    char *files[] = { path, NULL };
    if (path != NULL)
        load_files (files);
}

void
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
        gtk_file_dialog_set_title (d, app.mode == VC_MODE_FRAMES ? "Choose a video" : "Choose videos");
        gtk_file_filter_set_name (f, "Videos");
        gtk_file_filter_add_mime_type (f, "video/*");
        gtk_file_filter_add_suffix (f, "ts");   /* often typed as a Qt translation file */
    }
    gtk_file_dialog_set_default_filter (d, f);
    if (app.mode == VC_MODE_FRAMES)   /* one video at a time */
        gtk_file_dialog_open (d, app.win, NULL, file_chosen, NULL);
    else
        gtk_file_dialog_open_multiple (d, app.win, NULL, files_chosen, NULL);
}

gboolean
on_drop (GtkDropTarget *t, const GValue *value, double x, double y, gpointer user_data)
{
    (void) t; (void) x; (void) y; (void) user_data;
    if (!app.tools_ok || is_busy () || !G_VALUE_HOLDS (value, G_TYPE_FILE))
        return FALSE;
    g_autofree char *path = g_file_get_path (g_value_get_object (value));
    if (path == NULL)
        return FALSE;
    if (app.mode == VC_MODE_FRAMES) {   /* one video, never a folder */
        g_autofree char *base = g_path_get_basename (path);
        char *files[] = { path, NULL };
        if (!g_file_test (path, G_FILE_TEST_IS_REGULAR) || !vc_mode_accepts (app.mode, base))
            return FALSE;
        load_files (files);
        return TRUE;
    }
    if (!g_file_test (path, G_FILE_TEST_IS_DIR))
        return FALSE;
    load_source (path, NULL);
    return TRUE;
}

/* Estimates depend on the speed, the size cap and (through samples) the frame-rate cap. */
void
on_estimate_input (void)
{
    for (guint i = 0; app.items != NULL && i < app.items->len; i++)
        render_row (app.items->pdata[i]);
    update_speed_guide ();
    update_controls ();
}

void
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
                                          "Numbered from 0001, in a folder named after the video.", app.webp ? "webp" : "jpg");
    gtk_label_set_markup (GTK_LABEL (app.insp_note), m);
}

/* Everything that follows the mode without a reload: accent, inspector, empty page. */
void
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
    gtk_label_set_text (GTK_LABEL (app.hero), app.mode == VC_MODE_FRAMES ? "Drop a video" : hero);
    gtk_widget_set_visible (app.choose_btn, app.mode != VC_MODE_FRAMES);   /* frames takes one video */
}

void
on_gpu (void)
{
    app.preset = gtk_switch_get_active (GTK_SWITCH (app.gpu)) ? 0 : app.speed;
    on_estimate_input ();
}

/* Locked while busy (update_controls). Rows start fresh: the list is rebuilt from the same
 * source with the new mode's filter (SPEC-app-shell.md D3). */
void
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
    if (app.items != NULL && m == VC_MODE_FRAMES && (app.folder != NULL || g_strv_length (app.files) != 1)) {
        /* frames takes one video: a folder or several files go back to the empty page */
        g_cancellable_cancel (app.probe_cancel);
        sample_stop ();   /* before the items it points at are freed */
        gtk_list_box_remove_all (GTK_LIST_BOX (app.list));
        g_clear_pointer (&app.items, g_ptr_array_unref);
        g_clear_pointer (&app.folder, g_free);
        g_clear_pointer (&app.files, g_strfreev);
        app.anchor = NULL;
        update_header ();
        update_controls ();
    } else if (app.items != NULL) {
        load_source (app.folder, app.files);
    } else {
        update_controls ();
    }
}

void
on_fps (GtkToggleButton *b, gpointer user_data)
{
    if (!gtk_toggle_button_get_active (b))
        return;
    app.fps_idx = GPOINTER_TO_UINT (user_data);
    on_estimate_input ();
}

void
on_format (GtkToggleButton *b, gpointer user_data)
{
    if (!gtk_toggle_button_get_active (b))
        return;
    app.webp = GPOINTER_TO_UINT (user_data) == 0;
    gtk_label_set_text (GTK_LABEL (app.fmt_caption), app.webp ? "Smaller, plays in Safari 14 and later."
                                                              : "Bigger files that open everywhere.");
    update_frames_note ();
}

void
on_quality (GtkRange *r, gpointer user_data)
{
    (void) user_data;
    g_autofree char *v = g_strdup_printf ("%d%%", (int) gtk_range_get_value (r));
    gtk_label_set_text (GTK_LABEL (app.quality_val), v);
}

void
on_copy_fix (void)
{
    gdk_clipboard_set_text (gtk_widget_get_clipboard (app.banner),
                            gtk_label_get_text (GTK_LABEL (app.banner_code)));
    show_toast ("edit-copy-symbolic", "Copied", "Paste it into a terminal");
}
