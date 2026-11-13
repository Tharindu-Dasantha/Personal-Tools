#include "app.h"

/* The v3 design (Figma "v3 – Media Tools"): colours are variables, so a mode's accent and the dark
 * appearance each change one place. */
#define DARK_VARS \
    "  --text: #f5f5f7; --text2: #a1a1a6; --text3: #6e6e73; --bg: #1e1e20; --side: #28282b;" \
    "  --insp: #232326; --fill: #323236; --line: #38383c; --card: #2c2c2f; --sel: #1e1e20;" \
    "  --seg-on: #5a5a5f; --shadow: rgba(0,0,0,0.4); --green: #32d74b; --green-bg: #17351f;" \
    "  --amber: #ffb340; --amber-bg: #3a2a12; --red: #ff6961; --red-bg: #3d1c1e; --teal: #5ac8dd;" \
    "  --teal-bg: #123238;"
const char CSS[] =
    "window.mt, window.close-dialog {"
    "  --accent: #0a84ff; --text: #1d1d1f; --text2: #6e6e73; --text3: #a1a1a6;"
    "  --bg: #ffffff; --side: #f3f3f6; --insp: #fafafc; --fill: #f2f2f5; --line: #e6e6eb;"
    "  --card: #ffffff; --sel: #ffffff; --seg-on: #ffffff; --shadow: rgba(0,0,0,0.08);"
    "  --green: #1f8f4e; --green-bg: #e6f6ec; --amber: #b25e00; --amber-bg: #fff3e0;"
    "  --red: #d70015; --red-bg: #ffe9eb; --teal: #0f7f91; --teal-bg: #e3f6f9;"
    "  color: var(--text); background: var(--bg); font-size: 13px; font-family: Inter, sans-serif; }"
    "tooltip { font-family: Inter, sans-serif; }"
    /* sync_dark sets .dark from the system scheme or the sidebar's Appearance choice */
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
    /* 16px dots, 9px apart; the theme's border-spacing would widen the gaps */
    ".sidebar windowcontrols { margin: 12px 0 0 12px; border-spacing: 0; }"
    ".sidebar windowcontrols button { min-width: 16px; min-height: 16px; padding: 0; margin: 0 9px 0 0;"
    "  border-radius: 999px; border: none; background: #ff5f57; color: #4d0000;"
    "  box-shadow: inset 0 0 0 1px rgba(0,0,0,0.14); transition: background 120ms; }"
    ".sidebar windowcontrols button.minimize { background: #febc2e; color: #995700; }"
    ".sidebar windowcontrols button.maximize { background: #28c840; color: #006500; }"
    ".sidebar windowcontrols button:active { filter: brightness(0.85); }"
    ".sidebar windowcontrols button image { background: none; box-shadow: none; padding: 0; margin: 0;"
    "  min-width: 0; min-height: 0; -gtk-icon-size: 11px; color: inherit; opacity: 0;"
    "  transition: opacity 100ms; }"
    ".sidebar windowcontrols:hover button image { opacity: 1; }"
    /* unfocused window: dots go neutral grey, like macOS */
    "window:backdrop .sidebar windowcontrols:not(:hover) button { background: color-mix(in srgb, var(--text) 18%, transparent); }"
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
    ".dropzone { border: 1px dashed color-mix(in srgb, var(--text) 25%, transparent); border-radius: 8px; padding: 32px 48px; }"
    ".drop-icon { background: linear-gradient(160deg, color-mix(in srgb, var(--accent) 75%, white), var(--accent));"
    "  color: white; border-radius: 18px; min-width: 76px; min-height: 76px; -gtk-icon-size: 34px;"
    "  box-shadow: 0 6px 18px color-mix(in srgb, var(--accent) 35%, transparent); }"
    ".hero { font-size: 22px; font-weight: 700; }"
    ".empty-sub { color: var(--text2); font-size: 14px; }"

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
    "  margin-right: 4px; color: var(--text2); }"
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
    "menubutton.link image { -gtk-icon-size: 13px; }"
    "expander-widget > box > title { padding: 0; }"
    "expander-widget > box > title > expander { -gtk-icon-size: 12px; min-width: 12px; min-height: 12px;"
    "  margin-right: 4px; color: var(--text2); }"
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
    ".range-trough { background: var(--fill); min-height: 4px; border-radius: 2px; }"
    ".range-fill { background: var(--accent); border-radius: 2px; }"
    ".range-knob { background: white; min-width: 20px; min-height: 20px; border-radius: 999px;"
    "  box-shadow: 0 1px 3px rgba(0,0,0,0.3); }"
    ".range:disabled { opacity: 0.4; }"
    ".guide { padding: 6px; font-size: 12px; }"
    ".guide .heading { font-size: 12px; }"
    ".guide .caption { font-size: 11px; }"
    ".guide .colhead { font-size: 10px; }"
    ".guide-row { padding: 4px 8px; border-radius: 6px; }"
    ".guide progressbar trough, .guide progressbar progress { min-height: 3px; }"
    ".guide progressbar trough { min-width: 96px; }"
    ".guide-row.current { background: color-mix(in srgb, var(--accent) 14%, transparent); }"
    ".guide-row.current > label:first-child { color: var(--accent); font-weight: 600; }"
    ".caption { font-size: 12px; opacity: 0.6; }";

/* The design's font, inside the binary so the app needs no install or internet. Pango loads fonts
 * from files, so each one is copied once into the user's cache. Any failure leaves the system font. */
void
load_fonts (void)
{
    static const char *const names[] = { "Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf",
                                         "Inter-Bold.ttf" };
    g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "media-tools", "fonts", NULL);
    if (g_mkdir_with_parents (dir, 0755) != 0)
        return;
    for (gsize i = 0; i < G_N_ELEMENTS (names); i++) {
        g_autofree char *res = g_strconcat ("/io/github/Tharindu_Dasantha/MediaTools/fonts/", names[i], NULL);
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

static char *
settings_file (void)
{
    return g_build_filename (g_get_user_config_dir (), "media-tools", "settings.ini", NULL);
}

void
load_appearance (void)
{
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_autofree char *f = settings_file ();
    if (g_key_file_load_from_file (k, f, G_KEY_FILE_NONE, NULL))
        app.appearance = MIN ((guint) g_key_file_get_integer (k, "ui", "appearance", NULL), 2);
}

/* Dark when Appearance says so, or on Auto when the colour scheme is dark or the app is asked for
 * a dark theme. */
void
sync_dark (void)
{
    GtkInterfaceColorScheme scheme;
    gboolean prefer;
    g_object_get (gtk_settings_get_default (), "gtk-interface-color-scheme", &scheme,
                  "gtk-application-prefer-dark-theme", &prefer, NULL);
    GtkWidget *w[] = { GTK_WIDGET (app.win), app.close_dialog };
    for (gsize i = 0; i < G_N_ELEMENTS (w) && w[i] != NULL; i++) {
        if (app.appearance == 2
            || (app.appearance == 0 && (scheme == GTK_INTERFACE_COLOR_SCHEME_DARK || prefer)))
            gtk_widget_add_css_class (w[i], "dark");
        else
            gtk_widget_remove_css_class (w[i], "dark");
    }
}

void
on_appearance (GtkToggleButton *b, gpointer user_data)
{
    if (!gtk_toggle_button_get_active (b))
        return;
    app.appearance = GPOINTER_TO_UINT (user_data);
    sync_dark ();
    g_autoptr (GKeyFile) k = g_key_file_new ();
    g_autofree char *f = settings_file ();
    g_key_file_load_from_file (k, f, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_set_integer (k, "ui", "appearance", (gint) app.appearance);
    g_autofree char *dir = g_path_get_dirname (f);
    if (g_mkdir_with_parents (dir, 0755) == 0)
        g_key_file_save_to_file (k, f, NULL);   /* best effort: only the next launch's look depends on it */
}
