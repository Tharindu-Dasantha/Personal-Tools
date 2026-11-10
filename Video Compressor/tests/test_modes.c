#include "modes.h"

#include <glib/gstdio.h>

static const VcMode MODES[] = { VC_MODE_COMPRESS, VC_MODE_WEB_VIDEO, VC_MODE_FRAMES, VC_MODE_IMAGES };

/* Same captures as test_compress.c: subs-movtext.mp4 and hlg-10bit.mov. */
static const char PROBE_8BIT_SUBS[] =
    "index=0\ncodec_name=h264\ncodec_type=video\nwidth=640\nheight=360\npix_fmt=yuv420p\n"
    "color_space=unknown\ncolor_transfer=unknown\ncolor_primaries=unknown\n"
    "index=1\ncodec_name=aac\ncodec_type=audio\n"
    "index=2\ncodec_name=mov_text\ncodec_type=subtitle\nwidth=N/A\nheight=N/A\n"
    "duration=10.000000\n";
static const char PROBE_HLG[] =
    "index=0\ncodec_name=prores\ncodec_type=video\nwidth=1280\nheight=720\n"
    "pix_fmt=yuv422p10le\ncolor_space=bt2020nc\ncolor_transfer=arib-std-b67\n"
    "color_primaries=bt2020\nindex=1\ncodec_name=aac\ncodec_type=audio\n"
    "duration=10.000000\n";

static void
test_accepts (void)
{
    const char *videos[] = { "a.mp4", "B.MKV", "c.MoV", "d.webm" };
    const char *images[] = { "a.jpg", "B.JPEG", "c.png", "d.WebP", "e.bmp", "f.tif", "g.TIFF" };
    const char *never[] = { ".a.part.mp4", ".a.part.png", ".hidden.jpg", "notes.txt", "anim.gif",
                            "logo.svg", "noext", "compressed" };
    for (gsize m = 0; m < G_N_ELEMENTS (MODES); m++) {
        gboolean img = MODES[m] == VC_MODE_IMAGES;
        for (gsize i = 0; i < G_N_ELEMENTS (videos); i++)
            g_assert_true (vc_mode_accepts (MODES[m], videos[i]) == !img);
        for (gsize i = 0; i < G_N_ELEMENTS (images); i++)
            g_assert_true (vc_mode_accepts (MODES[m], images[i]) == img);
        for (gsize i = 0; i < G_N_ELEMENTS (never); i++)
            g_assert_false (vc_mode_accepts (MODES[m], never[i]));
    }
}

static void
test_out_dir_and_trash (void)
{
    g_assert_cmpstr (vc_mode_out_dir (VC_MODE_COMPRESS), ==, "compressed");
    g_assert_cmpstr (vc_mode_out_dir (VC_MODE_WEB_VIDEO), ==, "web");
    g_assert_cmpstr (vc_mode_out_dir (VC_MODE_FRAMES), ==, "frames");
    g_assert_cmpstr (vc_mode_out_dir (VC_MODE_IMAGES), ==, "compressed");
    g_assert_true (vc_mode_can_trash (VC_MODE_COMPRESS));
    g_assert_false (vc_mode_can_trash (VC_MODE_WEB_VIDEO));
    g_assert_false (vc_mode_can_trash (VC_MODE_FRAMES));
    g_assert_true (vc_mode_can_trash (VC_MODE_IMAGES));
}

static void
test_missing_tool (void)
{
    const guint all = VC_HAVE_X265 | VC_HAVE_X264 | VC_HAVE_ZSCALE | VC_HAVE_LIBWEBP
                      | VC_HAVE_MAGICK | VC_HAVE_PNGQUANT;
    static const struct { VcMode mode; guint bit; const char *name; } needs[] = {
        { VC_MODE_COMPRESS, VC_HAVE_X265, "libx265" },
        { VC_MODE_WEB_VIDEO, VC_HAVE_X264, "libx264" }, { VC_MODE_WEB_VIDEO, VC_HAVE_ZSCALE, "zscale" },
        { VC_MODE_FRAMES, VC_HAVE_LIBWEBP, "libwebp" }, { VC_MODE_FRAMES, VC_HAVE_ZSCALE, "zscale" },
        { VC_MODE_IMAGES, VC_HAVE_MAGICK, "magick" }, { VC_MODE_IMAGES, VC_HAVE_PNGQUANT, "pngquant" },
    };
    for (gsize m = 0; m < G_N_ELEMENTS (MODES); m++)
        g_assert_null (vc_mode_missing_tool (MODES[m], all));
    /* Each tool missing in turn: its mode names it, and every mode that doesn't need it is ready. */
    for (gsize i = 0; i < G_N_ELEMENTS (needs); i++) {
        guint have = all & ~needs[i].bit;
        for (gsize m = 0; m < G_N_ELEMENTS (MODES); m++) {
            const char *missing = vc_mode_missing_tool (MODES[m], have);
            gboolean needed = FALSE;
            for (gsize j = 0; j < G_N_ELEMENTS (needs); j++)
                needed |= needs[j].mode == MODES[m] && needs[j].bit == needs[i].bit;
            if (needed)
                g_assert_cmpstr (missing, ==, needs[i].name);
            else
                g_assert_null (missing);
        }
    }
}

static void
assert_strv_equal (char **a, char **b)
{
    g_assert_cmpuint (g_strv_length (a), ==, g_strv_length (b));
    for (guint i = 0; a[i] != NULL; i++)
        g_assert_cmpstr (a[i], ==, b[i]);
}

static void
test_compress_steps (void)
{
    g_autofree char *dir = g_dir_make_tmp ("vc-modes-XXXXXX", NULL);
    g_autofree char *input = g_build_filename (dir, "my clip.mov", NULL);
    struct { const char *probe; VcOptions opt; } cases[] = {
        { PROBE_8BIT_SUBS, { .preset = "slow" } },
        { PROBE_HLG, { .preset = "faster", .max_short_side = 480, .max_fps = 30 } },
        { PROBE_8BIT_SUBS, { .preset = "medium", .keep_subs = TRUE } },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autoptr (VcProbe) p = vc_parse_probe (cases[i].probe, NULL);
        g_autofree char *part = NULL;
        g_autofree char *out = vc_output_path (input, cases[i].opt.keep_subs, &part);
        g_auto (GStrv) argv = vc_build_ffmpeg_argv (input, part, p, &cases[i].opt);

        g_autofree char *reason = NULL;
        g_autoptr (GPtrArray) steps = vc_mode_steps (VC_MODE_COMPRESS, input, p, &cases[i].opt, &reason);
        g_assert_null (reason);
        g_assert_cmpuint (steps->len, ==, 1);
        VcStep *s = steps->pdata[0];
        assert_strv_equal (s->argv, argv);
        g_assert_cmpstr (s->part, ==, part);
        g_assert_cmpstr (s->output, ==, out);
        g_assert_true (s->progress);
        g_assert_cmpint (s->soft_exit, ==, 0);
    }

    /* An existing output skips the item with the reason, and never offers to overwrite it. */
    g_autofree char *cdir = g_build_filename (dir, "compressed", NULL);
    g_autofree char *out = g_build_filename (cdir, "my clip.mp4", NULL);
    g_assert_cmpint (g_mkdir (cdir, 0755), ==, 0);
    g_assert_true (g_file_set_contents (out, "x", 1, NULL));
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_8BIT_SUBS, NULL);
    g_autofree char *reason = NULL;
    VcOptions o = { .preset = "slow" };
    g_assert_null (vc_mode_steps (VC_MODE_COMPRESS, input, p, &o, &reason));
    g_assert_cmpstr (reason, ==, "compressed/my clip.mp4 exists");
    /* With subtitles on the output is .mkv, which doesn't exist yet. */
    g_clear_pointer (&reason, g_free);
    o.keep_subs = TRUE;
    g_autoptr (GPtrArray) mkv = vc_mode_steps (VC_MODE_COMPRESS, input, p, &o, &reason);
    g_assert_nonnull (mkv);

    /* Images has no builder yet. */
    g_assert_null (vc_mode_steps (VC_MODE_IMAGES, input, p, &o, &reason));
    g_assert_null (reason);
    g_unlink (out);
    g_rmdir (cdir);
    g_rmdir (dir);
}

static void
test_remove_part (void)
{
    g_autofree char *dir = g_dir_make_tmp ("vc-part-XXXXXX", NULL);

    /* a file part, and one that is already gone */
    g_autofree char *file = g_build_filename (dir, ".clip.part.mp4", NULL);
    g_assert_true (g_file_set_contents (file, "x", 1, NULL));
    g_assert_true (vc_remove_part (file));
    g_assert_false (g_file_test (file, G_FILE_TEST_EXISTS));
    g_assert_true (vc_remove_part (file));

    /* a directory part with files */
    g_autofree char *pdir = g_build_filename (dir, ".clip.part", NULL);
    g_assert_cmpint (g_mkdir (pdir, 0755), ==, 0);
    for (int i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf ("%s/frame-%04d.png", pdir, i);
        g_assert_true (g_file_set_contents (name, "x", 1, NULL));
    }
    g_assert_true (vc_remove_part (pdir));
    g_assert_false (g_file_test (pdir, G_FILE_TEST_EXISTS));

    /* not a part: refused and untouched */
    g_autofree char *keep = g_build_filename (dir, "clip.mp4", NULL);
    g_autofree char *keep2 = g_build_filename (dir, "clip.partial.mp4", NULL);
    g_assert_true (g_file_set_contents (keep, "x", 1, NULL));
    g_assert_true (g_file_set_contents (keep2, "x", 1, NULL));
    g_assert_false (vc_remove_part (keep));
    g_assert_false (vc_remove_part (keep2));
    g_assert_false (vc_remove_part (dir));
    g_assert_true (g_file_test (keep, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (keep2, G_FILE_TEST_EXISTS));

    /* a part directory holding a subdirectory: refused, nothing inside removed */
    g_autofree char *nest = g_build_filename (dir, ".x.part", NULL);
    g_autofree char *sub = g_build_filename (nest, "sub", NULL);
    g_autofree char *inner = g_build_filename (nest, "a.png", NULL);
    g_assert_cmpint (g_mkdir_with_parents (sub, 0755), ==, 0);
    g_assert_true (g_file_set_contents (inner, "x", 1, NULL));
    g_assert_false (vc_remove_part (nest));
    g_assert_true (g_file_test (inner, G_FILE_TEST_EXISTS));
    g_assert_true (g_file_test (sub, G_FILE_TEST_IS_DIR));

    g_unlink (inner);
    g_rmdir (sub);
    g_rmdir (nest);
    g_unlink (keep);
    g_unlink (keep2);
    g_assert_cmpint (g_rmdir (dir), ==, 0);   /* nothing else was left behind */
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/modes/accepts", test_accepts);
    g_test_add_func ("/modes/out-dir-and-trash", test_out_dir_and_trash);
    g_test_add_func ("/modes/missing-tool", test_missing_tool);
    g_test_add_func ("/modes/compress-steps", test_compress_steps);
    g_test_add_func ("/modes/remove-part", test_remove_part);
    return g_test_run ();
}
