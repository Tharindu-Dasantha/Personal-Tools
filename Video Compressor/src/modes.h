#pragma once

#include "compress.h"

/* The app's modes (SPEC-app-shell.md). Options are compress.h's VcOptions; later modules add
 * their own fields there, and each builder reads only its own. */
typedef enum { VC_MODE_COMPRESS, VC_MODE_WEB_VIDEO, VC_MODE_FRAMES, VC_MODE_IMAGES } VcMode;

/* What a mode may need beyond ffmpeg and ffprobe, checked once at startup. */
enum {
    VC_HAVE_X265 = 1 << 0,     /* ffmpeg encoder libx265 */
    VC_HAVE_X264 = 1 << 1,     /* ffmpeg encoder libx264 */
    VC_HAVE_ZSCALE = 1 << 2,   /* ffmpeg filter zscale */
    VC_HAVE_LIBWEBP = 1 << 3,  /* ffmpeg encoder libwebp */
    VC_HAVE_MAGICK = 1 << 4,   /* `magick` on PATH */
    VC_HAVE_PNGQUANT = 1 << 5, /* `pngquant` on PATH */
};

typedef struct {
    char **argv;        /* argv[0] is "ffmpeg", "magick" or "pngquant" */
    char *part;         /* file or directory this step writes */
    char *output;       /* final path; NULL = intermediate, deleted when the job ends */
    gboolean progress;  /* TRUE: argv has -progress pipe:1, so vc_parse_progress_line drives the bar */
    int soft_exit;      /* non-zero exit code meaning "no output, not an error" (pngquant 99); 0 = none */
    gboolean part_is_dir; /* the runner creates `part` as an empty directory before the step */
    double duration;    /* seconds the progress counts to; 0 = the probe's duration */
} VcStep;

gboolean vc_mode_accepts (VcMode mode, const char *name);     /* extension filter, case-insensitive */
const char *vc_mode_out_dir (VcMode mode);                    /* "compressed", "web", "frames", "compressed" */
gboolean vc_mode_can_trash (VcMode mode);
const char *vc_mode_missing_tool (VcMode mode, guint have);   /* NULL when ready; `have` = VC_HAVE_* bits */
/* The steps for one item, in order (free with g_ptr_array_unref). NULL when every output already
 * exists, with *skip_reason set ("compressed/clip.mp4 exists"); NULL without a reason for a mode
 * that has no builder yet. */
GPtrArray *vc_mode_steps (VcMode mode, const char *input, const VcProbe *probe,
                          const VcOptions *opt, char **skip_reason);
void vc_step_free (VcStep *step);
/* Web video (SPEC-web-video.md): web/<name>.mp4 next to the input, and its part when part_path isn't NULL. */
char *vc_web_video_output (const char *input, char **part_path);
/* One H.264 step; NULL + "web/<name>.mp4 exists" when it does. */
GPtrArray *vc_web_video_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                               char **skip_reason);
/* The zscale + tonemap filters that turn HDR into SDR BT.709, or NULL for an SDR probe. */
char *vc_tonemap_chain (const VcProbe *probe);

/* Video → frames (SPEC-video-frames.md). "1:05.25" → 65.25; FALSE for anything else.
 * Empty string → TRUE, *seconds = 0. */
gboolean vc_parse_time (const char *text, double *seconds);
int vc_frames_jpeg_qscale (int quality);   /* 1..100 → mjpeg -q:v 31..2 */
/* Frames the range gives at opt->fps (0: source_fps); 0 when the range starts past the end or
 * the duration or rate is unknown. */
guint vc_frames_estimate (double duration, double source_fps, const VcOptions *opt);
/* frames/<stem> next to the input, and its part frames/.<stem>.part when part_path isn't NULL. */
char *vc_frames_output (const char *input, char **part_path);
/* One step writing a directory of frames; NULL + "frames/<stem>/ exists" or "starts after the end". */
GPtrArray *vc_frames_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                            char **skip_reason);

/* Deletes a step's part: a file, or a directory of regular files (emptied, then removed). Refuses,
 * touching nothing, a path whose name doesn't end in ".part" (before any extension) and a
 * directory that holds a subdirectory. TRUE when the path is gone (also when it never existed). */
gboolean vc_remove_part (const char *path);
