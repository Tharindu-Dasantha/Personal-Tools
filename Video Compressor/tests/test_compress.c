#include "compress.h"

#include <string.h>

/* Returns the index of `flag` in argv, or -1. */
static int
argv_find (char **argv, const char *flag)
{
    for (int i = 0; argv[i] != NULL; i++)
        if (strcmp (argv[i], flag) == 0)
            return i;
    return -1;
}

/* The value after `flag`, or NULL when the flag is absent. */
static const char *
argv_value (char **argv, const char *flag)
{
    int i = argv_find (argv, flag);
    return i < 0 ? NULL : argv[i + 1];
}

static void
test_class_boundaries (void)
{
    struct { int w, h; const char *label; int crf, akbps; } cases[] = {
        {  426,  240, "240p",  24,  64 }, {  640,  360, "360p",  24,  96 },
        {  854,  480, "480p",  25,  96 }, { 1280,  720, "720p",  26, 128 },
        { 1920, 1080, "1080p", 27, 128 }, { 1080, 1920, "1080p", 27, 128 },
        { 3840, 2160, "4K",    28, 160 },
        /* edges of the table */
        {  400,  300, "240p",  24,  64 }, {  400,  301, "360p",  24,  96 },
        { 2000, 1200, "1080p", 27, 128 }, { 2000, 1201, "4K",    28, 160 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        const VcClass *c = vc_class_for (cases[i].w, cases[i].h);
        g_assert_cmpstr (c->label, ==, cases[i].label);
        g_assert_cmpint (c->crf, ==, cases[i].crf);
        g_assert_cmpint (c->audio_kbps, ==, cases[i].akbps);
    }
}

static void
test_video_names (void)
{
    const char *yes[] = { "a.mp4", "B.MP4", "c.mkv", "d.MoV", "e.avi", "f.webm", "g.m2ts",
                          "h.3gp", "it's a \"test\" $HOME ünï.mp4", "x.y.mpeg" };
    const char *no[] = { "notes.txt", "pic.jpg", ".clip.part.mp4", ".hidden.mkv",
                         "compressed", "mp4", "noext", "a.mp4.txt" };
    for (gsize i = 0; i < G_N_ELEMENTS (yes); i++)
        g_assert_true (vc_is_video_name (yes[i]));
    for (gsize i = 0; i < G_N_ELEMENTS (no); i++)
        g_assert_false (vc_is_video_name (no[i]));
}

static void
test_ffprobe_argv (void)
{
    g_auto (GStrv) argv = vc_build_ffprobe_argv ("/v/a b.mp4");
    g_assert_cmpstr (argv[0], ==, "ffprobe");
    g_assert_cmpstr (argv_value (argv, "-of"), ==, "default=nw=1");
    g_assert_cmpstr (argv_value (argv, "-i"), ==, "/v/a b.mp4");
    g_assert_true (g_str_has_prefix (argv_value (argv, "-show_entries"), "stream=index,"));
}

/* Captured from ffprobe 8.1 on the subs-movtext.mp4 fixture. */
static const char PROBE_8BIT_SUBS[] =
    "index=0\ncodec_name=h264\ncodec_type=video\nwidth=640\nheight=360\npix_fmt=yuv420p\n"
    "color_space=unknown\ncolor_transfer=unknown\ncolor_primaries=unknown\n"
    "index=1\ncodec_name=aac\ncodec_type=audio\n"
    "index=2\ncodec_name=mov_text\ncodec_type=subtitle\nwidth=N/A\nheight=N/A\n"
    "duration=10.000000\n";

/* Captured from the hlg-10bit.mov fixture. */
static const char PROBE_HLG[] =
    "index=0\ncodec_name=prores\ncodec_type=video\nwidth=1280\nheight=720\n"
    "pix_fmt=yuv422p10le\ncolor_space=bt2020nc\ncolor_transfer=arib-std-b67\n"
    "color_primaries=bt2020\nindex=1\ncodec_name=aac\ncodec_type=audio\n"
    "duration=10.000000\n";

static void
test_parse_probe_good (void)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_8BIT_SUBS, &error);
    g_assert_no_error (error);
    g_assert_cmpint (p->width, ==, 640);
    g_assert_cmpint (p->height, ==, 360);
    g_assert_cmpstr (p->codec, ==, "h264");
    g_assert_cmpstr (p->pix_fmt, ==, "yuv420p");
    g_assert_null (p->color_primaries);
    g_assert_null (p->color_transfer);
    g_assert_null (p->color_space);
    g_assert_cmpfloat (p->duration, ==, 10.0);
    g_assert_cmpuint (p->n_subtitles, ==, 1);
    g_assert_true (p->has_mov_text);
    g_assert_false (vc_probe_is_10bit (p));
    g_assert_false (vc_probe_is_hdr (p));

    g_autoptr (VcProbe) h = vc_parse_probe (PROBE_HLG, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (h->color_transfer, ==, "arib-std-b67");
    g_assert_cmpuint (h->n_subtitles, ==, 0);
    g_assert_false (h->has_mov_text);
    g_assert_true (vc_probe_is_10bit (h));
    g_assert_true (vc_probe_is_hdr (h));
}

static void
test_parse_probe_missing_duration (void)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (VcProbe) p = vc_parse_probe (
        "index=0\ncodec_name=mpeg4\ncodec_type=video\nwidth=320\nheight=240\n"
        "duration=N/A\n", &error);
    g_assert_no_error (error);
    g_assert_cmpfloat (p->duration, ==, 0.0);
    g_assert_null (p->pix_fmt);
    g_assert_false (vc_probe_is_10bit (p));
}

static void
test_parse_probe_bad (void)
{
    const char *bad[] = {
        "",
        "garbage\n\x01\x02 no equals here",
        "moov atom not found\n",
        /* audio only */
        "index=0\ncodec_name=mp3\ncodec_type=audio\nduration=3.0\n",
        /* video with garbage / missing / zero size */
        "index=0\ncodec_name=h264\ncodec_type=video\nwidth=abc\nheight=360\n",
        "index=0\ncodec_name=h264\ncodec_type=video\nheight=360\n",
        "index=0\ncodec_name=h264\ncodec_type=video\nwidth=0\nheight=360\n",
        "index=0\ncodec_name=h264\ncodec_type=video\nwidth=640x\nheight=360\n",
        /* no codec */
        "index=0\ncodec_type=video\nwidth=640\nheight=360\n",
        /* fields with no stream marker */
        "codec_name=h264\ncodec_type=video\nwidth=640\nheight=360\n",
    };
    for (gsize i = 0; i < G_N_ELEMENTS (bad); i++) {
        g_autoptr (GError) error = NULL;
        VcProbe *p = vc_parse_probe (bad[i], &error);
        g_assert_null (p);
        g_assert_error (error, VC_ERROR, 0);
    }
}

static void
test_parse_probe_first_video_wins (void)
{
    g_autoptr (VcProbe) p = vc_parse_probe (
        "index=0\ncodec_name=h264\ncodec_type=video\nwidth=1920\nheight=1080\n"
        "index=1\ncodec_name=mjpeg\ncodec_type=video\nwidth=300\nheight=300\n", NULL);
    g_assert_nonnull (p);
    g_assert_cmpint (p->width, ==, 1920);
    g_assert_cmpstr (p->codec, ==, "h264");
}

static void
test_output_path (void)
{
    g_autofree char *part = NULL;
    g_autofree char *out = vc_output_path ("/v/dir/clip.MOV", FALSE, &part);
    g_assert_cmpstr (out, ==, "/v/dir/compressed/clip.mp4");
    g_assert_cmpstr (part, ==, "/v/dir/compressed/.clip.part.mp4");

    g_autofree char *part2 = NULL;
    g_autofree char *out2 = vc_output_path ("/v/my.holiday clip.mkv", TRUE, &part2);
    g_assert_cmpstr (out2, ==, "/v/compressed/my.holiday clip.mkv");
    g_assert_cmpstr (part2, ==, "/v/compressed/.my.holiday clip.part.mkv");

    g_autofree char *out3 = vc_output_path ("/v/x.avi", FALSE, NULL);
    g_assert_cmpstr (out3, ==, "/v/compressed/x.mp4");
}

static void
test_ffmpeg_argv_8bit (void)
{
    const char *name = "/v/it's a \"test\" $HOME ünïcødé 🎬.mov";
    const char *out = "/v/compressed/.it's a \"test\" $HOME ünïcødé 🎬.part.mp4";
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) argv = vc_build_ffmpeg_argv (name, out, p, "slow", FALSE);

    const char *expected[] = {
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats",
        "-progress", "pipe:1", "-y", "-i", name,
        "-map", "0:v:0", "-map", "0:a?",
        "-c:v", "libx265", "-preset", "slow", "-crf", "24", "-x265-params", "log-level=error",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "96k",
        "-sn", "-tag:v", "hvc1", "-movflags", "+faststart",
        out, NULL,
    };
    g_assert_cmpstrv (argv, expected);
}

static void
test_ffmpeg_argv_10bit_hlg (void)
{
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_HLG, NULL);
    g_auto (GStrv) argv = vc_build_ffmpeg_argv ("/v/a.mov", "/v/compressed/.a.part.mp4", p,
                                                "slow", FALSE);
    g_assert_cmpstr (argv_value (argv, "-pix_fmt"), ==, "yuv420p10le");
    g_assert_cmpstr (argv_value (argv, "-profile:v"), ==, "main10");
    g_assert_cmpstr (argv_value (argv, "-color_primaries"), ==, "bt2020");
    g_assert_cmpstr (argv_value (argv, "-color_trc"), ==, "arib-std-b67");
    g_assert_cmpstr (argv_value (argv, "-colorspace"), ==, "bt2020nc");
    g_assert_cmpstr (argv_value (argv, "-crf"), ==, "26");
    g_assert_cmpstr (argv_value (argv, "-b:a"), ==, "128k");

    /* 10-bit without colour tags: no tag flags at all */
    g_autoptr (VcProbe) q = vc_parse_probe (
        "index=0\ncodec_name=hevc\ncodec_type=video\nwidth=1920\nheight=1080\n"
        "pix_fmt=yuv420p10le\ncolor_space=unknown\ncolor_transfer=unknown\n"
        "color_primaries=bt709\n", NULL);
    g_auto (GStrv) argv2 = vc_build_ffmpeg_argv ("/v/b.mkv", "/v/compressed/.b.part.mp4", q,
                                                 "slow", FALSE);
    g_assert_cmpstr (argv_value (argv2, "-profile:v"), ==, "main10");
    g_assert_cmpstr (argv_value (argv2, "-color_primaries"), ==, "bt709");
    g_assert_cmpint (argv_find (argv2, "-color_trc"), ==, -1);
    g_assert_cmpint (argv_find (argv2, "-colorspace"), ==, -1);

    /* 8-bit never gets colour flags or main10 */
    g_autoptr (VcProbe) e = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) argv3 = vc_build_ffmpeg_argv ("/v/c.mp4", "/v/o.mp4", e, "slow", FALSE);
    g_assert_cmpint (argv_find (argv3, "-profile:v"), ==, -1);
    g_assert_cmpint (argv_find (argv3, "-color_primaries"), ==, -1);
}

static void
test_ffmpeg_argv_presets (void)
{
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_HLG, NULL);
    g_assert_cmpstr (VC_DEFAULT_PRESET, ==, "slow");
    for (int i = 0; VC_PRESETS[i] != NULL; i++) {
        g_auto (GStrv) argv = vc_build_ffmpeg_argv ("/a.mp4", "/b.mp4", p, VC_PRESETS[i], FALSE);
        g_assert_cmpstr (argv_value (argv, "-preset"), ==, VC_PRESETS[i]);
    }
}

static void
test_ffmpeg_argv_subtitles (void)
{
    /* mov_text source → srt */
    g_autoptr (VcProbe) mt = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) a = vc_build_ffmpeg_argv ("/v/a.mp4", "/v/compressed/.a.part.mkv", mt,
                                             "slow", TRUE);
    int s = argv_find (a, "0:s?");
    g_assert_cmpint (s, >, 0);
    g_assert_cmpstr (a[s - 1], ==, "-map");
    g_assert_cmpstr (argv_value (a, "-c:s"), ==, "srt");
    g_assert_cmpint (argv_find (a, "-sn"), ==, -1);
    g_assert_cmpint (argv_find (a, "hvc1"), ==, -1);
    g_assert_cmpint (argv_find (a, "+faststart"), ==, -1);

    /* other subtitle codecs (or none) → copy */
    g_autoptr (VcProbe) srt = vc_parse_probe (
        "index=0\ncodec_name=h264\ncodec_type=video\nwidth=640\nheight=360\n"
        "index=1\ncodec_name=subrip\ncodec_type=subtitle\n", NULL);
    g_assert_cmpuint (srt->n_subtitles, ==, 1);
    g_assert_false (srt->has_mov_text);
    g_auto (GStrv) b = vc_build_ffmpeg_argv ("/v/a.mkv", "/v/o.mkv", srt, "slow", TRUE);
    g_assert_cmpstr (argv_value (b, "-c:s"), ==, "copy");

    /* subtitles off */
    g_auto (GStrv) c = vc_build_ffmpeg_argv ("/v/a.mp4", "/v/o.mp4", mt, "slow", FALSE);
    g_assert_cmpint (argv_find (c, "0:s?"), ==, -1);
    g_assert_cmpint (argv_find (c, "-c:s"), ==, -1);
    g_assert_cmpint (argv_find (c, "-sn"), >, 0);
    g_assert_cmpstr (argv_value (c, "-tag:v"), ==, "hvc1");
}

static void
test_progress_line (void)
{
    double f = -1;
    g_assert_true (vc_parse_progress_line ("out_time_us=30000000", 60.0, &f));
    g_assert_cmpfloat_with_epsilon (f, 0.5, 1e-9);
    g_assert_true (vc_parse_progress_line ("out_time_us=-2000", 60.0, &f));
    g_assert_cmpfloat (f, ==, 0.0);
    g_assert_true (vc_parse_progress_line ("out_time_us=90000000", 60.0, &f));
    g_assert_cmpfloat (f, ==, 1.0);
    g_assert_true (vc_parse_progress_line ("progress=end", 60.0, &f));
    g_assert_cmpfloat (f, ==, 1.0);

    f = -1;
    g_assert_false (vc_parse_progress_line ("out_time_us=N/A", 60.0, &f));
    g_assert_false (vc_parse_progress_line ("out_time_us=30000000", 0.0, &f));
    g_assert_false (vc_parse_progress_line ("progress=continue", 60.0, &f));
    g_assert_false (vc_parse_progress_line ("frame=12", 60.0, &f));
    g_assert_false (vc_parse_progress_line ("", 60.0, &f));
    g_assert_false (vc_parse_progress_line ("out_time_us=", 60.0, &f));
    g_assert_cmpfloat (f, ==, -1);
}

static void
test_worth_keeping (void)
{
    g_assert_true (vc_output_is_worth_keeping (1000, 940));
    g_assert_true (vc_output_is_worth_keeping (1000, 950));
    g_assert_false (vc_output_is_worth_keeping (1000, 960));
    g_assert_false (vc_output_is_worth_keeping (1000, 1200));
    g_assert_false (vc_output_is_worth_keeping (1000, 0));
    g_assert_true (vc_output_is_worth_keeping (G_GUINT64_CONSTANT (50000000000),
                                               G_GUINT64_CONSTANT (10000000000)));
}

static void
test_should_trash (void)
{
    g_assert_true (vc_should_trash_original (TRUE, TRUE, 60.0, 60.0, TRUE));
    /* each condition failing alone */
    g_assert_false (vc_should_trash_original (FALSE, TRUE, 60.0, 60.0, TRUE));
    g_assert_false (vc_should_trash_original (TRUE, FALSE, 60.0, 60.0, TRUE));
    g_assert_false (vc_should_trash_original (TRUE, TRUE, 60.0, 60.0, FALSE));
    /* durations: 0.9 s off is fine, 1.1 s is not, either direction */
    g_assert_true (vc_should_trash_original (TRUE, TRUE, 60.0, 60.9, TRUE));
    g_assert_true (vc_should_trash_original (TRUE, TRUE, 60.0, 59.1, TRUE));
    g_assert_false (vc_should_trash_original (TRUE, TRUE, 60.0, 61.1, TRUE));
    g_assert_false (vc_should_trash_original (TRUE, TRUE, 60.0, 58.9, TRUE));
    /* unknown source duration, or output probe failed (0) */
    g_assert_false (vc_should_trash_original (TRUE, TRUE, 0.0, 0.0, TRUE));
    g_assert_false (vc_should_trash_original (TRUE, TRUE, 0.5, 0.0, TRUE));
    g_assert_false (vc_should_trash_original (TRUE, TRUE, 0.0, 0.5, TRUE));
    /* every other combination of the three flags */
    for (int m = 0; m < 7; m++)
        g_assert_false (vc_should_trash_original (m & 1, m & 2, 60.0, 60.0, m & 4));
}

static void
test_format_duration (void)
{
    struct { double s; const char *text; } cases[] = {
        { 0, "?:??" }, { -1, "?:??" }, { 0.4, "0:00" }, { 59.6, "1:00" },
        { 60.07, "1:00" }, { 754, "12:34" }, { 3723, "1:02:03" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autofree char *t = vc_format_duration (cases[i].s);
        g_assert_cmpstr (t, ==, cases[i].text);
    }
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/class/boundaries", test_class_boundaries);
    g_test_add_func ("/scan/video-names", test_video_names);
    g_test_add_func ("/probe/argv", test_ffprobe_argv);
    g_test_add_func ("/probe/good", test_parse_probe_good);
    g_test_add_func ("/probe/missing-duration", test_parse_probe_missing_duration);
    g_test_add_func ("/probe/bad", test_parse_probe_bad);
    g_test_add_func ("/probe/first-video-wins", test_parse_probe_first_video_wins);
    g_test_add_func ("/output/path", test_output_path);
    g_test_add_func ("/ffmpeg/argv-8bit", test_ffmpeg_argv_8bit);
    g_test_add_func ("/ffmpeg/argv-10bit-hlg", test_ffmpeg_argv_10bit_hlg);
    g_test_add_func ("/ffmpeg/argv-presets", test_ffmpeg_argv_presets);
    g_test_add_func ("/ffmpeg/argv-subtitles", test_ffmpeg_argv_subtitles);
    g_test_add_func ("/progress/line", test_progress_line);
    g_test_add_func ("/rules/worth-keeping", test_worth_keeping);
    g_test_add_func ("/rules/should-trash", test_should_trash);
    g_test_add_func ("/format/duration", test_format_duration);
    return g_test_run ();
}
