#include "modes.h"

#include <glib/gstdio.h>

static const char PROBE_1080[] =
    "index=0\ncodec_name=h264\ncodec_type=video\nwidth=1920\nheight=1080\npix_fmt=yuv420p\n"
    "color_space=unknown\ncolor_transfer=unknown\ncolor_primaries=unknown\n"
    "index=1\ncodec_name=aac\ncodec_type=audio\nduration=20.000000\n";
static const char PROBE_HLG[] =
    "index=0\ncodec_name=prores\ncodec_type=video\nwidth=1280\nheight=720\n"
    "pix_fmt=yuv422p10le\ncolor_space=bt2020nc\ncolor_transfer=arib-std-b67\n"
    "color_primaries=bt2020\nduration=10.000000\n";
static const char PROBE_PQ[] =
    "index=0\ncodec_name=prores\ncodec_type=video\nwidth=1280\nheight=720\n"
    "pix_fmt=yuv422p10le\ncolor_space=bt2020nc\ncolor_transfer=smpte2084\n"
    "color_primaries=bt2020\nduration=10.000000\n";
static const char PROBE_PQ_UNTAGGED[] =   /* HDR transfer, but no primaries or matrix */
    "index=0\ncodec_name=hevc\ncodec_type=video\nwidth=1280\nheight=720\n"
    "pix_fmt=yuv420p10le\ncolor_space=unknown\ncolor_transfer=smpte2084\n"
    "color_primaries=unknown\nduration=10.000000\n";
static const char PROBE_10BIT_SDR[] =
    "index=0\ncodec_name=hevc\ncodec_type=video\nwidth=1920\nheight=1080\n"
    "pix_fmt=yuv420p10le\ncolor_space=bt709\ncolor_transfer=bt709\n"
    "color_primaries=bt709\nduration=10.000000\n";

static const char TONEMAP_TAIL[] =
    ":t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=hable:desat=0,"
    "zscale=t=bt709:m=bt709:r=tv,format=yuv420p";

/* The single step for `input`; the test owns the dir it lives in. */
static VcStep *
one_step (GPtrArray **steps, const char *input, const char *probe_text, VcOptions opt)
{
    g_autoptr (VcProbe) p = vc_parse_probe (probe_text, NULL);
    g_assert_nonnull (p);
    g_autofree char *reason = NULL;
    *steps = vc_mode_steps (VC_MODE_WEB_VIDEO, input, p, &opt, &reason);
    g_assert_null (reason);
    g_assert_nonnull (*steps);
    g_assert_cmpuint ((*steps)->len, ==, 1);
    return (*steps)->pdata[0];
}

static void
assert_argv (char **argv, const char *const *want)
{
    for (guint i = 0; want[i] != NULL; i++) {
        g_assert_nonnull (argv[i]);
        g_assert_cmpstr (argv[i], ==, want[i]);
    }
    g_assert_cmpuint (g_strv_length (argv), ==, g_strv_length ((char **) want));
}

static void
test_argv_widths (void)
{
    struct { int width; const char *scale; } cases[] = {
        { 1920, "scale='trunc(min(iw,1920)/2)*2':-2,format=yuv420p" },
        { 1280, "scale='trunc(min(iw,1280)/2)*2':-2,format=yuv420p" },
        { 0, "scale='trunc(iw/2)*2':-2,format=yuv420p" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/clip.MOV", PROBE_1080, (VcOptions) { .max_width = cases[i].width });
        const char *want[] = {
            "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
            "-y", "-i", "/v/clip.MOV", "-map", "0:v:0", "-map", "0:a:0?", "-vf", cases[i].scale,
            "-c:v", "libx264", "-profile:v", "high", "-preset", "slow", "-crf", "23", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-b:a", "128k", "-ac", "2",
            "-sn", "-dn", "-map_metadata", "-1", "-map_chapters", "-1", "-movflags", "+faststart",
            "/v/web/.clip.part.mp4", NULL
        };
        assert_argv (s->argv, want);
        g_assert_cmpstr (s->part, ==, "/v/web/.clip.part.mp4");
        g_assert_cmpstr (s->output, ==, "/v/web/clip.mp4");   /* lowercase extension */
        g_assert_true (s->progress);
        g_assert_cmpint (s->soft_exit, ==, 0);
    }
}

static void
test_remove_audio (void)
{
    g_autoptr (GPtrArray) steps = NULL;
    VcStep *s = one_step (&steps, "/v/clip.mp4", PROBE_1080, (VcOptions) { .max_width = 1920, .no_audio = TRUE });
    g_assert_true (g_strv_contains ((const char *const *) s->argv, "-an"));
    g_assert_false (g_strv_contains ((const char *const *) s->argv, "0:a:0?"));
    g_assert_false (g_strv_contains ((const char *const *) s->argv, "aac"));
    g_assert_false (g_strv_contains ((const char *const *) s->argv, "-b:a"));
}

static const char *
vf_of (VcStep *s)
{
    for (guint i = 0; s->argv[i] != NULL; i++)
        if (g_str_equal (s->argv[i], "-vf"))
            return s->argv[i + 1];
    g_assert_not_reached ();
}

static void
test_tonemap (void)
{
    struct { const char *probe; const char *head; } hdr[] = {
        { PROBE_HLG, "scale='trunc(min(iw,1920)/2)*2':-2,zscale=tin=arib-std-b67:pin=bt2020:min=bt2020nc" },
        { PROBE_PQ, "scale='trunc(min(iw,1920)/2)*2':-2,zscale=tin=smpte2084:pin=bt2020:min=bt2020nc" },
        /* missing primaries/matrix fall back to BT.2020 */
        { PROBE_PQ_UNTAGGED, "scale='trunc(min(iw,1920)/2)*2':-2,zscale=tin=smpte2084:pin=bt2020:min=bt2020nc" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (hdr); i++) {
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/hdr.mov", hdr[i].probe, (VcOptions) { .max_width = 1920 });
        g_autofree char *want = g_strconcat (hdr[i].head, TONEMAP_TAIL, NULL);
        g_assert_cmpstr (vf_of (s), ==, want);
    }
    /* 10-bit SDR and 8-bit: just converted to 8-bit yuv420p */
    const char *sdr[] = { PROBE_10BIT_SDR, PROBE_1080 };
    for (gsize i = 0; i < G_N_ELEMENTS (sdr); i++) {
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/sdr.mov", sdr[i], (VcOptions) { .max_width = 1920 });
        g_assert_cmpstr (vf_of (s), ==, "scale='trunc(min(iw,1920)/2)*2':-2,format=yuv420p");
    }
}

static void
test_paths_and_skip (void)
{
    g_autofree char *dir = g_dir_make_tmp ("vc-web-XXXXXX", NULL);
    const char *odd = "it's a \"test\" $HOME ünïcødé 🎬.MOV";
    g_autofree char *input = g_build_filename (dir, odd, NULL);
    g_autofree char *wdir = g_build_filename (dir, "web", NULL);
    g_autofree char *out = g_build_filename (wdir, "it's a \"test\" $HOME ünïcødé 🎬.mp4", NULL);
    g_autofree char *part = g_build_filename (wdir, ".it's a \"test\" $HOME ünïcødé 🎬.part.mp4", NULL);
    {
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, input, PROBE_1080, (VcOptions) { 0 });
        g_assert_cmpstr (s->output, ==, out);
        g_assert_cmpstr (s->part, ==, part);
        g_assert_cmpstr (s->argv[10], ==, input);   /* one argv element each, no quoting */
        g_assert_cmpstr (s->argv[g_strv_length (s->argv) - 1], ==, part);
    }

    g_assert_cmpint (g_mkdir (wdir, 0755), ==, 0);
    g_assert_true (g_file_set_contents (out, "x", 1, NULL));
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_1080, NULL);
    g_autofree char *reason = NULL;
    VcOptions o = { 0 };
    g_assert_null (vc_mode_steps (VC_MODE_WEB_VIDEO, input, p, &o, &reason));
    g_assert_cmpstr (reason, ==, "web/it's a \"test\" $HOME ünïcødé 🎬.mp4 exists");

    g_unlink (out);
    g_rmdir (wdir);
    g_assert_cmpint (g_rmdir (dir), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/web/argv-widths", test_argv_widths);
    g_test_add_func ("/web/remove-audio", test_remove_audio);
    g_test_add_func ("/web/tonemap", test_tonemap);
    g_test_add_func ("/web/paths-and-skip", test_paths_and_skip);
    return g_test_run ();
}
