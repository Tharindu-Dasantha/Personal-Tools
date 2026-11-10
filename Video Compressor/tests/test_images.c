#include "modes.h"

#include <glib/gstdio.h>
#include <string.h>

/* Captured with vc_build_ffprobe_argv() (ffmpeg 8.1): a JPEG, an RGBA PNG and a palette PNG. Still
 * images have no duration (or a 1-frame one). */
static const char PROBE_JPEG[] =
    "index=0\ncodec_name=mjpeg\ncodec_type=video\nwidth=4000\nheight=3000\npix_fmt=yuvj420p\n"
    "color_space=bt470bg\ncolor_transfer=unknown\ncolor_primaries=unknown\navg_frame_rate=25/1\n"
    "bit_rate=N/A\nduration=0.040000\n";
static const char PROBE_PNG_RGBA[] =
    "index=0\ncodec_name=png\ncodec_type=video\nwidth=640\nheight=480\npix_fmt=rgba\n"
    "color_space=gbr\ncolor_transfer=unknown\ncolor_primaries=unknown\navg_frame_rate=25/1\n"
    "bit_rate=N/A\nduration=N/A\n";
static const char PROBE_PNG_PAL8[] =
    "index=0\ncodec_name=png\ncodec_type=video\nwidth=64\nheight=48\npix_fmt=pal8\n"
    "color_space=gbr\ncolor_transfer=unknown\ncolor_primaries=bt709\navg_frame_rate=25/1\n"
    "bit_rate=N/A\nduration=N/A\n";

static VcProbe *
probe_with (const char *pix_fmt)
{
    VcProbe *p = g_new0 (VcProbe, 1);
    p->width = 800;
    p->height = 600;
    p->codec = g_strdup ("bmp");
    p->pix_fmt = g_strdup (pix_fmt);
    return p;
}

static void
assert_argv (char **argv, const char *const *want)
{
    g_assert_cmpuint (g_strv_length (argv), ==, g_strv_length ((char **) want));
    for (guint i = 0; want[i] != NULL; i++)
        g_assert_cmpstr (argv[i], ==, want[i]);
}

static char *
cdir_file (const char *dir, const char *name)
{
    return g_build_filename (dir, "compressed", name, NULL);
}

static void
test_parse (void)
{
    const char *probes[] = { PROBE_JPEG, PROBE_PNG_RGBA, PROBE_PNG_PAL8 };
    for (gsize i = 0; i < G_N_ELEMENTS (probes); i++) {
        g_autoptr (GError) error = NULL;
        g_autoptr (VcProbe) p = vc_parse_probe (probes[i], &error);
        g_assert_no_error (error);
        g_assert_nonnull (p);
        g_assert_cmpint (p->width, >, 0);
        g_assert_nonnull (p->pix_fmt);
    }
    g_autoptr (VcProbe) png = vc_parse_probe (PROBE_PNG_RGBA, NULL);
    g_assert_cmpfloat (png->duration, ==, 0);
    g_assert_cmpstr (png->codec, ==, "png");
}

static void
test_accepts (void)
{
    const char *yes[] = { "a.jpg", "a.JPEG", "a.png", "a.webp", "a.BMP", "a.tif", "a.tiff" };
    const char *no[] = { "anim.gif", "logo.svg", "IMG_1.HEIC", "a.avif", "clip.mp4", ".x.part.jpg" };
    for (gsize i = 0; i < G_N_ELEMENTS (yes); i++)
        g_assert_true (vc_mode_accepts (VC_MODE_IMAGES, yes[i]));
    for (gsize i = 0; i < G_N_ELEMENTS (no); i++)
        g_assert_false (vc_mode_accepts (VC_MODE_IMAGES, no[i]));
}

static void
test_has_alpha (void)
{
    const char *alpha[] = { "rgba", "ya8", "yuva420p", "pal8", "bgra", "rgba64le", "gbrap" };
    const char *opaque[] = { "rgb24", "yuvj420p", "yuv420p", "gray", "bgr24", "rgb48le" };
    for (gsize i = 0; i < G_N_ELEMENTS (alpha); i++) {
        g_autoptr (VcProbe) p = probe_with (alpha[i]);
        g_assert_true (vc_probe_has_alpha (p));
    }
    for (gsize i = 0; i < G_N_ELEMENTS (opaque); i++) {
        g_autoptr (VcProbe) p = probe_with (opaque[i]);
        g_assert_false (vc_probe_has_alpha (p));
    }
}

static void
test_outputs (void)
{
    g_autoptr (VcProbe) jpeg = vc_parse_probe (PROBE_JPEG, NULL);
    g_autofree char *webp = NULL;
    g_autofree char *same = vc_image_outputs ("/p/IMG.JPEG", jpeg, &webp);
    g_assert_cmpstr (same, ==, "/p/compressed/IMG.jpg");
    g_assert_cmpstr (webp, ==, "/p/compressed/IMG.webp");

    g_autofree char *w2 = (char *) 1;
    g_autofree char *s2 = vc_image_outputs ("/p/a.WebP", jpeg, &w2);
    g_assert_cmpstr (s2, ==, "/p/compressed/a.webp");
    g_assert_null (w2);

    g_autoptr (VcProbe) rgba = probe_with ("rgba");
    g_autoptr (VcProbe) rgb = probe_with ("rgb24");
    g_autofree char *b1 = vc_image_outputs ("/p/logo.bmp", rgba, NULL);
    g_autofree char *b2 = vc_image_outputs ("/p/scan.TIFF", rgb, NULL);
    g_assert_cmpstr (b1, ==, "/p/compressed/logo.png");
    g_assert_cmpstr (b2, ==, "/p/compressed/scan.jpg");
}

typedef struct { const char *file; const char *pix_fmt; int max_width; guint n; } PlanCase;

static void
test_plan (void)
{
    static const PlanCase cases[] = {
        { "a.jpg", "yuvj420p", 0, 2 },  { "a.png", "rgba", 0, 2 },    { "a.png", "rgba", 1280, 3 },
        { "a.webp", "yuv420p", 0, 1 },  { "a.bmp", "rgb24", 0, 2 },   { "a.bmp", "bgra", 0, 3 },
        { "a.tiff", "rgb48le", 0, 2 },  { "a.tif", "rgba", 640, 3 },  { "a.jpg", "yuvj420p", 1280, 2 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autoptr (VcProbe) p = probe_with (cases[i].pix_fmt);
        g_autofree char *input = g_build_filename ("/nonexistent", cases[i].file, NULL);
        VcOptions o = { .max_width = cases[i].max_width };
        g_autofree char *reason = NULL;
        g_autoptr (GPtrArray) steps = vc_mode_steps (VC_MODE_IMAGES, input, p, &o, &reason);
        g_assert_null (reason);
        g_assert_cmpuint (steps->len, ==, cases[i].n);
        for (guint s = 0; s < steps->len; s++) {
            VcStep *st = steps->pdata[s];
            g_assert_false (st->progress);
            /* only the 3-step plans have an intermediate, and it comes first */
            g_assert_true ((st->output == NULL) == (cases[i].n == 3 && s == 0));
            g_assert_true (g_str_has_prefix (st->argv[0], "magick") || g_str_has_prefix (st->argv[0], "pngquant"));
            g_assert_cmpint (st->soft_exit, ==, strcmp (st->argv[0], "pngquant") == 0 ? 99 : 0);
        }
        VcStep *last = steps->pdata[steps->len - 1];
        g_assert_true (g_str_has_suffix (last->output, ".webp"));
    }
}

static void
test_argv (void)
{
    g_autoptr (VcProbe) jpeg = vc_parse_probe (PROBE_JPEG, NULL);
    g_autoptr (VcProbe) png = vc_parse_probe (PROBE_PNG_RGBA, NULL);
    g_autofree char *reason = NULL;
    VcOptions o = { 0 };

    g_autoptr (GPtrArray) j = vc_image_steps ("/p/IMG.JPEG", jpeg, &o, &reason);
    VcStep *s = j->pdata[0];
    const char *jpg[] = { "magick", "/p/IMG.JPEG", "-auto-orient", "+profile", "!icc,*",
                          "-sampling-factor", "4:2:0", "-interlace", "JPEG", "-quality", "82",
                          "/p/compressed/.IMG.part.jpg", NULL };
    assert_argv (s->argv, jpg);
    g_assert_cmpstr (s->part, ==, "/p/compressed/.IMG.part.jpg");
    g_assert_cmpstr (s->output, ==, "/p/compressed/IMG.jpg");
    s = j->pdata[1];
    const char *wp[] = { "magick", "/p/IMG.JPEG", "-auto-orient", "+profile", "!icc,*", "-quality",
                         "80", "-define", "webp:method=6", "/p/compressed/.IMG.part.webp", NULL };
    assert_argv (s->argv, wp);
    g_assert_cmpstr (s->output, ==, "/p/compressed/IMG.webp");

    o.max_width = 1280;
    g_autoptr (GPtrArray) jr = vc_image_steps ("/p/IMG.JPEG", jpeg, &o, &reason);
    const char *jpg_r[] = { "magick", "/p/IMG.JPEG", "-auto-orient", "+profile", "!icc,*",
                            "-resize", "1280>", "-sampling-factor", "4:2:0", "-interlace", "JPEG",
                            "-quality", "82", "/p/compressed/.IMG.part.jpg", NULL };
    assert_argv (((VcStep *) jr->pdata[0])->argv, jpg_r);
    const char *wp_r[] = { "magick", "/p/IMG.JPEG", "-auto-orient", "+profile", "!icc,*", "-resize",
                           "1280>", "-quality", "80", "-define", "webp:method=6",
                           "/p/compressed/.IMG.part.webp", NULL };
    assert_argv (((VcStep *) jr->pdata[1])->argv, wp_r);

    /* PNG with a resize: magick to an intermediate, then pngquant reads it after `--` */
    g_autoptr (GPtrArray) pr = vc_image_steps ("/p/logo.png", png, &o, &reason);
    const char *mid[] = { "magick", "/p/logo.png", "-auto-orient", "-resize", "1280>",
                          "/p/compressed/.logo.mid.part.png", NULL };
    assert_argv (((VcStep *) pr->pdata[0])->argv, mid);
    g_assert_cmpstr (((VcStep *) pr->pdata[0])->part, ==, "/p/compressed/.logo.mid.part.png");
    const char *pq_mid[] = { "pngquant", "--quality=65-90", "--speed", "3", "--strip", "--output",
                             "/p/compressed/.logo.part.png", "--", "/p/compressed/.logo.mid.part.png",
                             NULL };
    assert_argv (((VcStep *) pr->pdata[1])->argv, pq_mid);
    g_assert_cmpstr (((VcStep *) pr->pdata[1])->output, ==, "/p/compressed/logo.png");

    o.max_width = 0;
    g_autoptr (GPtrArray) p0 = vc_image_steps ("/p/logo.png", png, &o, &reason);
    const char *pq[] = { "pngquant", "--quality=65-90", "--speed", "3", "--strip", "--output",
                         "/p/compressed/.logo.part.png", "--", "/p/logo.png", NULL };
    assert_argv (((VcStep *) p0->pdata[0])->argv, pq);

    /* A TIFF is read by its first page only */
    g_autoptr (VcProbe) rgb = probe_with ("rgb24");
    g_autoptr (GPtrArray) t = vc_image_steps ("/p/scan.tif", rgb, &o, &reason);
    g_assert_cmpstr (((VcStep *) t->pdata[0])->argv[1], ==, "/p/scan.tif[0]");
    g_assert_cmpstr (((VcStep *) t->pdata[0])->output, ==, "/p/compressed/scan.jpg");
}

static void
test_existing (void)
{
    g_autofree char *dir = g_dir_make_tmp ("vc-img-XXXXXX", NULL);
    g_autofree char *cdir = g_build_filename (dir, "compressed", NULL);
    g_assert_cmpint (g_mkdir (cdir, 0755), ==, 0);
    const char *name = "it's \"$5\" ünï 📷.jpg";
    g_autofree char *input = g_build_filename (dir, name, NULL);
    g_autoptr (VcProbe) p = vc_parse_probe (PROBE_JPEG, NULL);
    VcOptions o = { 0 };
    g_autofree char *reason = NULL;

    /* The name passes through untouched, as one argv element */
    g_autoptr (GPtrArray) all = vc_image_steps (input, p, &o, &reason);
    g_assert_cmpuint (all->len, ==, 2);
    g_assert_cmpstr (((VcStep *) all->pdata[0])->argv[1], ==, input);
    g_autofree char *want = cdir_file (dir, "it's \"$5\" ünï 📷.jpg");
    g_assert_cmpstr (((VcStep *) all->pdata[0])->output, ==, want);

    /* jpg exists: only the webp step is left */
    g_assert_true (g_file_set_contents (want, "x", 1, NULL));
    g_autoptr (GPtrArray) one = vc_image_steps (input, p, &o, &reason);
    g_assert_cmpuint (one->len, ==, 1);
    g_assert_true (g_str_has_suffix (((VcStep *) one->pdata[0])->output, ".webp"));

    /* both exist: skipped */
    g_autofree char *webp = cdir_file (dir, "it's \"$5\" ünï 📷.webp");
    g_assert_true (g_file_set_contents (webp, "x", 1, NULL));
    g_assert_null (vc_image_steps (input, p, &o, &reason));
    g_assert_cmpstr (reason, ==, "compressed/it's \"$5\" ünï 📷.jpg and it's \"$5\" ünï 📷.webp exist");

    /* only the webp exists: the jpg step is left */
    g_unlink (want);
    g_clear_pointer (&reason, g_free);
    g_autoptr (GPtrArray) jpg_only = vc_image_steps (input, p, &o, &reason);
    g_assert_cmpuint (jpg_only->len, ==, 1);
    g_assert_true (g_str_has_suffix (((VcStep *) jpg_only->pdata[0])->output, ".jpg"));

    /* a WebP source has one output, which exists */
    g_autofree char *w_in = g_build_filename (dir, "it's \"$5\" ünï 📷.webp", NULL);
    g_assert_null (vc_image_steps (w_in, p, &o, &reason));
    g_assert_cmpstr (reason, ==, "compressed/it's \"$5\" ünï 📷.webp exists");

    g_unlink (webp);
    g_rmdir (cdir);
    g_rmdir (dir);
}

static void
test_expected_size (void)
{
    int w, h;
    vc_image_expected_size (4000, 3000, 1280, &w, &h);
    g_assert_cmpint (w, ==, 1280);
    g_assert_cmpint (h, ==, 960);
    vc_image_expected_size (1001, 667, 1280, &w, &h);   /* never upscaled */
    g_assert_cmpint (w, ==, 1001);
    g_assert_cmpint (h, ==, 667);
    vc_image_expected_size (2400, 1601, 0, &w, &h);
    g_assert_cmpint (w, ==, 2400);
    g_assert_cmpint (h, ==, 1601);
}

static void
test_should_trash (void)
{
    g_assert_true (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 1280, 960));
    g_assert_false (vc_image_should_trash (FALSE, TRUE, TRUE, 1280, 960, 1280, 960));
    g_assert_false (vc_image_should_trash (TRUE, FALSE, TRUE, 1280, 960, 1280, 960));
    g_assert_false (vc_image_should_trash (TRUE, TRUE, FALSE, 1280, 960, 1280, 960));
    g_assert_false (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 1279, 960));
    g_assert_false (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 0, 0));
    g_assert_true (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 1280, 961));
    g_assert_true (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 1280, 959));
    g_assert_false (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 1280, 962));
    g_assert_false (vc_image_should_trash (TRUE, TRUE, TRUE, 1280, 960, 1280, 958));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/images/parse", test_parse);
    g_test_add_func ("/images/accepts", test_accepts);
    g_test_add_func ("/images/has-alpha", test_has_alpha);
    g_test_add_func ("/images/outputs", test_outputs);
    g_test_add_func ("/images/plan", test_plan);
    g_test_add_func ("/images/argv", test_argv);
    g_test_add_func ("/images/existing", test_existing);
    g_test_add_func ("/images/expected-size", test_expected_size);
    g_test_add_func ("/images/should-trash", test_should_trash);
    return g_test_run ();
}
