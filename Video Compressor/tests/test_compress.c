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
    g_auto (GStrv) argv = vc_build_ffmpeg_argv (name, out, p, &(VcOptions) { .preset = "slow" });

    const char *expected[] = {
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats",
        "-progress", "pipe:1", "-y", "-i", name,
        "-map", "0:v:0", "-map", "0:a?", "-map_metadata", "0",
        "-c:v", "libx265", "-preset", "slow", "-crf", "24", "-x265-params", "log-level=error",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "96k",
        "-sn", "-tag:v", "hvc1", "-movflags", "+faststart+use_metadata_tags",
        out, NULL,
    };
    g_assert_cmpstrv (argv, expected);
}

static void
test_ffmpeg_argv_10bit_hlg (void)
{
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_HLG, NULL);
    g_auto (GStrv) argv = vc_build_ffmpeg_argv ("/v/a.mov", "/v/compressed/.a.part.mp4", p, &(VcOptions) { .preset = "slow" });
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
    g_auto (GStrv) argv2 = vc_build_ffmpeg_argv ("/v/b.mkv", "/v/compressed/.b.part.mp4", q, &(VcOptions) { .preset = "slow" });
    g_assert_cmpstr (argv_value (argv2, "-profile:v"), ==, "main10");
    g_assert_cmpstr (argv_value (argv2, "-color_primaries"), ==, "bt709");
    g_assert_cmpint (argv_find (argv2, "-color_trc"), ==, -1);
    g_assert_cmpint (argv_find (argv2, "-colorspace"), ==, -1);

    /* 8-bit never gets colour flags or main10 */
    g_autoptr (VcProbe) e = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) argv3 = vc_build_ffmpeg_argv ("/v/c.mp4", "/v/o.mp4", e, &(VcOptions) { .preset = "slow" });
    g_assert_cmpint (argv_find (argv3, "-profile:v"), ==, -1);
    g_assert_cmpint (argv_find (argv3, "-color_primaries"), ==, -1);
}

static void
test_ffmpeg_argv_presets (void)
{
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_HLG, NULL);
    g_assert_cmpstr (VC_DEFAULT_PRESET, ==, "slow");
    g_assert_cmpstr (VC_PRESETS[0], ==, VC_PRESET_GPU);
    for (int i = 1; VC_PRESETS[i] != NULL; i++) {
        g_auto (GStrv) argv = vc_build_ffmpeg_argv ("/a.mp4", "/b.mp4", p,
                                                    &(VcOptions) { .preset = VC_PRESETS[i] });
        g_assert_cmpstr (argv_value (argv, "-preset"), ==, VC_PRESETS[i]);
        g_assert_cmpstr (argv_value (argv, "-c:v"), ==, "libx265");
    }
}

static void
test_ffmpeg_argv_subtitles (void)
{
    /* mov_text source → srt */
    g_autoptr (VcProbe) mt = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) a = vc_build_ffmpeg_argv ("/v/a.mp4", "/v/compressed/.a.part.mkv", mt, &(VcOptions) { .preset = "slow", .keep_subs = TRUE });
    int s = argv_find (a, "0:s?");
    g_assert_cmpint (s, >, 0);
    g_assert_cmpstr (a[s - 1], ==, "-map");
    g_assert_cmpstr (argv_value (a, "-c:s"), ==, "srt");
    g_assert_cmpint (argv_find (a, "-sn"), ==, -1);
    g_assert_cmpint (argv_find (a, "hvc1"), ==, -1);
    g_assert_cmpint (argv_find (a, "-movflags"), ==, -1);

    /* other subtitle codecs (or none) → copy */
    g_autoptr (VcProbe) srt = vc_parse_probe (
        "index=0\ncodec_name=h264\ncodec_type=video\nwidth=640\nheight=360\n"
        "index=1\ncodec_name=subrip\ncodec_type=subtitle\n", NULL);
    g_assert_cmpuint (srt->n_subtitles, ==, 1);
    g_assert_false (srt->has_mov_text);
    g_auto (GStrv) b = vc_build_ffmpeg_argv ("/v/a.mkv", "/v/o.mkv", srt, &(VcOptions) { .preset = "slow", .keep_subs = TRUE });
    g_assert_cmpstr (argv_value (b, "-c:s"), ==, "copy");

    /* subtitles off */
    g_auto (GStrv) c = vc_build_ffmpeg_argv ("/v/a.mp4", "/v/o.mp4", mt, &(VcOptions) { .preset = "slow" });
    g_assert_cmpint (argv_find (c, "0:s?"), ==, -1);
    g_assert_cmpint (argv_find (c, "-c:s"), ==, -1);
    g_assert_cmpint (argv_find (c, "-sn"), >, 0);
    g_assert_cmpstr (argv_value (c, "-tag:v"), ==, "hvc1");
}

static VcProbe *
probe_of (const char *pix_fmt)
{
    VcProbe *p = g_new0 (VcProbe, 1);
    p->pix_fmt = g_strdup (pix_fmt);
    return p;
}

static void
test_10bit_formats (void)
{
    const char *eight[] = { "nv12", "yuv410p", "yuv420p", "yuvj420p", "rgb24", "yuv444p" };
    const char *ten[] = { "yuv420p10le", "yuv422p10le", "p010le", "yuv420p12le", "yuv420p10be",
                          "p012le", "gray10le" };
    for (gsize i = 0; i < G_N_ELEMENTS (eight); i++) {
        g_autoptr (VcProbe) p = probe_of (eight[i]);
        g_assert_false (vc_probe_is_10bit (p));
    }
    for (gsize i = 0; i < G_N_ELEMENTS (ten); i++) {
        g_autoptr (VcProbe) p = probe_of (ten[i]);
        g_assert_true (vc_probe_is_10bit (p));
    }
}

static void
test_ffmpeg_argv_hdr10 (void)
{
    g_autoptr (VcProbe) pq = vc_parse_probe (
        "index=0\ncodec_name=hevc\ncodec_type=video\nwidth=3840\nheight=2160\n"
        "pix_fmt=yuv420p10le\ncolor_space=bt2020nc\ncolor_transfer=smpte2084\n"
        "color_primaries=bt2020\n", NULL);
    g_auto (GStrv) a = vc_build_ffmpeg_argv ("/a.mkv", "/o.mp4", pq, &(VcOptions) { .preset = "slow" });
    g_assert_cmpstr (argv_value (a, "-x265-params"), ==, "log-level=error:hdr10=1:repeat-headers=1");
    g_assert_cmpstr (argv_value (a, "-color_trc"), ==, "smpte2084");

    g_autoptr (VcProbe) hlg = vc_parse_probe (PROBE_HLG, NULL);
    g_autoptr (VcProbe) sdr = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) b = vc_build_ffmpeg_argv ("/a.mov", "/o.mp4", hlg, &(VcOptions) { .preset = "slow" });
    g_auto (GStrv) c = vc_build_ffmpeg_argv ("/a.mp4", "/o.mp4", sdr, &(VcOptions) { .preset = "slow" });
    g_assert_cmpstr (argv_value (b, "-x265-params"), ==, "log-level=error");
    g_assert_cmpstr (argv_value (c, "-x265-params"), ==, "log-level=error");
}

/* 1080p video plus the given audio streams ("codec:bps", bps may be N/A). */
static VcProbe *
probe_with_audio (const char *const *audio)
{
    GString *t = g_string_new ("index=0\ncodec_name=h264\ncodec_type=video\nwidth=1920\n"
                               "height=1080\npix_fmt=yuv420p\navg_frame_rate=30/1\nbit_rate=16000000\n");
    for (int i = 0; audio[i] != NULL; i++) {
        g_auto (GStrv) f = g_strsplit (audio[i], ":", 2);
        g_string_append_printf (t, "index=%d\ncodec_name=%s\ncodec_type=audio\nbit_rate=%s\n",
                                i + 1, f[0], f[1]);
    }
    g_string_append (t, "duration=10.0\n");
    VcProbe *p = vc_parse_probe (t->str, NULL);
    g_string_free (t, TRUE);
    g_assert_nonnull (p);
    return p;
}

static void
test_audio_copy (void)
{
    /* 1080p class: 128k, so up to 140.8k copies */
    struct { const char *audio[3]; const char *codec; } cases[] = {
        { { "aac:128000", NULL }, "copy" },
        { { "aac:140000", "aac:96000", NULL }, "copy" },
        { { "aac:256000", NULL }, "aac" },
        { { "aac:N/A", NULL }, "aac" },
        { { "opus:96000", NULL }, "aac" },
        { { "ac3:128000", NULL }, "aac" },
        { { "aac:128000", "ac3:128000", NULL }, "aac" },
        { { "ac3:128000", "aac:128000", NULL }, "aac" },
        { { NULL }, "aac" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autoptr (VcProbe) p = probe_with_audio (cases[i].audio);
        g_auto (GStrv) a = vc_build_ffmpeg_argv ("/a.mp4", "/o.mp4", p,
                                                 &(VcOptions) { .preset = "medium" });
        g_assert_cmpstr (argv_value (a, "-c:a"), ==, cases[i].codec);
        g_assert_cmpint (argv_find (a, "-b:a") > 0, ==, strcmp (cases[i].codec, "aac") == 0);
    }
    const char *const aac128[] = { "aac:128000", NULL };
    g_autoptr (VcProbe) p = probe_with_audio (aac128);
    g_assert_cmpint (p->aac_kbps, ==, 128);
    g_assert_cmpfloat (p->fps, ==, 30.0);
}

static void
test_frame_rate_cap (void)
{
    struct { const char *rate; double fps; gboolean capped; } cases[] = {
        { "60/1", 60, TRUE }, { "60000/1001", 59.94, TRUE }, { "30000/1001", 29.97, FALSE },
        { "30/1", 30, FALSE }, { "24/1", 24, FALSE }, { "0/0", 0, FALSE }, { "N/A", 0, FALSE },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autofree char *t = g_strdup_printf ("index=0\ncodec_name=h264\ncodec_type=video\n"
                                              "width=1920\nheight=1080\navg_frame_rate=%s\n",
                                              cases[i].rate);
        g_autoptr (VcProbe) p = vc_parse_probe (t, NULL);
        g_assert_cmpfloat_with_epsilon (p->fps, cases[i].fps, 0.01);
        g_auto (GStrv) on = vc_build_ffmpeg_argv ("/a.mp4", "/o.mp4", p,
                                                  &(VcOptions) { .preset = "medium", .max_fps = 30 });
        g_auto (GStrv) off = vc_build_ffmpeg_argv ("/a.mp4", "/o.mp4", p,
                                                   &(VcOptions) { .preset = "medium" });
        g_assert_cmpstr (argv_value (on, "-fpsmax"), ==, cases[i].capped ? "30" : NULL);
        g_assert_cmpint (argv_find (off, "-fpsmax"), ==, -1);
    }
}

static VcProbe *
probe_sized (int w, int h, const char *codec)
{
    VcProbe *p = g_new0 (VcProbe, 1);
    p->width = w;
    p->height = h;
    p->codec = g_strdup (codec);
    p->pix_fmt = g_strdup ("yuv420p");
    p->duration = 60;
    return p;
}

static void
test_max_size (void)
{
    struct { int w, h, max; const char *vf, *crf, *abr; } cases[] = {
        /* 4K → 1080p class (CRF 27, 128k), by the short side for landscape and portrait */
        { 3840, 2160, 1080, "scale=w='if(gt(iw,ih),-2,1080)':h='if(gt(iw,ih),1080,-2)'", "27", "128k" },
        { 2160, 3840, 1080, "scale=w='if(gt(iw,ih),-2,1080)':h='if(gt(iw,ih),1080,-2)'", "27", "128k" },
        { 1920, 1080, 720, "scale=w='if(gt(iw,ih),-2,720)':h='if(gt(iw,ih),720,-2)'", "26", "128k" },
        /* odd source sizes: -2 rounds the long side to even */
        { 1917, 1079, 720, "scale=w='if(gt(iw,ih),-2,720)':h='if(gt(iw,ih),720,-2)'", "26", "128k" },
        /* never upscale; equal is left alone */
        { 1280, 720, 1080, NULL, "26", "128k" },
        { 1920, 1080, 1080, NULL, "27", "128k" },
        { 3840, 2160, 0, NULL, "28", "160k" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autoptr (VcProbe) p = probe_sized (cases[i].w, cases[i].h, "h264");
        VcOptions o = { .preset = "medium", .max_short_side = cases[i].max };
        g_auto (GStrv) a = vc_build_ffmpeg_argv ("/a.mp4", "/o.mp4", p, &o);
        g_assert_cmpstr (argv_value (a, "-vf"), ==, cases[i].vf);
        g_assert_cmpstr (argv_value (a, "-crf"), ==, cases[i].crf);
        g_assert_cmpstr (argv_value (a, "-b:a"), ==, cases[i].abr);
    }
    /* the estimate follows the output class: 4K capped to 1080p estimates like 1080p */
    g_autoptr (VcProbe) k4 = probe_sized (3840, 2160, "h264");
    g_autoptr (VcProbe) hd = probe_sized (1920, 1080, "h264");
    guint64 big = G_GUINT64_CONSTANT (1000000000);
    g_assert_cmpuint (vc_estimate_size (k4, big, &(VcOptions) { .preset = "medium", .max_short_side = 1080 }),
                      ==, vc_estimate_size (hd, big, &(VcOptions) { .preset = "medium" }));
    g_assert_cmpuint (vc_estimate_size (k4, big, &(VcOptions) { .preset = "medium" }), >,
                      vc_estimate_size (hd, big, &(VcOptions) { .preset = "medium" }));
}

static void
test_ffmpeg_argv_gpu (void)
{
    g_autoptr (VcProbe) sdr = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_auto (GStrv) a = vc_build_ffmpeg_argv ("/v/a.mp4", "/v/o.mp4", sdr,
                                             &(VcOptions) { .preset = VC_PRESET_GPU });
    const char *expected[] = {
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats",
        "-progress", "pipe:1", "-y",
        "-vaapi_device", "/dev/dri/renderD128", "-hwaccel", "vaapi", "-hwaccel_output_format", "vaapi",
        "-i", "/v/a.mp4", "-map", "0:v:0", "-map", "0:a?", "-map_metadata", "0",
        "-vf", "format=nv12|vaapi,hwupload",
        "-c:v", "hevc_vaapi", "-low_power", "1", "-rc_mode", "CQP", "-qp", "26",
        "-c:a", "aac", "-b:a", "96k",
        "-sn", "-tag:v", "hvc1", "-movflags", "+faststart+use_metadata_tags",
        "/v/o.mp4", NULL,
    };
    g_assert_cmpstrv (a, expected);

    /* 10-bit HLG: p010 upload, main10, colour tags; downscale uses scale_vaapi */
    g_autoptr (VcProbe) hlg = vc_parse_probe (PROBE_HLG, NULL);
    VcOptions o = { .preset = VC_PRESET_GPU, .max_short_side = 480 };
    g_auto (GStrv) b = vc_build_ffmpeg_argv ("/v/a.mov", "/v/o.mp4", hlg, &o);
    g_assert_cmpstr (argv_value (b, "-vf"), ==, "format=p010|vaapi,hwupload,"
                     "scale_vaapi=w='if(gt(iw,ih),-2,480)':h='if(gt(iw,ih),480,-2)'");
    g_assert_cmpstr (argv_value (b, "-profile:v"), ==, "main10");
    g_assert_cmpstr (argv_value (b, "-color_trc"), ==, "arib-std-b67");
    g_assert_cmpstr (argv_value (b, "-qp"), ==, "27");   /* 480p class */
    g_assert_cmpint (argv_find (b, "-pix_fmt"), ==, -1);
    g_assert_cmpint (argv_find (b, "-x265-params"), ==, -1);

    /* phone portrait (rotation tag): rotated on the GPU, autorotate off; the CPU path is unchanged */
    struct { const char *rot; const char *dir; } rots[] = {
        { "-90", "clock" }, { "90", "cclock" }, { "180", "reversal" }, { "-180", "reversal" },
        { "270", "clock" }, { "0", NULL },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (rots); i++) {
        g_autofree char *text = g_strdup_printf ("index=0\ncodec_name=hevc\ncodec_type=video\n"
                                                 "width=1920\nheight=1080\nrotation=%s\n", rots[i].rot);
        g_autoptr (VcProbe) r = vc_parse_probe (text, NULL);
        g_auto (GStrv) ga = vc_build_ffmpeg_argv ("/a.mov", "/o.mp4", r,
                                                  &(VcOptions) { .preset = VC_PRESET_GPU, .max_short_side = 720 });
        g_auto (GStrv) ca = vc_build_ffmpeg_argv ("/a.mov", "/o.mp4", r, &(VcOptions) { .preset = "fast" });
        g_autofree char *vf = rots[i].dir == NULL ? g_strdup ("format=nv12|vaapi,hwupload,scale_vaapi=w='if(gt(iw,ih),-2,720)':h='if(gt(iw,ih),720,-2)'")
            : g_strdup_printf ("format=nv12|vaapi,hwupload,transpose_vaapi=dir=%s,"
                               "scale_vaapi=w='if(gt(iw,ih),-2,720)':h='if(gt(iw,ih),720,-2)'", rots[i].dir);
        g_assert_cmpstr (argv_value (ga, "-vf"), ==, vf);
        g_assert_cmpint (argv_find (ga, "-noautorotate") >= 0, ==, rots[i].dir != NULL);
        g_assert_cmpint (argv_find (ga, "-noautorotate"), <, argv_find (ga, "-i") + (rots[i].dir == NULL ? 1 : 0));
        g_assert_cmpint (argv_find (ca, "-noautorotate"), ==, -1);
        g_assert_cmpint (argv_find (ca, "-vf"), ==, -1);
    }

    g_auto (GStrv) t = vc_build_gpu_test_argv ();
    g_assert_cmpstr (argv_value (t, "-c:v"), ==, "hevc_vaapi");
    g_assert_cmpstr (argv_value (t, "-frames:v"), ==, "1");
}

static void
test_estimate_codec_aware (void)
{
    VcOptions med = { .preset = "medium" };
    /* 1080p at 10 Mbps for 60 s: H.264 shrinks, an already lean HEVC (iPhone) does not */
    g_autoptr (VcProbe) h264 = probe_sized (1920, 1080, "h264");
    g_autoptr (VcProbe) hevc = probe_sized (1920, 1080, "hevc");
    guint64 src = 75000000;
    g_assert_true (vc_worth_encoding (src, vc_estimate_size (h264, src, &med)));
    /* 2.5 Mbps HEVC: the cap is its own bitrate, plus audio, so no 5% saving */
    g_assert_false (vc_worth_encoding (18750000, vc_estimate_size (hevc, 18750000, &med)));
    /* a lean lecture recording: 128k video + 192k AAC in 60 s (2.5 MB). Capping by the whole file's
     * bitrate counted the audio twice and predicted no saving; the real GPU result was −38%. */
    g_autoptr (VcProbe) lec = probe_sized (1280, 720, "h264");
    lec->video_kbps = 128;
    lec->aac_kbps = 192;
    VcOptions gpu = { .preset = VC_PRESET_GPU };
    g_assert_true (vc_worth_encoding (2499199, vc_estimate_size (lec, 2499199, &gpu)));
    g_assert_cmpuint (vc_estimate_size (lec, 2499199, &gpu), ==,
                      (guint64) ((1.15 * 128 * 0.6 + 128) * 1000.0 / 8.0 * 60));
    /* the video stream's bit_rate is parsed */
    g_autoptr (VcProbe) vb = vc_parse_probe ("index=0\ncodec_name=h264\ncodec_type=video\nwidth=640\n"
                                             "height=360\nbit_rate=128611\n", NULL);
    g_assert_cmpint (vb->video_kbps, ==, 128);
    /* unknown duration always encodes */
    hevc->duration = 0;
    g_assert_true (vc_worth_encoding (18750000, vc_estimate_size (hevc, 18750000, &med)));
}

static void
test_sample (void)
{
    /* 26 s rotated 10-bit HLG phone clip, Max 720p, Max 30 fps on, preset x265 slow: the sample is
     * still the GPU path with the same size/rotation/fps, 6 s from the middle, video only, to a pipe */
    g_autoptr (VcProbe) p = vc_parse_probe (
        "index=0\ncodec_name=hevc\ncodec_type=video\nwidth=1920\nheight=1080\npix_fmt=yuv420p10le\n"
        "color_transfer=arib-std-b67\navg_frame_rate=60/1\nrotation=-90\n"
        "index=1\ncodec_name=aac\ncodec_type=audio\nbit_rate=192000\nduration=26.0\n", NULL);
    VcOptions o = { .preset = "slow", .max_short_side = 720, .max_fps = 30, .keep_subs = TRUE };
    double secs = 0;
    g_auto (GStrv) a = vc_build_sample_argv ("/v/a.mov", p, &o, &secs);
    g_assert_cmpfloat (secs, ==, 12.0);
    /* three 4 s windows centred at 20/50/80%, each its own input and output, all to stdout */
    const char *starts[] = { "3.200", "11.000", "18.800" };
    guint n_in = 0, n_pipe = 0, n_dev = 0;
    for (guint i = 0; a[i] != NULL; i++) {
        if (strcmp (a[i], "-ss") == 0) {
            g_assert_cmpuint (n_in, <, 3);
            g_assert_cmpstr (a[i + 1], ==, starts[n_in]);
            g_assert_cmpstr (a[i + 2], ==, "-t");
            g_assert_cmpstr (a[i + 3], ==, "4.000");
            g_assert_cmpstr (a[i + 4], ==, "-i");
            n_in++;
        }
        n_pipe += strcmp (a[i], "pipe:1") == 0;
        n_dev += strcmp (a[i], "-vaapi_device") == 0;
    }
    g_assert_cmpuint (n_in, ==, 3);
    g_assert_cmpuint (n_pipe, ==, 3);
    g_assert_cmpuint (n_dev, ==, 1);
    g_assert_cmpint (argv_find (a, "2:v:0"), >, argv_find (a, "1:v:0"));
    g_assert_cmpint (argv_find (a, "1:v:0"), >, argv_find (a, "0:v:0"));
    g_assert_cmpint (argv_find (a, "-ss"), <, argv_find (a, "-i"));
    g_assert_cmpint (argv_find (a, "-noautorotate"), <, argv_find (a, "-i"));
    g_assert_cmpstr (argv_value (a, "-vf"), ==, "format=p010|vaapi,hwupload,transpose_vaapi=dir=clock,"
                     "scale_vaapi=w='if(gt(iw,ih),-2,720)':h='if(gt(iw,ih),720,-2)'");
    g_assert_cmpstr (argv_value (a, "-fpsmax"), ==, "30");
    g_assert_cmpstr (argv_value (a, "-c:v"), ==, "hevc_vaapi");
    g_assert_cmpstr (argv_value (a, "-qp"), ==, "28");
    g_assert_cmpstr (argv_value (a, "-f"), ==, "hevc");
    g_assert_cmpstr (a[g_strv_length (a) - 1], ==, "pipe:1");
    const char *absent[] = { "-progress", "-y", "-c:a", "0:a?", "0:s?", "-map_metadata", "-movflags",
                             "-x265-params", "-preset" };
    for (gsize i = 0; i < G_N_ELEMENTS (absent); i++)
        g_assert_cmpint (argv_find (a, absent[i]), ==, -1);
    g_assert_cmpint (argv_find (a, "-an"), >, 0);

    /* a clip of 12 s or less is sampled whole, in one window */
    p->duration = 5.5;
    g_auto (GStrv) b = vc_build_sample_argv ("/v/a.mov", p, &o, &secs);
    g_assert_cmpfloat (secs, ==, 5.5);
    g_assert_cmpstr (argv_value (b, "-ss"), ==, "0.000");
    g_assert_cmpstr (argv_value (b, "-t"), ==, "5.500");
    g_assert_cmpint (argv_find (b, "1:v:0"), ==, -1);

    /* estimate: GPU uses the measured bitrate; x265 presets scale by their size factor */
    p->duration = 60;
    VcOptions gpu = { .preset = VC_PRESET_GPU }, med = { .preset = "medium" };
    guint64 src = G_GUINT64_CONSTANT (100000000);
    /* 192k AAC > 1080p's 128k + 10%, so audio is re-encoded at 128k */
    g_assert_cmpuint (vc_estimate_from_sample (p, src, &gpu, 2300), ==,
                      (guint64) ((2300 + 128) * 1000.0 / 8.0 * 60));
    g_assert_cmpuint (vc_estimate_from_sample (p, src, &med, 2300), ==,
                      (guint64) ((2300 / 1.15 + 128) * 1000.0 / 8.0 * 60));
    g_assert_cmpuint (vc_estimate_from_sample (p, 1000, &gpu, 2300), ==, 1000);   /* never above */
    g_assert_cmpuint (vc_estimate_from_sample (p, src, &gpu, 0), ==, 0);
    g_assert_true (vc_worth_encoding (src, 0));
    g_assert_false (vc_worth_encoding (1000, 960));
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

static void
test_preset_factors (void)
{
    double t, sz;
    vc_preset_factors ("medium", &t, &sz);
    g_assert_cmpfloat (t, ==, 1.0);
    g_assert_cmpfloat (sz, ==, 1.0);
    vc_preset_factors ("nonsense", &t, &sz);
    g_assert_cmpfloat (t, ==, 1.0);
    g_assert_cmpfloat (sz, ==, 1.0);
    /* every dropdown preset has factors, and time grows with each slower preset */
    double prev = 0;
    for (guint i = 0; VC_PRESETS[i] != NULL; i++) {
        vc_preset_factors (VC_PRESETS[i], &t, &sz);
        g_assert_cmpfloat (t, >, prev);
        g_assert_cmpfloat (sz, >, 0.5);
        g_assert_cmpfloat (sz, <, 2.0);
        prev = t;
    }
}

static void
test_estimate_size (void)
{
    VcProbe p = { .width = 1280, .height = 720, .duration = 60.0 };
    /* 720p at medium: (1600 video + 128 audio) kbps for 60 s */
    g_assert_cmpuint (vc_estimate_size (&p, 60931727, &(VcOptions) { .preset = "medium" }), ==, 12960000);
    /* the preset size factor scales only the video part */
    double t, sz;
    vc_preset_factors ("slow", &t, &sz);
    g_assert_cmpuint (vc_estimate_size (&p, 60931727, &(VcOptions) { .preset = "slow" }), ==,
                      (guint64) ((1600 * sz + 128) * 1000.0 / 8.0 * 60.0));
    /* a lean source caps the video at 60% of its own bitrate: 6 MB over 60 s = 800 kbps */
    guint64 lean = vc_estimate_size (&p, 6000000, &(VcOptions) { .preset = "medium" });
    g_assert_cmpuint (lean, ==, (guint64) ((6000000 * 8.0 / 1000.0 / 60.0 * 0.6 + 128)
                                           * 1000.0 / 8.0 * 60.0));
    /* never above the source (an already tiny HEVC file), and 0 when the duration is unknown */
    g_assert_cmpuint (vc_estimate_size (&p, 97000, &(VcOptions) { .preset = "medium" }), <=, 97000);
    VcProbe unknown = { .width = 1280, .height = 720, .duration = 0 };
    g_assert_cmpuint (vc_estimate_size (&unknown, 60931727, &(VcOptions) { .preset = "medium" }), ==, 0);
    g_assert_cmpuint (vc_estimate_size (&p, 0, &(VcOptions) { .preset = "medium" }), ==, 0);
    /* portrait uses the short side, like the class */
    VcProbe portrait = { .width = 720, .height = 1280, .duration = 60.0 };
    g_assert_cmpuint (vc_estimate_size (&portrait, 60931727, &(VcOptions) { .preset = "medium" }), ==, 12960000);
}

static void
test_labels (void)
{
    struct { const char *in, *out; } codecs[] = {
        { "h264", "H.264" }, { "hevc", "HEVC" }, { "mpeg4", "MPEG-4" }, { "wmv3", "WMV3" }, { NULL, "?" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (codecs); i++) {
        g_autofree char *t = vc_codec_label (codecs[i].in);
        g_assert_cmpstr (t, ==, codecs[i].out);
    }
    struct { double s; const char *text; } left[] = {
        { 0, "Less than a minute left" }, { 59, "Less than a minute left" },
        { 540, "About 9 min left" }, { 3570, "About 1 h left" }, { 4800, "About 1 h 20 min left" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (left); i++) {
        g_autofree char *t = vc_format_time_left (left[i].s);
        g_assert_cmpstr (t, ==, left[i].text);
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
    g_test_add_func ("/probe/10bit-formats", test_10bit_formats);
    g_test_add_func ("/ffmpeg/argv-hdr10", test_ffmpeg_argv_hdr10);
    g_test_add_func ("/ffmpeg/audio-copy", test_audio_copy);
    g_test_add_func ("/ffmpeg/frame-rate-cap", test_frame_rate_cap);
    g_test_add_func ("/ffmpeg/max-size", test_max_size);
    g_test_add_func ("/ffmpeg/argv-gpu", test_ffmpeg_argv_gpu);
    g_test_add_func ("/estimate/codec-aware", test_estimate_codec_aware);
    g_test_add_func ("/estimate/sample", test_sample);
    g_test_add_func ("/progress/line", test_progress_line);
    g_test_add_func ("/rules/worth-keeping", test_worth_keeping);
    g_test_add_func ("/rules/should-trash", test_should_trash);
    g_test_add_func ("/format/duration", test_format_duration);
    g_test_add_func ("/format/labels", test_labels);
    g_test_add_func ("/estimate/preset-factors", test_preset_factors);
    g_test_add_func ("/estimate/size", test_estimate_size);
    return g_test_run ();
}
