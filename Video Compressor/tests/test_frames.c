#include "modes.h"

#include <glib/gstdio.h>

static const char PROBE_1080[] =
    "index=0\ncodec_name=h264\ncodec_type=video\nwidth=1920\nheight=1080\npix_fmt=yuv420p\n"
    "avg_frame_rate=30/1\ncolor_space=unknown\ncolor_transfer=unknown\ncolor_primaries=unknown\n"
    "index=1\ncodec_name=aac\ncodec_type=audio\nduration=60.000000\n";
static const char PROBE_HLG[] =
    "index=0\ncodec_name=prores\ncodec_type=video\nwidth=1280\nheight=720\n"
    "pix_fmt=yuv422p10le\ncolor_space=bt2020nc\ncolor_transfer=arib-std-b67\n"
    "color_primaries=bt2020\nduration=10.000000\n";

static void
test_parse_time (void)
{
    struct { const char *text; double seconds; } ok[] = {
        { "", 0 }, { "  ", 0 }, { "12", 12 }, { "12.5", 12.5 }, { "1:05", 65 }, { "1:05.25", 65.25 },
        { "0:01:05", 65 }, { "1:00:00", 3600 }, { " 10 ", 10 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (ok); i++) {
        double s = -1;
        g_assert_true (vc_parse_time (ok[i].text, &s));
        g_assert_cmpfloat_with_epsilon (s, ok[i].seconds, 1e-9);
    }
    const char *bad[] = { "-1", "1:75", "abc", "1::2", "1:2:3:4", "12.", ".5", "1.5:00", "1:60",
                          "1e3", "1,5", ":5" };
    for (gsize i = 0; i < G_N_ELEMENTS (bad); i++) {
        double s;
        g_assert_false (vc_parse_time (bad[i], &s));
    }
}

static void
test_jpeg_qscale (void)
{
    g_assert_cmpint (vc_frames_jpeg_qscale (1), ==, 31);
    g_assert_cmpint (vc_frames_jpeg_qscale (80), ==, 8);
    g_assert_cmpint (vc_frames_jpeg_qscale (100), ==, 2);
    for (int q = 2; q <= 100; q++)   /* higher quality never gives a bigger (worse) qscale */
        g_assert_cmpint (vc_frames_jpeg_qscale (q), <=, vc_frames_jpeg_qscale (q - 1));
}

static void
test_estimate (void)
{
    g_assert_cmpuint (vc_frames_estimate (60, 30, &(VcOptions) { .fps = 15 }), ==, 900);
    g_assert_cmpuint (vc_frames_estimate (60, 30, &(VcOptions) { .fps = 12, .start = 10, .end = 12 }),
                      ==, 24);
    /* End past the end runs to the end */
    g_assert_cmpuint (vc_frames_estimate (60, 30, &(VcOptions) { .fps = 10, .start = 50, .end = 90 }),
                      ==, 100);
    /* Start past (or at) the end */
    g_assert_cmpuint (vc_frames_estimate (60, 30, &(VcOptions) { .fps = 10, .start = 60 }), ==, 0);
    g_assert_cmpuint (vc_frames_estimate (60, 30, &(VcOptions) { .fps = 10, .start = 75 }), ==, 0);
    /* Every frame uses the source rate */
    g_assert_cmpuint (vc_frames_estimate (60, 30000.0 / 1001, &(VcOptions) { 0 }), ==, 1798);
    /* unknown duration or rate */
    g_assert_cmpuint (vc_frames_estimate (0, 30, &(VcOptions) { .fps = 15 }), ==, 0);
    g_assert_cmpuint (vc_frames_estimate (60, 0, &(VcOptions) { 0 }), ==, 0);
}

static VcStep *
one_step (GPtrArray **steps, const char *input, const char *probe_text, VcOptions opt)
{
    g_autoptr (VcProbe) p = vc_parse_probe (probe_text, NULL);
    g_assert_nonnull (p);
    g_autofree char *reason = NULL;
    *steps = vc_mode_steps (VC_MODE_FRAMES, input, p, &opt, &reason);
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
test_argv (void)
{
    {   /* WebP, whole video */
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/clip.MOV", PROBE_1080,
                              (VcOptions) { .fps = 15, .max_width = 1920, .webp = TRUE, .quality = 80 });
        const char *want[] = {
            "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
            "-y", "-i", "/v/clip.MOV", "-an", "-sn", "-dn", "-vf", "fps=15,scale='min(iw,1920)':-2",
            "-c:v", "libwebp", "-quality", "80", "/v/frames/.clip.part/frame_%04d.webp", NULL
        };
        assert_argv (s->argv, want);
        g_assert_cmpstr (s->part, ==, "/v/frames/.clip.part");
        g_assert_cmpstr (s->output, ==, "/v/frames/clip");
        g_assert_true (s->part_is_dir);
        g_assert_true (s->progress);
        g_assert_cmpfloat (s->duration, ==, 60);
    }
    {   /* JPG with a range: trim before -i, progress over the range */
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/clip.mp4", PROBE_1080,
                              (VcOptions) { .fps = 12, .start = 10, .end = 12, .max_width = 640,
                                            .quality = 80 });
        const char *want[] = {
            "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats", "-progress", "pipe:1",
            "-y", "-ss", "10.000", "-to", "12.000", "-i", "/v/clip.mp4", "-an", "-sn", "-dn",
            "-vf", "fps=12,scale='min(iw,640)':-2", "-c:v", "mjpeg", "-q:v", "8",
            "/v/frames/.clip.part/frame_%04d.jpg", NULL
        };
        assert_argv (s->argv, want);
        g_assert_cmpfloat (s->duration, ==, 2);
    }
    {   /* Start only; End past the end counts to the video's end */
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/clip.mp4", PROBE_1080,
                              (VcOptions) { .fps = 12, .start = 50.5, .end = 99, .webp = TRUE, .quality = 80 });
        g_assert_cmpstr (s->argv[9], ==, "-ss");
        g_assert_cmpstr (s->argv[10], ==, "50.500");
        g_assert_cmpfloat (s->duration, ==, 9.5);
    }
    {   /* Every frame and Original width: no filter at all */
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/clip.mp4", PROBE_1080, (VcOptions) { .webp = TRUE, .quality = 80 });
        g_assert_false (g_strv_contains ((const char *const *) s->argv, "-vf"));
    }
    {   /* HDR: the web video tone-map chain after the scale */
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, "/v/hdr.mov", PROBE_HLG,
                              (VcOptions) { .max_width = 1920, .webp = TRUE, .quality = 80 });
        g_assert_cmpstr (s->argv[14], ==, "-vf");
        g_assert_cmpstr (s->argv[15], ==,
                         "scale='min(iw,1920)':-2,zscale=tin=arib-std-b67:pin=bt2020:min=bt2020nc:t=linear:"
                         "npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=hable:desat=0,"
                         "zscale=t=bt709:m=bt709:r=tv");
    }
}

static void
test_paths_and_skip (void)
{
    g_autofree char *dir = g_dir_make_tmp ("vc-frames-XXXXXX", NULL);
    const char *odd = "it's a \"test\" $HOME ünïcødé 🎬.mov";
    const char *stem = "it's a \"test\" $HOME ünïcødé 🎬";
    g_autofree char *input = g_build_filename (dir, odd, NULL);
    g_autofree char *fdir = g_build_filename (dir, "frames", NULL);
    g_autofree char *out = g_build_filename (fdir, stem, NULL);
    g_autofree char *part_name = g_strdup_printf (".%s.part", stem);
    g_autofree char *part = g_build_filename (fdir, part_name, NULL);
    g_autofree char *pattern = g_build_filename (part, "frame_%04d.webp", NULL);
    VcOptions o = { .fps = 15, .webp = TRUE, .quality = 80 };
    {
        g_autoptr (GPtrArray) steps = NULL;
        VcStep *s = one_step (&steps, input, PROBE_1080, o);
        g_assert_cmpstr (s->output, ==, out);
        g_assert_cmpstr (s->part, ==, part);
        g_assert_cmpstr (s->argv[10], ==, input);   /* one argv element each, no quoting */
        g_assert_cmpstr (s->argv[g_strv_length (s->argv) - 1], ==, pattern);
    }

    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_1080, NULL);
    g_autofree char *reason = NULL;
    VcOptions late = o;
    late.start = 60;
    g_assert_null (vc_mode_steps (VC_MODE_FRAMES, input, p, &late, &reason));
    g_assert_cmpstr (reason, ==, "starts after the end");
    g_clear_pointer (&reason, g_free);

    g_assert_cmpint (g_mkdir_with_parents (out, 0755), ==, 0);
    g_assert_null (vc_mode_steps (VC_MODE_FRAMES, input, p, &o, &reason));
    g_assert_cmpstr (reason, ==, "frames/it's a \"test\" $HOME ünïcødé 🎬/ exists");

    /* the part directory is one vc_remove_part accepts */
    g_assert_cmpint (g_mkdir (part, 0755), ==, 0);
    g_autofree char *frame = g_build_filename (part, "frame_0001.webp", NULL);
    g_assert_true (g_file_set_contents (frame, "x", 1, NULL));
    g_assert_true (vc_remove_part (part));
    g_assert_false (g_file_test (part, G_FILE_TEST_EXISTS));

    g_rmdir (out);
    g_rmdir (fdir);
    g_assert_cmpint (g_rmdir (dir), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/frames/parse-time", test_parse_time);
    g_test_add_func ("/frames/jpeg-qscale", test_jpeg_qscale);
    g_test_add_func ("/frames/estimate", test_estimate);
    g_test_add_func ("/frames/argv", test_argv);
    g_test_add_func ("/frames/paths-and-skip", test_paths_and_skip);
    return g_test_run ();
}
