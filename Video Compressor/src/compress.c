#include "compress.h"

#include <math.h>
#include <string.h>

G_DEFINE_QUARK (vc-error-quark, vc_error)

/* The only place to tune quality against size. Lower CRF on small frames because each
 * pixel covers more of the screen, so artifacts show more. */
static const VcClass VC_CLASSES[] = {
    { "240p",  300, 24, 27,  64 }, { "360p",   400, 24, 27,  96 },
    { "480p",  576, 25, 28,  96 }, { "720p",   800, 26, 29, 128 },
    { "1080p",1200, 27, 30, 128 }, { "4K", G_MAXINT, 28, 31, 160 },
};

const char *const VC_PRESETS[] = {
    VC_PRESET_GPU, "faster", "fast", "medium", "slow", "slower", NULL,
};

/* Rough output video bitrate per class at medium, in the same order as VC_CLASSES. Only used for
 * the size estimate shown before encoding; it never changes what is encoded.
 * shortcut: guessed from a noisy 16 Mbps 1080p test clip (3.7 Mbps at medium) and scaled by
 * pixel count; ignores frame rate. Recalibrate once real phone clips have been measured (T11). */
static const int EST_VIDEO_KBPS[] = { 350, 600, 900, 1600, 3000, 8000 };
G_STATIC_ASSERT (G_N_ELEMENTS (EST_VIDEO_KBPS) == G_N_ELEMENTS (VC_CLASSES));

/* Encode time and output size against medium, measured with this app's argv on this laptop
 * (tasks/plan.md, "Preset factors"). At a fixed CRF a slower preset keeps more detail; it does
 * not reliably make the file smaller. */
static const struct { const char *name; double time, size; } PRESET_FACTORS[] = {
    { VC_PRESET_GPU, 0.1, 1.5 },
    { "faster", 0.69, 0.99 }, { "fast", 0.85, 1.06 }, { "medium", 1.0, 1.0 },
    { "slow", 2.1, 1.08 },    { "slower", 8.2, 1.06 },
};

/* How far below its own bitrate HEVC can go for each source codec, for the estimate. Older codecs
 * need about twice HEVC's bitrate; a source that is already HEVC/VP9/AV1 gains little. */
static const struct { const char *codec; double cap; } SOURCE_CAP[] = {
    { "hevc", 1.0 }, { "vp9", 1.0 }, { "av1", 1.0 },
};
#define SOURCE_CAP_DEFAULT 0.6

static const char *const VIDEO_EXTS[] = {
    "mp4", "mkv", "mov", "avi", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "ts", "m2ts", "3gp",
};

const VcClass *
vc_class_for (int width, int height)
{
    /* Short side, so portrait video and rotation metadata don't change the class. */
    int short_side = MIN (width, height);
    for (gsize i = 0; i < G_N_ELEMENTS (VC_CLASSES); i++)
        if (short_side <= VC_CLASSES[i].max_short_side)
            return &VC_CLASSES[i];
    g_assert_not_reached ();
}

gboolean
vc_is_video_name (const char *name)
{
    /* Hidden names cover our own `.x.part.mp4` temp files. */
    if (name[0] == '.')
        return FALSE;
    const char *dot = strrchr (name, '.');
    if (dot == NULL)
        return FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS (VIDEO_EXTS); i++)
        if (g_ascii_strcasecmp (dot + 1, VIDEO_EXTS[i]) == 0)
            return TRUE;
    return FALSE;
}

char **
vc_build_ffprobe_argv (const char *input)
{
    /* `index` comes first in each stream's block, so it marks where a stream starts. */
    const char *argv[] = {
        "ffprobe", "-v", "error",
        "-show_entries",
        "stream=index,codec_type,codec_name,width,height,pix_fmt,"
        "color_primaries,color_transfer,color_space,avg_frame_rate,bit_rate:format=duration",
        "-of", "default=nw=1",
        "-i", input, NULL,
    };
    return g_strdupv ((char **) argv);
}

typedef struct {
    char *type, *codec, *pix_fmt, *primaries, *transfer, *space;
    const char *width, *height, *fps, *bit_rate;   /* point into the parsed text */
} Stream;

static void
stream_clear (Stream *s)
{
    g_free (s->type); g_free (s->codec); g_free (s->pix_fmt);
    g_free (s->primaries); g_free (s->transfer); g_free (s->space);
    memset (s, 0, sizeof *s);
}

static gboolean
parse_dim (const char *text, int *out)
{
    gint64 v;
    if (text == NULL || !g_ascii_string_to_signed (text, 10, 1, 65535, &v, NULL))
        return FALSE;
    *out = (int) v;
    return TRUE;
}

/* "30000/1001" → 29.97; 0 for "0/0" or anything unparsable. */
static double
parse_rate (const char *text)
{
    gint64 num, den;
    g_auto (GStrv) parts = text != NULL ? g_strsplit (text, "/", 2) : NULL;
    if (parts == NULL || parts[0] == NULL || parts[1] == NULL
        || !g_ascii_string_to_signed (parts[0], 10, 1, G_MAXINT32, &num, NULL)
        || !g_ascii_string_to_signed (parts[1], 10, 1, G_MAXINT32, &den, NULL))
        return 0;
    return (double) num / den;
}

/* Takes the stream's fields; the first video stream wins. */
static gboolean
commit_stream (VcProbe *p, Stream *s, gboolean *have_video, GError **error)
{
    gboolean ok = TRUE;
    if (g_strcmp0 (s->type, "video") == 0 && !*have_video) {
        *have_video = TRUE;
        if (!parse_dim (s->width, &p->width) || !parse_dim (s->height, &p->height)) {
            g_set_error (error, VC_ERROR, 0, "Bad video size");
            ok = FALSE;
        } else if (s->codec == NULL) {
            g_set_error (error, VC_ERROR, 0, "Unknown video codec");
            ok = FALSE;
        }
        p->codec = g_steal_pointer (&s->codec);
        p->pix_fmt = g_steal_pointer (&s->pix_fmt);
        p->color_primaries = g_steal_pointer (&s->primaries);
        p->color_transfer = g_steal_pointer (&s->transfer);
        p->color_space = g_steal_pointer (&s->space);
        p->fps = parse_rate (s->fps);
    } else if (g_strcmp0 (s->type, "audio") == 0) {
        gint64 bps;
        /* -1 marks a stream that can't be copied; vc_parse_probe turns it into 0 */
        if (p->aac_kbps >= 0 && g_strcmp0 (s->codec, "aac") == 0 && s->bit_rate != NULL
            && g_ascii_string_to_signed (s->bit_rate, 10, 1, G_MAXINT32, &bps, NULL))
            p->aac_kbps = MAX (p->aac_kbps, (int) ((bps + 999) / 1000));
        else
            p->aac_kbps = -1;
    } else if (g_strcmp0 (s->type, "subtitle") == 0) {
        p->n_subtitles++;
        if (g_strcmp0 (s->codec, "mov_text") == 0)
            p->has_mov_text = TRUE;
    }
    stream_clear (s);
    return ok;
}

VcProbe *
vc_parse_probe (const char *text, GError **error)
{
    g_autoptr (VcProbe) p = g_new0 (VcProbe, 1);
    g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
    Stream s = { 0 };
    gboolean in_stream = FALSE, have_video = FALSE, ok = TRUE;

    for (char **l = lines; *l != NULL && ok; l++) {
        char *eq = strchr (*l, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        const char *key = *l, *val = g_strstrip (eq + 1);
        /* "N/A"/"unknown" mean ffprobe has no value; treat both as absent. */
        if (strcmp (val, "N/A") == 0 || strcmp (val, "unknown") == 0 || *val == '\0')
            val = NULL;

        if (strcmp (key, "index") == 0) {
            if (in_stream)
                ok = commit_stream (p, &s, &have_video, error);
            in_stream = TRUE;
        } else if (strcmp (key, "duration") == 0 && val != NULL) {
            char *end;
            double d = g_ascii_strtod (val, &end);
            if (*end == '\0' && isfinite (d) && d > 0)
                p->duration = d;
        } else if (in_stream) {
            char **slot = strcmp (key, "codec_type") == 0 ? &s.type
                        : strcmp (key, "codec_name") == 0 ? &s.codec
                        : strcmp (key, "pix_fmt") == 0 ? &s.pix_fmt
                        : strcmp (key, "color_primaries") == 0 ? &s.primaries
                        : strcmp (key, "color_transfer") == 0 ? &s.transfer
                        : strcmp (key, "color_space") == 0 ? &s.space
                        : NULL;
            if (slot != NULL) {
                g_free (*slot);
                *slot = g_strdup (val);
            } else if (strcmp (key, "width") == 0) {
                s.width = val;   /* points into `lines`, parsed at commit */
            } else if (strcmp (key, "height") == 0) {
                s.height = val;
            } else if (strcmp (key, "avg_frame_rate") == 0) {
                s.fps = val;
            } else if (strcmp (key, "bit_rate") == 0) {
                s.bit_rate = val;
            }
        }
    }
    if (ok && in_stream)
        ok = commit_stream (p, &s, &have_video, error);
    stream_clear (&s);
    if (!ok)
        return NULL;
    if (!have_video) {
        g_set_error (error, VC_ERROR, 0, "No video stream");
        return NULL;
    }
    p->aac_kbps = MAX (p->aac_kbps, 0);
    return g_steal_pointer (&p);
}

void
vc_probe_free (VcProbe *probe)
{
    if (probe == NULL)
        return;
    g_free (probe->codec);
    g_free (probe->pix_fmt);
    g_free (probe->color_primaries);
    g_free (probe->color_transfer);
    g_free (probe->color_space);
    g_free (probe);
}

gboolean
vc_probe_is_10bit (const VcProbe *probe)
{
    /* Real depth markers only: "nv12" and "yuv410p" are 8-bit. p010le/p012le match 10le/12le. */
    static const char *const marks[] = { "p10", "p12", "10le", "10be", "12le", "12be" };
    for (gsize i = 0; probe->pix_fmt != NULL && i < G_N_ELEMENTS (marks); i++)
        if (strstr (probe->pix_fmt, marks[i]) != NULL)
            return TRUE;
    return FALSE;
}

gboolean
vc_probe_is_hdr (const VcProbe *probe)
{
    return g_strcmp0 (probe->color_transfer, "arib-std-b67") == 0       /* HLG */
        || g_strcmp0 (probe->color_transfer, "smpte2084") == 0;         /* PQ / HDR10 */
}

char *
vc_output_path (const char *input, gboolean keep_subs, char **part_path)
{
    g_autofree char *dir = g_path_get_dirname (input);
    g_autofree char *base = g_path_get_basename (input);
    char *dot = strrchr (base, '.');
    if (dot != NULL && dot != base)
        *dot = '\0';
    const char *ext = keep_subs ? "mkv" : "mp4";

    if (part_path != NULL) {
        g_autofree char *part_name = g_strdup_printf (".%s.part.%s", base, ext);
        *part_path = g_build_filename (dir, "compressed", part_name, NULL);
    }
    g_autofree char *name = g_strdup_printf ("%s.%s", base, ext);
    return g_build_filename (dir, "compressed", name, NULL);
}

static int
out_short_side (const VcProbe *probe, const VcOptions *opt)
{
    int side = MIN (probe->width, probe->height);
    return opt->max_short_side > 0 ? MIN (side, opt->max_short_side) : side;
}

const VcClass *
vc_output_class (const VcProbe *probe, const VcOptions *opt)
{
    int side = out_short_side (probe, opt);
    return vc_class_for (side, side);
}

/* shortcut: the first render node; pick the device another way only on a multi-GPU machine. */
#define GPU_DEVICE "/dev/dri/renderD128"

char **
vc_build_ffmpeg_argv (const char *input, const char *output, const VcProbe *probe,
                      const VcOptions *opt)
{
    const VcClass *cls = vc_output_class (probe, opt);
    gboolean gpu = g_strcmp0 (opt->preset, VC_PRESET_GPU) == 0;
    gboolean ten = vc_probe_is_10bit (probe);
    int side = out_short_side (probe, opt);
    GStrvBuilder *b = g_strv_builder_new ();
    char num[3][16];

    /* -y only ever hits our own .part temp file; final names are checked before encoding. */
    const char *head[] = {
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-nostats",
        "-progress", "pipe:1", "-y", NULL,
    };
    g_strv_builder_addv (b, head);
    if (gpu)
        /* Decode on the GPU too; a codec it can't decode falls back to the CPU on its own. */
        g_strv_builder_add_many (b, "-vaapi_device", GPU_DEVICE, "-hwaccel", "vaapi",
                                 "-hwaccel_output_format", "vaapi", NULL);
    g_strv_builder_add_many (b, "-i", input, "-map", "0:v:0", "-map", "0:a?", NULL);
    if (opt->keep_subs)
        g_strv_builder_add_many (b, "-map", "0:s?", NULL);
    /* ffmpeg 8 drops the source's creation_time unless metadata is mapped explicitly. */
    g_strv_builder_add_many (b, "-map_metadata", "0", NULL);

    /* Scale by the short side so portrait and rotated video work; -2 keeps the other side even. */
    GString *vf = g_string_new (NULL);
    if (gpu)
        /* format=…|vaapi takes GPU-decoded frames as they are and uploads CPU-decoded ones. */
        g_string_append (vf, ten ? "format=p010|vaapi,hwupload" : "format=nv12|vaapi,hwupload");
    if (side < MIN (probe->width, probe->height))
        g_string_append_printf (vf, "%s%s=w='if(gt(iw,ih),-2,%d)':h='if(gt(iw,ih),%d,-2)'",
                                vf->len > 0 ? "," : "", gpu ? "scale_vaapi" : "scale", side, side);
    if (vf->len > 0)
        g_strv_builder_add_many (b, "-vf", vf->str, NULL);
    g_string_free (vf, TRUE);
    if (opt->max_fps > 0 && probe->fps > opt->max_fps + 0.5) {
        g_snprintf (num[0], sizeof num[0], "%d", opt->max_fps);
        g_strv_builder_add_many (b, "-fpsmax", num[0], NULL);
    }

    if (gpu) {
        g_snprintf (num[1], sizeof num[1], "%d", cls->gpu_qp);
        /* low_power is the GPU's fixed-function encoder: ~10x x265 medium (plan-performance.md) */
        g_strv_builder_add_many (b, "-c:v", "hevc_vaapi", "-low_power", "1", "-rc_mode", "CQP",
                                 "-qp", num[1], NULL);
        if (ten)
            g_strv_builder_add_many (b, "-profile:v", "main10", NULL);
    } else {
        g_snprintf (num[1], sizeof num[1], "%d", cls->crf);
        /* PQ (HDR10) needs its SEI headers for players to switch to HDR; HLG does not. */
        gboolean pq = g_strcmp0 (probe->color_transfer, "smpte2084") == 0;
        g_strv_builder_add_many (b, "-c:v", "libx265", "-preset", opt->preset, "-crf", num[1],
                                 "-x265-params",
                                 pq ? "log-level=error:hdr10=1:repeat-headers=1" : "log-level=error",
                                 "-pix_fmt", ten ? "yuv420p10le" : "yuv420p", NULL);
        if (ten)
            g_strv_builder_add_many (b, "-profile:v", "main10", NULL);
    }
    if (ten) {
        /* Copying the colour tags is what keeps HDR from looking washed out. */
        if (probe->color_primaries != NULL)
            g_strv_builder_add_many (b, "-color_primaries", probe->color_primaries, NULL);
        if (probe->color_transfer != NULL)
            g_strv_builder_add_many (b, "-color_trc", probe->color_transfer, NULL);
        if (probe->color_space != NULL)
            g_strv_builder_add_many (b, "-colorspace", probe->color_space, NULL);
    }

    /* AAC already at about the target bitrate: copying saves a second lossy pass. */
    if (probe->aac_kbps > 0 && probe->aac_kbps * 10 <= cls->audio_kbps * 11) {
        g_strv_builder_add_many (b, "-c:a", "copy", NULL);
    } else {
        g_snprintf (num[2], sizeof num[2], "%dk", cls->audio_kbps);
        g_strv_builder_add_many (b, "-c:a", "aac", "-b:a", num[2], NULL);
    }

    if (opt->keep_subs)
        /* mov_text can't be stream-copied into MKV; srt keeps the text. */
        g_strv_builder_add_many (b, "-c:s", probe->has_mov_text ? "srt" : "copy", NULL);
    else
        /* hvc1 is what QuickTime/iOS need to play HEVC in MP4. use_metadata_tags keeps the
         * phone's own keys (location, creation date, camera), which MP4 drops otherwise. */
        g_strv_builder_add_many (b, "-sn", "-tag:v", "hvc1",
                                 "-movflags", "+faststart+use_metadata_tags", NULL);

    g_strv_builder_add (b, output);
    char **argv = g_strv_builder_end (b);
    g_strv_builder_unref (b);
    return argv;
}

char **
vc_build_gpu_test_argv (void)
{
    const char *argv[] = {
        "ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-vaapi_device", GPU_DEVICE,
        "-f", "lavfi", "-i", "color=s=256x256", "-frames:v", "1", "-vf", "format=nv12,hwupload",
        "-c:v", "hevc_vaapi", "-low_power", "1", "-f", "null", "-", NULL,
    };
    return g_strdupv ((char **) argv);
}

gboolean
vc_parse_progress_line (const char *line, double duration, double *fraction)
{
    if (strcmp (line, "progress=end") == 0) {
        *fraction = 1.0;
        return TRUE;
    }
    gint64 us;
    if (duration <= 0 || !g_str_has_prefix (line, "out_time_us=")
        || !g_ascii_string_to_signed (line + strlen ("out_time_us="), 10,
                                      G_MININT64, G_MAXINT64, &us, NULL))
        return FALSE;
    *fraction = CLAMP (us / 1e6 / duration, 0.0, 1.0);
    return TRUE;
}

gboolean
vc_output_is_worth_keeping (guint64 src_bytes, guint64 out_bytes)
{
    /* At least 5% smaller; integer math so 95% exactly counts as kept. */
    return out_bytes > 0 && out_bytes * 100 <= src_bytes * 95;
}

/* A duration of 0 means unknown (or the output probe failed), which never matches. */
gboolean
vc_should_trash_original (gboolean opt_on, gboolean exit_ok, double src_dur, double out_dur,
                          gboolean kept)
{
    return opt_on && exit_ok && kept && src_dur > 0 && out_dur > 0
           && fabs (src_dur - out_dur) <= 1.0;
}

char *
vc_format_duration (double seconds)
{
    if (seconds <= 0)
        return g_strdup ("?:??");
    guint64 s = (guint64) llround (seconds);
    if (s >= 3600)
        return g_strdup_printf ("%" G_GUINT64_FORMAT ":%02u:%02u",
                                s / 3600, (guint) (s / 60 % 60), (guint) (s % 60));
    return g_strdup_printf ("%" G_GUINT64_FORMAT ":%02u", s / 60, (guint) (s % 60));
}

void
vc_preset_factors (const char *preset, double *time_vs_medium, double *size_vs_medium)
{
    *time_vs_medium = *size_vs_medium = 1.0;
    for (gsize i = 0; i < G_N_ELEMENTS (PRESET_FACTORS); i++)
        if (g_strcmp0 (preset, PRESET_FACTORS[i].name) == 0) {
            *time_vs_medium = PRESET_FACTORS[i].time;
            *size_vs_medium = PRESET_FACTORS[i].size;
        }
}

guint64
vc_estimate_size (const VcProbe *probe, guint64 src_bytes, const VcOptions *opt)
{
    if (probe->duration <= 0 || src_bytes == 0)
        return 0;
    const VcClass *cls = vc_output_class (probe, opt);
    double time, size, cap = SOURCE_CAP_DEFAULT;
    vc_preset_factors (opt->preset, &time, &size);
    for (gsize i = 0; i < G_N_ELEMENTS (SOURCE_CAP); i++)
        if (g_strcmp0 (probe->codec, SOURCE_CAP[i].codec) == 0)
            cap = SOURCE_CAP[i].cap;
    /* A lean source caps the guess. shortcut: ignores the frame-rate cap (fewer frames, smaller
     * file), so the estimate is pessimistic there; refine if it hides real savings. */
    double src_kbps = src_bytes * 8.0 / 1000.0 / probe->duration;
    double video_kbps = size * MIN (EST_VIDEO_KBPS[cls - VC_CLASSES], src_kbps * cap);
    double bytes = (video_kbps + cls->audio_kbps) * 1000.0 / 8.0 * probe->duration;
    return MIN ((guint64) bytes, src_bytes);
}

gboolean
vc_worth_encoding (const VcProbe *probe, guint64 src_bytes, const VcOptions *opt)
{
    guint64 est = vc_estimate_size (probe, src_bytes, opt);
    return est == 0 || vc_output_is_worth_keeping (src_bytes, est);
}

char *
vc_codec_label (const char *codec)
{
    static const char *const names[][2] = {
        { "h264", "H.264" }, { "hevc", "HEVC" }, { "mpeg4", "MPEG-4" }, { "mpeg2video", "MPEG-2" },
        { "vp8", "VP8" }, { "vp9", "VP9" }, { "av1", "AV1" }, { "prores", "ProRes" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (names); i++)
        if (g_strcmp0 (codec, names[i][0]) == 0)
            return g_strdup (names[i][1]);
    return codec != NULL ? g_ascii_strup (codec, -1) : g_strdup ("?");
}

char *
vc_format_time_left (double seconds)
{
    if (seconds < 60)
        return g_strdup ("Less than a minute left");
    guint m = (guint) llround (seconds / 60);
    if (m < 60)
        return g_strdup_printf ("About %u min left", m);
    if (m % 60 == 0)
        return g_strdup_printf ("About %u h left", m / 60);
    return g_strdup_printf ("About %u h %u min left", m / 60, m % 60);
}
