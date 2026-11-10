#include "modes.h"

#include <string.h>

static char *
basename_of (const char *path)
{
    g_autofree char *base = g_path_get_basename (path);
    return g_filename_display_name (base);
}

/* Every web output is SDR 8-bit H.264 in MP4 (SPEC-web-video.md, "Encode settings"). */
static char **
build_argv (const char *input, const char *part, const VcProbe *probe, const VcOptions *opt)
{
    GStrvBuilder *b = g_strv_builder_new ();
    /* -y only ever hits our own .part temp file; the final name is checked before encoding. */
    g_strv_builder_add_many (b, "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error",
                             "-nostats", "-progress", "pipe:1", "-y", "-i", input,
                             "-map", "0:v:0", NULL);
    if (!opt->no_audio)
        g_strv_builder_add_many (b, "-map", "0:a:0?", NULL);

    /* iw is the display width: autorotate runs before this filter. Never upscale, always even. */
    GString *vf = g_string_new (NULL);
    if (opt->max_width > 0)
        g_string_append_printf (vf, "scale='trunc(min(iw,%d)/2)*2':-2", opt->max_width);
    else
        g_string_append (vf, "scale='trunc(iw/2)*2':-2");
    if (vc_probe_is_hdr (probe))
        g_string_append_printf (vf, ",zscale=tin=%s:pin=%s:min=%s:t=linear:npl=100,format=gbrpf32le,"
                                "zscale=p=bt709,tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv",
                                probe->color_transfer,
                                probe->color_primaries ? probe->color_primaries : "bt2020",
                                probe->color_space ? probe->color_space : "bt2020nc");
    g_string_append (vf, ",format=yuv420p");
    g_strv_builder_add_many (b, "-vf", vf->str, "-c:v", "libx264", "-profile:v", "high",
                             "-preset", "slow", "-crf", "23", "-pix_fmt", "yuv420p", NULL);
    g_string_free (vf, TRUE);

    if (opt->no_audio)
        g_strv_builder_add (b, "-an");
    else
        g_strv_builder_add_many (b, "-c:a", "aac", "-b:a", "128k", "-ac", "2", NULL);
    /* No metadata: phone clips carry GPS location. */
    g_strv_builder_add_many (b, "-sn", "-dn", "-map_metadata", "-1", "-map_chapters", "-1",
                             "-movflags", "+faststart", part, NULL);
    char **argv = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    return argv;
}

char *
vc_web_video_output (const char *input, char **part_path)
{
    g_autofree char *dir = g_path_get_dirname (input);
    g_autofree char *base = g_path_get_basename (input);
    char *dot = strrchr (base, '.');
    if (dot != NULL && dot != base)
        *dot = '\0';
    if (part_path != NULL) {
        g_autofree char *part_name = g_strdup_printf (".%s.part.mp4", base);
        *part_path = g_build_filename (dir, "web", part_name, NULL);
    }
    g_autofree char *name = g_strdup_printf ("%s.mp4", base);
    return g_build_filename (dir, "web", name, NULL);
}

GPtrArray *
vc_web_video_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                    char **skip_reason)
{
    *skip_reason = NULL;
    char *part = NULL;
    char *out = vc_web_video_output (input, &part);
    if (g_file_test (out, G_FILE_TEST_EXISTS)) {
        g_autofree char *display = basename_of (out);
        *skip_reason = g_strdup_printf ("web/%s exists", display);
        g_free (out);
        g_free (part);
        return NULL;
    }
    VcStep *s = g_new0 (VcStep, 1);
    s->part = part;
    s->argv = build_argv (input, part, probe, opt);
    s->output = out;
    s->progress = TRUE;
    GPtrArray *steps = g_ptr_array_new_with_free_func ((GDestroyNotify) vc_step_free);
    g_ptr_array_add (steps, s);
    return steps;
}
