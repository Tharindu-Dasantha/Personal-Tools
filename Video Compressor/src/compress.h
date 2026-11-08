#pragma once

#include <glib.h>

typedef struct {
    const char *label;   /* "720p" */
    int max_short_side;  /* inclusive upper bound */
    int crf;
    int audio_kbps;
} VcClass;

typedef struct {
    int width, height;
    char *codec;
    char *pix_fmt;
    /* NULL when ffprobe reports nothing or "unknown" */
    char *color_primaries, *color_transfer, *color_space;
    double duration;     /* seconds, 0 when unknown */
    guint n_subtitles;
    gboolean has_mov_text;
} VcProbe;

#define VC_ERROR (vc_error_quark ())
GQuark vc_error_quark (void);

extern const char *const VC_PRESETS[];   /* NULL-terminated */
#define VC_DEFAULT_PRESET "slow"

const VcClass *vc_class_for (int width, int height);
gboolean vc_is_video_name (const char *name);

char **vc_build_ffprobe_argv (const char *input);
VcProbe *vc_parse_probe (const char *text, GError **error);
void vc_probe_free (VcProbe *probe);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (VcProbe, vc_probe_free)
gboolean vc_probe_is_10bit (const VcProbe *probe);
gboolean vc_probe_is_hdr (const VcProbe *probe);

char *vc_output_path (const char *input, gboolean keep_subs, char **part_path);
char **vc_build_ffmpeg_argv (const char *input, const char *output, const VcProbe *probe,
                             const char *preset, gboolean keep_subs);
gboolean vc_parse_progress_line (const char *line, double duration, double *fraction);
gboolean vc_output_is_worth_keeping (guint64 src_bytes, guint64 out_bytes);
gboolean vc_should_trash_original (gboolean opt_on, gboolean exit_ok, double src_dur,
                                   double out_dur, gboolean kept);
char *vc_format_duration (double seconds);
void vc_preset_factors (const char *preset, double *time_vs_medium, double *size_vs_medium);
/* Rough compressed size in bytes, never above src_bytes; 0 when the duration is unknown. */
guint64 vc_estimate_size (const VcProbe *probe, guint64 src_bytes, const char *preset);
