#pragma once

#include <glib.h>

typedef struct {
    const char *label;   /* "720p" */
    int max_short_side;  /* inclusive upper bound */
    int crf;
    int gpu_qp;          /* hevc_vaapi CQP that matches `crf` (tasks/plan-performance.md, R8) */
    int audio_kbps;
} VcClass;

typedef struct {
    int width, height;
    char *codec;
    char *pix_fmt;
    /* NULL when ffprobe reports nothing or "unknown" */
    char *color_primaries, *color_transfer, *color_space;
    double duration;     /* seconds, 0 when unknown */
    double fps;          /* average frame rate, 0 when unknown */
    int video_kbps;      /* the video stream's own bitrate, 0 when unknown (often in MKV) */
    int rotation;        /* display rotation in degrees as ffprobe reports it (phones: -90), 0 if none */
    int aac_kbps;        /* > 0: every audio stream is AAC, and this is the highest bitrate */
    guint n_subtitles;
    gboolean has_mov_text;
} VcProbe;

#define VC_ERROR (vc_error_quark ())
GQuark vc_error_quark (void);

/* NULL-terminated. VC_PRESET_GPU (first) is the Intel GPU encoder; the rest are x265 presets. */
extern const char *const VC_PRESETS[];
#define VC_PRESET_GPU "gpu"
#define VC_DEFAULT_PRESET "slow"

typedef struct {
    const char *preset;   /* a VC_PRESETS name */
    gboolean keep_subs;
    int max_short_side;   /* 0: keep the size; else downscale so the short side is at most this */
    int max_fps;          /* 0: keep the frame rate; else cap it */
} VcOptions;

const VcClass *vc_class_for (int width, int height);
gboolean vc_is_video_name (const char *name);

char **vc_build_ffprobe_argv (const char *input);
VcProbe *vc_parse_probe (const char *text, GError **error);
void vc_probe_free (VcProbe *probe);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (VcProbe, vc_probe_free)
gboolean vc_probe_is_10bit (const VcProbe *probe);
gboolean vc_probe_is_hdr (const VcProbe *probe);

char *vc_output_path (const char *input, gboolean keep_subs, char **part_path);
/* The class that sets quality and audio bitrate: the output size's, after any downscale. */
const VcClass *vc_output_class (const VcProbe *probe, const VcOptions *opt);
char **vc_build_ffmpeg_argv (const char *input, const char *output, const VcProbe *probe,
                             const VcOptions *opt);
/* Argv for a 1-frame test encode; exit 0 means the GPU encoder works on this machine. */
char **vc_build_gpu_test_argv (void);
gboolean vc_parse_progress_line (const char *line, double duration, double *fraction);
gboolean vc_output_is_worth_keeping (guint64 src_bytes, guint64 out_bytes);
gboolean vc_should_trash_original (gboolean opt_on, gboolean exit_ok, double src_dur,
                                   double out_dur, gboolean kept);
char *vc_format_duration (double seconds);
void vc_preset_factors (const char *preset, double *time_vs_medium, double *size_vs_medium);
/* Rough compressed size in bytes, never above src_bytes; 0 when the duration is unknown. */
guint64 vc_estimate_size (const VcProbe *probe, guint64 src_bytes, const VcOptions *opt);
/* FALSE only when the estimate is known and says the output would not be kept. */
gboolean vc_worth_encoding (const VcProbe *probe, guint64 src_bytes, const VcOptions *opt);
/* "h264" → "H.264", "hevc" → "HEVC"; unknown names are upper-cased. */
char *vc_codec_label (const char *codec);
/* "Less than a minute left", "About 9 min left", "About 1 h left", "About 1 h 20 min left". */
char *vc_format_time_left (double seconds);
