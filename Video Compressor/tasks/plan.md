# Implementation Plan: Video Compressor

Source of truth: [../SPEC.md](../SPEC.md). Task checklist: [todo.md](todo.md).

## Overview
This plan builds the GTK4 C app that compresses every video in a folder with ffmpeg/x265, choosing settings from each file's resolution class. Results go to `compressed/`, and there is an option to move the originals to the Trash. The work is ordered so that each phase ends with something runnable. The riskiest parts (calling ffmpeg asynchronously from GTK, and cancelling it) come early.

Confirmed decisions: no recursive scan, no downscaling, subtitles dropped by default (opt-in **Keep subtitles** writes `.mkv`), x265 preset `slow` by default (a **Speed** dropdown changes it), Linux only.

## Architecture Decisions
- **Two source files.** `compress.c` holds all the decisions and has no GTK code, so it can be unit-tested headless. `main.c` holds the UI and the async process wiring. Nothing more until something needs it.
- **ffmpeg as a subprocess (`GSubprocess`, argv only).** It is already installed, has libx265, and avoids linking libav*. Progress comes from `-progress pipe:1`.
- **Everything async on the GLib main loop.** There are no worker threads, so no locking is needed. One encode runs at a time, driven by a simple queue index.
- **Writes are crash-safe.** Each encode writes `.part` and is renamed on exit 0. Removing an original goes through the Trash only, after the output has been verified.
- **Tests:** GLib `g_test` via CTest. ffprobe and progress parsers are tested against captured text strings. Real video files are used only in the manual integration checks (made by `make-fixtures.sh`).

## Dependency Graph
```
CMake skeleton + startup check (T1)
   └── scan + list (T2)
         └── ffprobe + class table (T3)
               └── encode queue → .part → rename (T4)
                     ├── progress bar (T5)
                     ├── cancel + cleanup (T6)
                     ├── skip-existing + not-smaller rule (T7)
                     │     └── output verification + trash decision (T9)
                     │           └── delete-originals UI + trash (T10)
                     │                 ├── hardening + calibration (T11)
                     │                 └── size estimate, speed guide, Start/Pause/Stop/Trash buttons, multi-select (T12–T15)
                     └── speed dropdown + keep-subtitles option (T8)
```
T5, T6, T7 and T8 depend only on T4 and don't depend on each other. All three touch `main.c`, so they run in sequence rather than in parallel.

## Roadmap

| Phase | Goal | Tasks | You can… |
|---|---|---|---|
| 1. Foundation | The app builds, opens, and refuses to run without ffmpeg/x265 | T1 | launch an empty window |
| 2. See your videos | Pick a folder and see each video's class, codec, duration and size | T2, T3 | browse a folder of videos |
| 3. Compress | Batch-compress with live progress, cancel, safe rules, speed and subtitle options | T4–T8 | **use it for real (MVP)** |
| 4. Delete originals | Optionally move verified originals to the Trash | T9, T10 | free up disk space automatically |
| 5. Harden | Edge cases, sanitizers, real-world size/quality calibration | T11 | trust it on your actual library |
| 6. Controls and estimates | Size estimates, speed guide, Start/Pause/Stop/Trash buttons, multi-select | T12–T15 | control exactly which videos run, and see the expected savings first |

## Task List

### Phase 1: Foundation
- [x] T1: CMake skeleton, empty window, startup check for ffmpeg/ffprobe/libx265, fixture script

### Checkpoint 1
- [x] Builds clean with `-Wall -Wextra -Werror`, `ctest` runs (even with 0–1 tests), window opens, missing-ffmpeg dialog shown when PATH is broken

### Phase 2: See your videos
- [x] T2: Folder chooser → list video files with size
- [x] T3: ffprobe each file → show class, codec and duration in the list

### Checkpoint 2
- [x] Fixture folder lists exactly the videos with correct classes (Success Criterion 1)

### Phase 3: Compress (MVP)
- [x] T4: Encode queue: each file → `compressed/.name.part.mp4` → rename, show new size and % saved
- [x] T5: Live per-file progress bar plus "3 / 12" overall count
- [x] T6: Cancel: kill ffmpeg, remove `.part`, stop the queue
- [x] T7: Skip files whose output already exists, and the "not smaller" rule
- [x] T8: Speed dropdown (default `slow`) + Keep subtitles checkbox (→ `.mkv`)

### Checkpoint 3 (MVP)
- [x] Success Criteria 2, 3, 5, 6, 7, 10, 11, 12 pass on fixtures. Review with you before Phase 4 (you approved Phase 4, 2026-11-08).

### Phase 4: Delete originals
- [x] T9: Output verification + `vc_should_trash_original()` decision (pure logic, tested)
- [x] T10: Checkbox, confirmation dialog, `g_file_trash()`, row status

### Checkpoint 4
- [x] Success Criterion 9 passes: originals restorable from Trash, and failures/cancels keep the original

### Phase 5: Harden
- [ ] T11: ASan/UBSan full run, real 200 MB video check, SSIM check, CRF table calibration (all done except the real 200 MB+ phone clip, Criterion 4)

### Checkpoint 5 (Complete)
- [ ] All 13 success criteria in SPEC.md met. Ready for your review. **Open:** 4 (real phone clip) and the iPhone HDR half of 12 (needs your eyes).

### Phase 6: Controls and estimates (added 2026-11-08 at your request)
- [x] T12: Size estimate per row (`vc_estimate_size()`), updates when Speed changes
- [x] T13: Speed guide tooltip (ⓘ + dropdown) from measured preset factors
- [x] T14: Icon buttons: Start / Pause (`SIGSTOP`) / Stop / Move originals to Trash
- [x] T15: Multi-select; buttons act on the selection, or on all videos when nothing is selected

### Checkpoint 6
- [x] ctest green (Debug and ASan builds), Debug build clean
- [ ] Review with you

## Risks and Mitigations
| Risk | Impact | Mitigation |
|---|---|---|
| Async subprocess + GTK wiring is the trickiest part | High | It is built first (T4) as one plain path. Progress and cancel are added on top only after it works. |
| Cancel races: deleting `.part` while ffmpeg still has it open | Med | `g_subprocess_force_exit()`, then delete only in the `wait_async` callback |
| x265 `slow` (the default) on 1080p/4K is slow on this laptop (16 threads, 7 GB RAM): roughly 2× `medium` | Med | One encode at a time. You can pick a faster preset from the Speed dropdown. Measure fps per preset in T11 and report it. |
| `mov_text` subtitles can't be stream-copied into MKV | Low | Convert them to `srt` (T8). Covered by a fixture. |
| Duration unknown (some `.avi`/`.ts` files) → no progress percentage | Low | The progress bar falls back to pulse mode and the encode still runs |
| **iPhone HDR / 10-bit sources** → forcing 8-bit would give washed-out colours | Med | T3 detects bit depth and colour tags. T4 encodes 10-bit sources as `main10` and copies the tags. Dolby Vision metadata is dropped (the base layer is kept). |
| `g_file_trash()` fails (removable drive, no Trash) | Low | Keep the original and show that in the row (already in the spec) |
| CRF table not yet validated on real footage | Med | Calibrated in T11 against SSIM ≥ 0.95 and your eyes. Table changes are an "ask first" item. |

## Open Questions
None. All decisions are recorded in SPEC.md.

## T11 results (2026-11-08)

The app's exact ffprobe → argv path (a throwaway driver linked against `vc_core`), timed on this
laptop. SSIM uses `ffmpeg -i out -i src -lavfi ssim`. fps is decoded frames divided by wall time.

| File | Class | Preset | Before | After | % of source | SSIM | fps |
|---|---|---|---|---|---|---|---|
| `720p.mp4` (fixture) | 720p | slow | 60.9 MB | 1.64 MB | 2.7% | 0.9992 | 48 |
| `720p.mp4` (fixture) | 720p | faster | 60.9 MB | 1.50 MB | 2.5% | 0.9990 | 122 |
| `1080p.mkv` (fixture) | 1080p | slow | 20.2 MB | 0.62 MB | 3.1% | 0.9992 | 22 |
| `1080p.mkv` (fixture) | 1080p | faster | 20.2 MB | 0.58 MB | 2.8% | 0.9990 | 56 |
| real 200 MB+ phone clip | – | slow / faster | – | – | – | – | – |

- Criterion 3 passes (2.7% ≤ 25%, SSIM 0.9992 ≥ 0.95). `testsrc` is far easier to compress than
  camera footage, so these numbers say nothing about the CRF table. No table change is proposed.
- Criterion 4 is still open: there is no phone recording on this machine (`~/Videos/Camera` is empty).
- `slow` is about 2.5× slower than `faster` at both sizes, for about 10% smaller files on `testsrc`.

**Sanitizers (update, after Phase 6):** with `libasan`/`libubsan` installed, a `-fsanitize=address,undefined -fno-sanitize-recover=all` Debug build passed `ctest`. On Xvfb it covered a full fixture run, stop, pause/resume, the Trash button, a stop during the output check, and closing while paused. Every session exited 0 with no ASan/UBSan report (leak checking was off: GTK's one-time allocations drown it). Criterion 13 passes.

**Sanitizers (first pass).** `libasan`/`libubsan` were not installed at first, so ASan could not run.
The stand-in was a `-O1 -D_FORTIFY_SOURCE=3 -fsanitize=undefined -fsanitize-undefined-trap-on-error`
build (UBSan with no runtime library; 86 trap sites in `vc_core`), run under `MALLOC_CHECK_=3
MALLOC_PERTURB_=165 G_DEBUG=fatal-criticals`. `ctest` passes. On Xvfb, a full fixture run at
`faster` with Keep subtitles and Move to Trash on covered the following, and the app exited 0
with nothing on stderr:
- probing, including `fake.mp4`
- a cancel during 1080p, then Start again
- 9 originals trashed and later restored byte-identical; `lowbitrate.mp4` already efficient
- a second Start (skips, plus the confirm dialog's Cancel)
- closing the window

**Bug found and fixed:** closing the window mid-encode crashed with SIGSEGV (exit 139) after the `.part`
was already removed. ffmpeg's late async callbacks ran `finish_job()` on destroyed row widgets.
`on_close` now sets `app.closing`, so the stream, exit, verify and trash callbacks touch no widgets
and start no work. A close during the output check keeps the original and does not trash it. Closing
mid-encode and mid-verify now exits 0 on both the sanitized and the plain Debug build.

### Preset factors (2026-11-08)

Wall time and output size against `medium`, using the app's argv. `noisy1080` is a 20 s 1080p `testsrc2` clip
with temporal noise at 16 Mbps (45.4 MB), a stand-in for phone grain. These factors drive the speed guide and the size estimate
(`PRESET_FACTORS` in `compress.c`).

| Preset | noisy1080 time | noisy1080 size | 720p time | 720p size | Used: time | Used: size |
|---|---|---|---|---|---|---|
| faster | 19.3 s (0.68×) | 9.15 MB (0.99×) | 15.1 s (0.70×) | 1.50 MB (1.00×) | 0.69 | 0.99 |
| fast | 21.0 s (0.74×) | 9.36 MB (1.01×) | 20.7 s (0.95×) | 1.66 MB (1.11×) | 0.85 | 1.06 |
| medium | 28.3 s | 9.28 MB | 21.8 s | 1.50 MB | 1 | 1 |
| slow | 66.4 s (2.35×) | 9.96 MB (1.07×) | 41.0 s (1.89×) | 1.64 MB (1.09×) | 2.1 | 1.08 |
| slower | 278.7 s (9.9×) | 9.94 MB (1.07×) | 142.6 s (6.6×) | 1.56 MB (1.04×) | 8.2 | 1.06 |

At a fixed CRF, slower presets didn't make smaller files on either clip. The SPEC's speed paragraph was corrected to match.
The size estimate's class bitrates (`EST_VIDEO_KBPS`) are scaled from noisy1080 at `medium` (3.7 Mbps → 3.0 Mbps
for 1080p). Recalibrate them once a real phone clip has been measured.
