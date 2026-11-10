#include "modes.h"

#include <math.h>
#include <string.h>

static gboolean
all_digits (const char *s, gsize n)
{
    if (n == 0)
        return FALSE;
    for (gsize i = 0; i < n; i++)
        if (!g_ascii_isdigit (s[i]))
            return FALSE;
    return TRUE;
}

gboolean
vc_parse_time (const char *text, double *seconds)
{
    g_autofree char *t = g_strstrip (g_strdup (text));
    *seconds = 0;
    if (*t == '\0')
        return TRUE;
    g_auto (GStrv) parts = g_strsplit (t, ":", 0);
    guint n = g_strv_length (parts);
    if (n > 3)
        return FALSE;
    double total = 0;
    for (guint i = 0; i < n; i++) {
        const char *p = parts[i];
        const char *dot = i == n - 1 ? strchr (p, '.') : NULL;   /* only seconds have a fraction */
        if (!all_digits (p, dot != NULL ? (gsize) (dot - p) : strlen (p))
            || (dot != NULL && !all_digits (dot + 1, strlen (dot + 1))))
            return FALSE;
        double v = g_ascii_strtod (p, NULL);
        if (i > 0 && v >= 60)   /* minutes and seconds after a colon */
            return FALSE;
        total = total * 60 + v;
    }
    *seconds = total;
    return TRUE;
}

int
vc_frames_jpeg_qscale (int quality)
{
    quality = CLAMP (quality, 1, 100);
    return (int) lround (2 + (100 - quality) * 29 / 99.0);
}

/* The range's length in seconds: End past the end runs to the end. */
static double
range_length (double duration, const VcOptions *opt)
{
    double end = opt->end > 0 && opt->end < duration ? opt->end : duration;
    return end - opt->start;
}

guint
vc_frames_estimate (double duration, double source_fps, const VcOptions *opt)
{
    double rate = opt->fps > 0 ? opt->fps : source_fps;
    double len = range_length (duration, opt);
    if (duration <= 0 || rate <= 0 || len <= 0)
        return 0;
    return (guint) lround (len * rate);
}

char *
vc_frames_output (const char *input, char **part_path)
{
    g_autofree char *dir = g_path_get_dirname (input);
    g_autofree char *base = g_path_get_basename (input);
    char *dot = strrchr (base, '.');
    if (dot != NULL && dot != base)
        *dot = '\0';
    if (part_path != NULL) {
        g_autofree char *part_name = g_strdup_printf (".%s.part", base);
        *part_path = g_build_filename (dir, "frames", part_name, NULL);
    }
    return g_build_filename (dir, "frames", base, NULL);
}

static void
add_time (GStrvBuilder *b, const char *flag, double seconds)
{
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_strv_builder_add_many (b, flag, g_ascii_formatd (num, sizeof num, "%.3f", seconds), NULL);
}

static char **
build_argv (const char *input, const char *part, const VcProbe *probe, const VcOptions *opt)
{
    GStrvBuilder *b = g_strv_builder_new ();
    g_strv_builder_add_many (b, "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error",
                             "-nostats", "-progress", "pipe:1", "-y", NULL);
    /* Before -i: a fast seek, still frame-accurate because every frame is re-encoded. */
    if (opt->start > 0)
        add_time (b, "-ss", opt->start);
    if (opt->end > 0)
        add_time (b, "-to", opt->end);
    g_strv_builder_add_many (b, "-i", input, "-an", "-sn", "-dn", NULL);

    GString *vf = g_string_new (NULL);
    if (opt->fps > 0)
        g_string_append_printf (vf, "fps=%g,", opt->fps);
    if (opt->max_width > 0)   /* never upscale */
        g_string_append_printf (vf, "scale='min(iw,%d)':-2,", opt->max_width);
    g_autofree char *tonemap = vc_tonemap_chain (probe);
    if (tonemap != NULL)
        g_string_append_printf (vf, "%s,", tonemap);
    if (vf->len > 0) {
        g_string_truncate (vf, vf->len - 1);
        g_strv_builder_add_many (b, "-vf", vf->str, NULL);
    }
    g_string_free (vf, TRUE);

    char q[16];
    const char *ext;
    if (opt->webp) {
        g_snprintf (q, sizeof q, "%d", CLAMP (opt->quality, 1, 100));
        g_strv_builder_add_many (b, "-c:v", "libwebp", "-quality", q, NULL);
        ext = "webp";
    } else {
        g_snprintf (q, sizeof q, "%d", vc_frames_jpeg_qscale (opt->quality));
        g_strv_builder_add_many (b, "-c:v", "mjpeg", "-q:v", q, NULL);
        ext = "jpg";
    }
    g_autofree char *pattern_name = g_strdup_printf ("frame_%%04d.%s", ext);
    g_autofree char *pattern = g_build_filename (part, pattern_name, NULL);
    g_strv_builder_add (b, pattern);
    char **argv = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    return argv;
}

GPtrArray *
vc_frames_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                 char **skip_reason)
{
    *skip_reason = NULL;
    if (probe->duration > 0 && opt->start >= probe->duration) {
        *skip_reason = g_strdup ("starts after the end");
        return NULL;
    }
    char *part = NULL;
    char *out = vc_frames_output (input, &part);
    if (g_file_test (out, G_FILE_TEST_EXISTS)) {
        g_autofree char *base = g_path_get_basename (out);
        g_autofree char *display = g_filename_display_name (base);
        *skip_reason = g_strdup_printf ("frames/%s/ exists", display);
        g_free (out);
        g_free (part);
        return NULL;
    }
    VcStep *s = g_new0 (VcStep, 1);
    s->argv = build_argv (input, part, probe, opt);
    s->part = part;
    s->output = out;
    s->progress = TRUE;
    s->part_is_dir = TRUE;
    /* -progress counts output time, which starts at 0 at the range's start */
    s->duration = probe->duration > 0 ? range_length (probe->duration, opt)
                  : opt->end > opt->start ? opt->end - opt->start : 0;
    GPtrArray *steps = g_ptr_array_new_with_free_func ((GDestroyNotify) vc_step_free);
    g_ptr_array_add (steps, s);
    return steps;
}
