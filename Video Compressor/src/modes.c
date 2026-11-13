#include "modes.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *const IMAGE_EXTS[] = { "jpg", "jpeg", "png", "webp", "bmp", "tif", "tiff" };

gboolean
vc_mode_accepts (VcMode mode, const char *name)
{
    if (mode != VC_MODE_IMAGES)
        return vc_is_video_name (name);
    const char *dot = strrchr (name, '.');
    if (name[0] == '.' || dot == NULL)   /* hidden names cover our own `.x.part.png` files */
        return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS (IMAGE_EXTS); i++)
        if (g_ascii_strcasecmp (dot + 1, IMAGE_EXTS[i]) == 0)
            return TRUE;
    return FALSE;
}

const char *
vc_mode_out_dir (VcMode mode)
{
    return mode == VC_MODE_WEB_VIDEO ? "web" : mode == VC_MODE_FRAMES ? "frames" : "compressed";
}

gboolean
vc_mode_can_trash (VcMode mode)
{
    /* Web video and frames are lossy derivatives, not replacements. */
    return mode == VC_MODE_COMPRESS || mode == VC_MODE_IMAGES;
}

const char *
vc_mode_missing_tool (VcMode mode, guint have)
{
    static const struct { VcMode mode; guint bit; const char *name; } needs[] = {
        { VC_MODE_COMPRESS, VC_HAVE_X265, "libx265" },
        { VC_MODE_WEB_VIDEO, VC_HAVE_X264, "libx264" }, { VC_MODE_WEB_VIDEO, VC_HAVE_ZSCALE, "zscale" },
        { VC_MODE_FRAMES, VC_HAVE_LIBWEBP, "libwebp" }, { VC_MODE_FRAMES, VC_HAVE_ZSCALE, "zscale" },
        { VC_MODE_IMAGES, VC_HAVE_MAGICK, "magick" }, { VC_MODE_IMAGES, VC_HAVE_PNGQUANT, "pngquant" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (needs); i++)
        if (needs[i].mode == mode && !(have & needs[i].bit))
            return needs[i].name;
    return NULL;
}

static GPtrArray *
compress_steps (const char *input, const VcProbe *probe, const VcOptions *opt, char **skip_reason)
{
    char *part = NULL;
    char *out = vc_output_path (input, opt->keep_subs, &part);
    if (g_file_test (out, G_FILE_TEST_EXISTS)) {
        g_autofree char *base = g_path_get_basename (out);
        g_autofree char *display = g_filename_display_name (base);
        *skip_reason = g_strdup_printf ("compressed/%s exists", display);
        g_free (out);
        g_free (part);
        return NULL;
    }
    GPtrArray *steps = g_ptr_array_new_with_free_func ((GDestroyNotify) vc_step_free);
    g_autofree char *passlog = NULL;
    if (vc_target_video_kbps (probe, opt) > 0 && g_strcmp0 (opt->preset, VC_PRESET_GPU) != 0) {
        /* x265 two-pass: pass 1's stats go in a hidden folder the runner removes with the job */
        g_autofree char *dir = g_path_get_dirname (out);
        g_autofree char *base = g_path_get_basename (out);
        *strrchr (base, '.') = '\0';
        g_autofree char *name = g_strdup_printf (".%s.2pass.part", base);
        VcStep *p1 = g_new0 (VcStep, 1);
        p1->part = g_build_filename (dir, name, NULL);
        p1->part_is_dir = TRUE;
        p1->progress = TRUE;
        passlog = g_build_filename (p1->part, "x265", NULL);
        p1->argv = vc_build_ffmpeg_pass_argv (input, NULL, probe, opt, 1, passlog);
        g_ptr_array_add (steps, p1);
    }
    VcStep *s = g_new0 (VcStep, 1);
    s->argv = vc_build_ffmpeg_pass_argv (input, part, probe, opt, passlog != NULL ? 2 : 0, passlog);
    s->part = part;
    s->output = out;
    s->progress = TRUE;
    g_ptr_array_add (steps, s);
    return steps;
}

GPtrArray *
vc_mode_steps (VcMode mode, const char *input, const VcProbe *probe, const VcOptions *opt,
               char **skip_reason)
{
    *skip_reason = NULL;
    if (mode == VC_MODE_COMPRESS)
        return compress_steps (input, probe, opt, skip_reason);
    if (mode == VC_MODE_WEB_VIDEO)
        return vc_web_video_steps (input, probe, opt, skip_reason);
    if (mode == VC_MODE_FRAMES)
        return vc_frames_steps (input, probe, opt, skip_reason);
    return vc_image_steps (input, probe, opt, skip_reason);
}

void
vc_step_free (VcStep *step)
{
    if (step == NULL)
        return;
    g_strfreev (step->argv);
    g_free (step->part);
    g_free (step->output);
    g_free (step);
}

static gboolean
is_part_name (const char *path)
{
    g_autofree char *base = g_path_get_basename (path);
    if (g_str_has_suffix (base, ".part"))
        return TRUE;
    char *dot = strrchr (base, '.');
    if (dot == NULL || dot == base)
        return FALSE;
    *dot = '\0';
    return g_str_has_suffix (base, ".part");
}

gboolean
vc_remove_part (const char *path)
{
    if (!is_part_name (path))
        return FALSE;
    GStatBuf st;
    if (g_lstat (path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR (st.st_mode))
        return g_unlink (path) == 0;

    /* Check everything first, so a refused directory is left exactly as it was. */
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    if (dir == NULL)
        return FALSE;
    g_autoptr (GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
    for (const char *name; (name = g_dir_read_name (dir)) != NULL;) {
        char *child = g_build_filename (path, name, NULL);
        g_ptr_array_add (files, child);
        if (g_lstat (child, &st) != 0 || S_ISDIR (st.st_mode))
            return FALSE;
    }
    gboolean ok = TRUE;
    for (guint i = 0; i < files->len; i++)
        ok &= g_unlink (files->pdata[i]) == 0;
    return ok && g_rmdir (path) == 0;
}
