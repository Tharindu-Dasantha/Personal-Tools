# Spec: app-shell

Module of Media Tools (history in [LOG.md](LOG.md)). Depends on `video-compress` ([SPEC.md](SPEC.md)), which it hosts as the first mode.

## Objective

Turn the Video Compressor window into **Media Tools**, a host for several modes, without changing what video compression
does. After this module the app looks and behaves as it does today, plus:

- a **Mode** dropdown in the header: `Compress video` (default), `Web video (Safari)`, `Video → frames`, `Compress images`.
  Until each later module lands, its entry is absent (not greyed).
- a **Choose Files** button (multi-select) next to **Choose Folder**. Both fill the same list.
- an **options bar** under the header that shows only the current mode's controls.
- a job runner that can run **several steps per item** (needed for images: a same-type copy and a `.webp`, sometimes via an
  intermediate file) and can write a **directory** as an output (needed for frames).

**User stories**
- I pick `Compress video`, choose a folder, and everything works exactly as in SPEC.md.
- I click **Choose Files**, multi-select three videos from different folders, and only those three are listed. Each one's
  output goes next to it (`<its folder>/compressed/`).
- I switch mode with a folder or file set loaded. The list reloads with the new mode's file filter (videos or images), and
  every row starts fresh. If some chosen files don't fit the new mode, the header says e.g. `2 files hidden (not images)`.
- If a mode needs a tool that isn't installed (e.g. pngquant), choosing that mode shows a banner, for example
  `Compress images needs pngquant: sudo dnf install pngquant`, and Start stays greyed. The other modes still work.

## Decisions
1. **Folder name, repo path and binary name stay** (`Video Compressor/`, `video-compressor`). Only the window title changes to
   `Media Tools`. Renaming them is a separate, later change.
2. **Mode is locked while anything is queued, encoding or being checked**, like Speed today.
3. **Switching mode resets row state.** The list is rebuilt from the same source (folder or file list) with the new filter, and
   results from the previous mode aren't kept. Existing outputs on disk are still detected and skipped, as today.
4. **One job at a time in the video modes; Compress images runs up to `MIN (8, cores / 2)` at once** (2026-11-11, PERFORMANCE.md item 2). magick and pngquant use about one core each, so 100 images took 71 s one at a time and 23 s eight at once, byte-identical outputs. Pause and Stop act on every running job; while any job is paused no new one starts (the rest of the queue waits, as before). Two GPU encodes at once were measured too and gave under 1.1×, so videos stay at one.
5. **Only `video-compress` and `image-compress` can trash originals.** In `web-video` and `video-frames` the Trash button and
   checkbox are hidden: those outputs are lossy derivatives, not replacements.
6. **The startup check is split.** `ffmpeg` and `ffprobe` are required to start, since every mode uses ffprobe to read files.
   Everything else is checked per mode:

   | Mode | Needs |
   |---|---|
   | Compress video | ffmpeg encoder `libx265` |
   | Web video | ffmpeg encoder `libx264`, filter `zscale` |
   | Video → frames | ffmpeg encoder `libwebp`, filter `zscale` |
   | Compress images | `magick` and `pngquant` on PATH |

   `ffmpeg -encoders` and `ffmpeg -filters` run once at startup. PATH lookups use `g_find_program_in_path()`.
7. **Size estimate and speed guide stay `Compress video`-only.** Other modes put their own short hint in the same row slot
   (e.g. `≈ 240 frames`), or nothing.

## Design

### Core (GTK-free, `src/modes.c` + `src/modes.h`)
```c
typedef enum { VC_MODE_COMPRESS, VC_MODE_WEB_VIDEO, VC_MODE_FRAMES, VC_MODE_IMAGES } VcMode;

typedef struct {
    char **argv;        /* argv[0] is "ffmpeg", "magick" or "pngquant" */
    char *part;         /* file or directory this step writes */
    char *output;       /* final path; NULL = intermediate, deleted when the job ends */
    gboolean progress;  /* TRUE: argv has -progress pipe:1, so the bar is driven by vc_parse_progress_line */
    int soft_exit;      /* non-zero exit code meaning "no output, not an error" (pngquant 99); 0 = none */
} VcStep;

typedef struct {        /* every mode's options; each builder reads only its own */
    const char *preset; gboolean keep_subs;              /* compress */
    int max_width;                                       /* web, frames, images; 0 = original */
    gboolean no_audio;                                   /* web */
    double fps, start, end; gboolean webp; int quality;  /* frames; fps 0 = source, end 0 = to the end */
} VcOptions;

gboolean   vc_mode_accepts (VcMode mode, const char *name);   /* extension filter, case-insensitive */
const char *vc_mode_out_dir (VcMode mode);                    /* "compressed", "web", "frames", "compressed" */
gboolean   vc_mode_can_trash (VcMode mode);
const char *vc_mode_missing_tool (VcMode mode, guint have);   /* NULL when ready; `have` = VC_HAVE_* bits */
/* NULL + reason ("compressed/clip.mp4 exists") when every output already exists */
GPtrArray *vc_mode_steps (VcMode mode, const char *input, const VcProbe *probe,
                          const VcOptions *opt, char **skip_reason);
void       vc_step_free (VcStep *step);
```
- The `VC_MODE_COMPRESS` branch of `vc_mode_steps()` wraps the existing `vc_output_path()` + `vc_build_ffmpeg_argv()` and
  returns one step. Those functions and their tests stay as they are.
- Each later module adds one source file with its own builder (`web_video.c`, `frames.c`, `images.c`), which
  `vc_mode_steps()` calls.

### UI + runner (`src/main.c`)
- An `Item` holds its step list. `start_job()` runs the steps in order, and a step starts only after the previous one has fully
  ended (stdout EOF, stderr EOF, exit). That is the same three-event wait as today, per step.
- When the last step exits 0 (or `soft_exit`), each step with an `output` and an existing `part` is moved into place with
  `g_file_move(..., G_FILE_COPY_NONE)`. Intermediate parts are deleted.
- On stop, failure or window close, every step's `part` is removed. A `part` directory is emptied (only the files the step
  wrote; it contains nothing else) and then removed. Never touch anything outside the parts.
- Steps without `progress` show a pulsing bar.
- The per-item `State` machine, pause (`SIGSTOP` on the current step's process), selection rules and `app.closing` checks are
  unchanged.
- Choose Files uses `gtk_file_dialog_open_multiple()` with the mode's filter. The list source is either one folder or a
  list of files, kept so that a mode switch can reload it.

## Tech Stack, Commands, Code Style
Same as [SPEC.md](SPEC.md). New source files go into the `vc_core` library in `CMakeLists.txt`. New test files each get their
own executable and `add_test()`.

## Project Structure (additions)
```
src/modes.c/.h          → VcMode, VcStep, VcOptions, filters, out dirs, tool check, step dispatch
tests/test_modes.c      → unit tests for modes.c
```

## Testing Strategy
- **Unit (`tests/test_modes.c`):**
  - `vc_mode_accepts()`: videos for the three video modes, images for images mode, case-insensitive, `.part` and hidden
    files rejected
  - `vc_mode_out_dir()` and `vc_mode_can_trash()` for every mode
  - `vc_mode_missing_tool()`: every mode with every tool missing in turn, and with all present
  - `vc_mode_steps(VC_MODE_COMPRESS, …)` returns exactly one step whose argv, part and output equal what the existing
    `vc_build_ffmpeg_argv()`/`vc_output_path()` give, for the 8-bit, 10-bit HLG and keep-subs cases
- **Regression:** all existing `test_compress` tests pass unchanged.
- **Manual (Xvfb, as in HANDOFF.md):** the full Phase 3–6 fixture run in `Compress video` mode, with the same results as
  before. Then Choose Files with two files from different folders, a mode switch with a folder loaded, and the
  missing-tool banner (`PATH` without pngquant).

## Boundaries
- **Always:** keep every SPEC.md rule for every mode: argv only, `.part` then rename, never overwrite, one job at a time (images: Decision 4),
  async I/O, an `app.closing` check in every new callback, tests before every commit.
- **Ask first:** renaming the folder or binary · letting web/frames modes trash originals · running jobs in parallel.
- **Never:** change what `Compress video` encodes or decides · delete anything that isn't a step's `part` · use
  `system()`/`popen()`.

## Success Criteria
1. In `Compress video` mode, SPEC.md success criteria 1–13 still hold (re-run on Xvfb with ASan).
2. Choose Files with files from two folders lists only those files, and each output lands in its own folder's `compressed/`.
3. Switching mode with a folder loaded relists it with the new filter, and the hidden-file count is right.
4. Mode, Choose Folder and Choose Files are locked while a job runs.
5. Hiding pngquant from PATH shows the banner in `Compress images` only, with Start greyed there and the other modes working.
6. Stopping a multi-step job mid-way (tested with images, once that module exists) leaves no `.part` file or directory.
7. `ctest` passes, and an ASan run of the full fixture set reports no errors.

## Open Questions
None.
