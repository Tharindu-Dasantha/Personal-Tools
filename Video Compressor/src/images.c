#include "modes.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SRC_JPEG, SRC_PNG, SRC_WEBP, SRC_OTHER } SrcType;   /* OTHER: BMP or TIFF */

static SrcType
source_type (const char *input, gboolean *tiff)
{
    const char *dot = strrchr (input, '.');
    const char *ext = dot != NULL ? dot + 1 : "";
    *tiff = g_ascii_strcasecmp (ext, "tif") == 0 || g_ascii_strcasecmp (ext, "tiff") == 0;
    return g_ascii_strcasecmp (ext, "jpg") == 0 || g_ascii_strcasecmp (ext, "jpeg") == 0 ? SRC_JPEG
           : g_ascii_strcasecmp (ext, "png") == 0 ? SRC_PNG
           : g_ascii_strcasecmp (ext, "webp") == 0 ? SRC_WEBP
           : SRC_OTHER;
}

gboolean
vc_probe_has_alpha (const VcProbe *probe)
{
    const char *f = probe->pix_fmt != NULL ? probe->pix_fmt : "";
    /* pal8 can carry a transparent colour (PNG tRNS), and palette images suit PNG anyway. */
    return strcmp (f, "pal8") == 0 || g_str_has_prefix (f, "ya") || g_str_has_prefix (f, "yuva")
           || g_str_has_prefix (f, "gbrap") || strstr (f, "rgba") != NULL || strstr (f, "bgra") != NULL
           || strstr (f, "argb") != NULL || strstr (f, "abgr") != NULL;
}

/* TRUE when another image in the input's folder has the same stem (photo.jpg + photo.png/.webp/.JPG).
 * Read from the folder, not the list, so the answer never depends on which files were chosen.
 * One scan per folder, counting images per stem, redone when the folder's mtime changes (creating
 * or removing a file changes it; outputs go into a subfolder, which doesn't). Main thread only. */
static gboolean
has_twin (const char *dir, const char *stem)
{
    static GHashTable *stems;   /* stem -> number of images with it */
    static char *stems_dir;
    static struct timespec stems_mtime;
    GStatBuf st;
    if (g_stat (dir, &st) != 0)
        return FALSE;
    if (stems == NULL || g_strcmp0 (dir, stems_dir) != 0 || st.st_mtim.tv_sec != stems_mtime.tv_sec
        || st.st_mtim.tv_nsec != stems_mtime.tv_nsec) {
        g_clear_pointer (&stems, g_hash_table_unref);
        stems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
        for (const char *e; d != NULL && (e = g_dir_read_name (d)) != NULL;) {
            const char *dot = strrchr (e, '.');
            if (dot == NULL || dot == e || !vc_mode_accepts (VC_MODE_IMAGES, e))
                continue;
            char *s = g_strndup (e, dot - e);
            guint n = GPOINTER_TO_UINT (g_hash_table_lookup (stems, s));
            g_hash_table_replace (stems, s, GUINT_TO_POINTER (n + 1));
        }
        g_free (stems_dir);
        stems_dir = g_strdup (dir);
        stems_mtime = st.st_mtim;
    }
    return GPOINTER_TO_UINT (g_hash_table_lookup (stems, stem)) > 1;   /* the input counts once */
}

/* compressed/<stem>.<ext> next to the input, or its hidden part .<stem><tag>.part.<ext>. With a twin
 * the stem is the whole name (photo.png.webp), so twins never share an output. */
static char *
out_path (const char *input, const char *tag, const char *ext)
{
    g_autofree char *dir = g_path_get_dirname (input);
    g_autofree char *name = g_path_get_basename (input);
    g_autofree char *base = g_strdup (name);
    char *dot = strrchr (base, '.');
    if (dot != NULL && dot != base) {
        *dot = '\0';
        if (has_twin (dir, base)) {
            g_free (base);
            base = g_strdup (name);
        }
    }
    g_autofree char *file = tag != NULL ? g_strdup_printf (".%s%s.part.%s", base, tag, ext)
                                        : g_strdup_printf ("%s.%s", base, ext);
    return g_build_filename (dir, "compressed", file, NULL);
}

/* The same-type extension: BMP/TIFF become png (alpha) or jpg. */
static const char *
same_ext (SrcType t, const VcProbe *probe)
{
    return t == SRC_JPEG ? "jpg" : t == SRC_PNG ? "png" : t == SRC_WEBP ? "webp"
           : vc_probe_has_alpha (probe) ? "png" : "jpg";
}

char *
vc_image_outputs (const char *input, const VcProbe *probe, char **webp_path)
{
    gboolean tiff;
    SrcType t = source_type (input, &tiff);
    if (webp_path != NULL)
        *webp_path = t == SRC_WEBP ? NULL : out_path (input, NULL, "webp");
    return out_path (input, NULL, same_ext (t, probe));
}

void
vc_image_expected_size (int src_w, int src_h, int max_width, int *w, int *h)
{
    *w = max_width > 0 && src_w > max_width ? max_width : src_w;
    *h = (int) lround ((double) src_h * *w / src_w);
}

gboolean
vc_image_should_trash (gboolean opt_on, gboolean steps_ok, gboolean same_type_kept,
                       int exp_w, int exp_h, int out_w, int out_h)
{
    return opt_on && steps_ok && same_type_kept && out_w == exp_w && abs (out_h - exp_h) <= 1;
}

static VcStep *
new_step (GStrvBuilder *b, char *part, char *output)
{
    VcStep *s = g_new0 (VcStep, 1);
    s->argv = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    s->part = part;
    s->output = output;
    return s;
}

/* magick IN -auto-orient, reading only a TIFF's first page (more would write one file each). */
static GStrvBuilder *
magick_in (const char *input, gboolean tiff)
{
    GStrvBuilder *b = g_strv_builder_new ();
    g_autofree char *in = tiff ? g_strconcat (input, "[0]", NULL) : g_strdup (input);
    g_strv_builder_add_many (b, "magick", in, "-auto-orient", NULL);
    return b;
}

static void
add_resize (GStrvBuilder *b, const VcOptions *opt)
{
    if (opt->max_width <= 0)
        return;
    g_autofree char *geo = g_strdup_printf ("%d>", opt->max_width);   /* '>': shrink only */
    g_strv_builder_add_many (b, "-resize", geo, NULL);
}

GPtrArray *
vc_image_steps (const char *input, const VcProbe *probe, const VcOptions *opt, char **skip_reason)
{
    *skip_reason = NULL;
    gboolean tiff;
    SrcType t = source_type (input, &tiff);
    const char *ext = same_ext (t, probe);
    g_autofree char *webp = NULL;
    g_autofree char *same = vc_image_outputs (input, probe, &webp);
    gboolean need_same = !g_file_test (same, G_FILE_TEST_EXISTS);
    gboolean need_webp = webp != NULL && !g_file_test (webp, G_FILE_TEST_EXISTS);
    if (!need_same && !need_webp) {
        g_autofree char *a = g_path_get_basename (same);
        g_autofree char *da = g_filename_display_name (a);
        if (webp == NULL) {
            *skip_reason = g_strdup_printf ("compressed/%s exists", da);
        } else {
            g_autofree char *b = g_path_get_basename (webp);
            g_autofree char *db = g_filename_display_name (b);
            *skip_reason = g_strdup_printf ("compressed/%s and %s exist", da, db);
        }
        return NULL;
    }

    GPtrArray *steps = g_ptr_array_new_with_free_func ((GDestroyNotify) vc_step_free);
    if (need_same && strcmp (ext, "jpg") == 0) {
        GStrvBuilder *b = magick_in (input, tiff);
        g_strv_builder_add_many (b, "+profile", "!icc,*", NULL);
        add_resize (b, opt);
        char *part = out_path (input, "", "jpg");
        g_strv_builder_add_many (b, "-sampling-factor", "4:2:0", "-interlace", "JPEG",
                                 "-quality", "82", part, NULL);
        g_ptr_array_add (steps, new_step (b, part, g_strdup (same)));
    } else if (need_same && strcmp (ext, "png") == 0) {
        /* pngquant reads only PNG and can't resize: BMP/TIFF or a resize go through magick first. */
        const char *pq_in = input;
        if (t != SRC_PNG || opt->max_width > 0) {
            GStrvBuilder *b = magick_in (input, tiff);
            add_resize (b, opt);
            char *mid = out_path (input, ".mid", "png");
            g_strv_builder_add (b, mid);
            g_ptr_array_add (steps, new_step (b, mid, NULL));
            pq_in = mid;
        }
        GStrvBuilder *b = g_strv_builder_new ();
        char *part = out_path (input, "", "png");
        g_strv_builder_add_many (b, "pngquant", "--quality=65-90", "--speed", "3", "--strip",
                                 "--output", part, "--", pq_in, NULL);
        VcStep *s = new_step (b, part, g_strdup (same));
        s->soft_exit = 99;   /* can't reach quality 65: no output, not an error */
        g_ptr_array_add (steps, s);
    }
    if (need_webp || (need_same && t == SRC_WEBP)) {
        GStrvBuilder *b = magick_in (input, tiff);
        g_strv_builder_add_many (b, "+profile", "!icc,*", NULL);
        add_resize (b, opt);
        char *part = out_path (input, "", "webp");
        g_strv_builder_add_many (b, "-quality", "80", "-define", "webp:method=6", part, NULL);
        g_ptr_array_add (steps, new_step (b, part, g_strdup (t == SRC_WEBP ? same : webp)));
    }
    return steps;
}
