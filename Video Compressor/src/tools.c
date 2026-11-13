#include "app.h"

/* ---- startup ---- */

static void
quit_after_alert (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);
    gtk_window_destroy (app.win);
}

static void
tools_failed (const char *problem)
{
    if (!app.closing)
        alert ("Media Tools can't run", problem, quit_after_alert);
}

void
gpu_test_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    if (!g_subprocess_wait_check_finish (proc, res, NULL) || app.closing)
        return;   /* no working GPU encoder: the GPU speed stays hidden */
    app.gpu_ok = TRUE;
    gtk_widget_set_visible (gtk_widget_get_parent (app.gpu), TRUE);
    update_speed_guide ();
    update_controls ();   /* starts size samples if a folder is already loaded */
}

static void
filters_listed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, NULL)
        || !g_subprocess_get_successful (proc)) {
        tools_failed ("ffmpeg -filters failed. Reinstall ffmpeg.");
        return;
    }
    if (app.closing)
        return;
    if (strstr (out, " zscale ") != NULL)
        app.have |= VC_HAVE_ZSCALE;
    app.tools_ok = TRUE;
    update_controls ();
}

/* Each mode's own tool is checked here (SPEC-app-shell.md D6): a missing one shows the banner in
 * that mode only. */
static void
encoders_listed (GObject *source, GAsyncResult *res, gpointer user_data)
{
    const char *ffmpeg = user_data;
    g_autoptr (GSubprocess) proc = G_SUBPROCESS (source);
    g_autofree char *out = NULL;
    if (!g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, NULL)
        || !g_subprocess_get_successful (proc)) {
        tools_failed ("ffmpeg -encoders failed. Reinstall ffmpeg.");
        return;
    }
    if (app.closing)
        return;
    static const struct { const char *name; guint bit; } encoders[] = {
        { " libx265 ", VC_HAVE_X265 }, { " libx264 ", VC_HAVE_X264 }, { " libwebp ", VC_HAVE_LIBWEBP },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (encoders); i++)
        if (strstr (out, encoders[i].name) != NULL)
            app.have |= encoders[i].bit;
    const char *argv[] = { ffmpeg, "-hide_banner", "-filters", NULL };
    g_autoptr (GError) error = NULL;
    GSubprocess *filters = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                    | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (filters == NULL) {
        tools_failed (error->message);
        return;
    }
    g_subprocess_communicate_utf8_async (filters, NULL, NULL, filters_listed, NULL);

    /* The list shows hardware encoders even when they can't run, so try one frame. */
    if (strstr (out, " hevc_vaapi ") == NULL)
        return;
    g_auto (GStrv) test_argv = vc_build_gpu_test_argv ();
    GSubprocess *test = g_subprocess_newv ((const char *const *) test_argv,
                                           G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                           | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    if (test != NULL)
        g_subprocess_wait_check_async (test, NULL, gpu_test_done, NULL);
}

/* Asynchronous so the window paints at once; Choose Folder stays off until it passes. Only ffmpeg
 * and ffprobe are needed to start: every mode reads files with ffprobe. */
void
check_tools (void)
{
    static char *ffmpeg;   /* kept for encoders_listed */
    ffmpeg = g_find_program_in_path ("ffmpeg");
    g_autofree char *ffprobe = g_find_program_in_path ("ffprobe");
    if (ffmpeg == NULL || ffprobe == NULL) {
        tools_failed ("ffmpeg and ffprobe must be installed and on PATH.\n"
                      "On Fedora: sudo dnf install ffmpeg (from RPM Fusion).");
        return;
    }
    g_autofree char *magick = g_find_program_in_path ("magick");
    g_autofree char *pngquant = g_find_program_in_path ("pngquant");
    app.have |= (magick != NULL ? VC_HAVE_MAGICK : 0) | (pngquant != NULL ? VC_HAVE_PNGQUANT : 0);
    const char *argv[] = { ffmpeg, "-hide_banner", "-encoders", NULL };
    g_autoptr (GError) error = NULL;
    GSubprocess *proc = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                 | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
    if (proc == NULL)
        tools_failed (error->message);
    else
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, encoders_listed, ffmpeg);
}
