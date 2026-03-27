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
                     │                 └── hardening + calibration (T11)
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

## Task List

### Phase 1: Foundation
- [x] T1: CMake skeleton, empty window, startup check for ffmpeg/ffprobe/libx265, fixture script

### Checkpoint 1
- [x] Builds clean with `-Wall -Wextra -Werror`, `ctest` runs (even with 0–1 tests), window opens, missing-ffmpeg dialog shown when PATH is broken

### Phase 2: See your videos
- [ ] T2: Folder chooser → list video files with size
- [ ] T3: ffprobe each file → show class, codec and duration in the list

### Checkpoint 2
- [ ] Fixture folder lists exactly the videos with correct classes (Success Criterion 1)

### Phase 3: Compress (MVP)
- [ ] T4: Encode queue: each file → `compressed/.name.part.mp4` → rename, show new size and % saved
- [ ] T5: Live per-file progress bar plus "3 / 12" overall count
- [ ] T6: Cancel: kill ffmpeg, remove `.part`, stop the queue
- [ ] T7: Skip files whose output already exists, and the "not smaller" rule
- [ ] T8: Speed dropdown (default `slow`) + Keep subtitles checkbox (→ `.mkv`)

### Checkpoint 3 (MVP)
- [ ] Success Criteria 2, 3, 5, 6, 7, 10, 11, 12 pass on fixtures. Review with you before Phase 4.

### Phase 4: Delete originals
- [ ] T9: Output verification + `vc_should_trash_original()` decision (pure logic, tested)
- [ ] T10: Checkbox, confirmation dialog, `g_file_trash()`, row status

### Checkpoint 4
- [ ] Success Criterion 9 passes: originals restorable from Trash, and failures/cancels keep the original

### Phase 5: Harden
- [ ] T11: ASan/UBSan full run, real 200 MB video check, SSIM check, CRF table calibration

### Checkpoint 5 (Complete)
- [ ] All 13 success criteria in SPEC.md met. Ready for your review.

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
