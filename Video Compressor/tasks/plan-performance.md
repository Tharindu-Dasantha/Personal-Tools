# Performance and Resource Plan: Video Compressor

Spec: [../SPEC.md](../SPEC.md) · Main plan: [plan.md](plan.md) · Written 2026-11-08 from a code review (no code changed yet).

## Goal
Compress a folder in less wall time and with less CPU, heat and desktop lag, without making outputs worse or breaking
any SPEC "Always/Never" rule (argv only, `.part` then rename, never overwrite, trash only after verification).

Targets, measured on the corpus in R0 against today's build:

| Metric | Today (default `slow`) | Target |
|---|---|---|
| Wall time for the corpus | baseline | ≤ 50% with x265, ≤ 15% with the GPU option |
| Desktop lag while encoding | noticeable | none (window drag and typing stay smooth) |
| Time wasted on outputs that get thrown away | full encode per file | ~0 for files predicted "no saving" |
| Output quality (SSIM vs source) | baseline | within 0.005 of baseline for x265; reported (not hidden) for GPU |
| Peak RAM during an encode | baseline | not higher |

## What was measured already (2026-11-08, this laptop: i5-1240P, 16 threads, 7 GB, Iris Xe)
10 s 1080p synthetic clip with heavy noise (worst case, sizes not representative):

| Encoder | Time | Size |
|---|---|---|
| libx265 `slow` (default) | 199 s | 64 MB |
| libx265 `medium` | 79 s | 51 MB |
| `hevc_qsv` (Intel iGPU) | 19 s | 67 MB |
| `hevc_vaapi` | 18 s | 113 MB |
| libsvtav1 preset 8 | 33 s | 106 MB |

Plus the existing preset table in [plan.md](plan.md#preset-factors-2026-11-08): `slow` is 2.1× the time of `medium` and
1.08× the size. `libvmaf` and `ssim` filters are available in the installed ffmpeg.

## Decisions needed from you (SPEC "Ask first")
| # | Decision | Task | Why it needs you |
|---|---|---|---|
| D1 | Default Speed `slow` → `medium` | R3 | SPEC Criterion 11 and plan.md say the default is `slow` |
| D2 | Start skips files predicted "no saving" unless selected | R6 | Changes what Start does |
| D3 | Add an Intel GPU (`hevc_qsv`) encoder option | R9 | Same codec (HEVC), different encoder; treat as "changing the codec" |
| D4 | Keep GPS location and creation date in outputs | R12 | Today location is dropped. Keeping it matters if you share compressed copies |

Not planned, on purpose: parallel encodes (x265 already uses all 16 threads; 7 GB RAM is tight for two 4K encodes),
AV1 (many iPhones can't play it smoothly), GPU decode (`-hwaccel`; decoding is not the bottleneck while x265 runs),
`GtkListView`, worker threads, caching. Revisit only if R0 numbers show a need.

## Order and how it fits Media Tools
```
R0 baseline
 ├── Phase A: quick wins       R1 10-bit fix · R2 low priority · R3 default medium · R4 async tool check
 ├── Phase B: skip waste       R5 codec-aware estimate → R6 skip "no saving" · R7 copy AAC audio
 ├── Phase C: GPU encoder      R8 measure QSV quality → R9 GPU speed option
 ├── Phase D: big folders      R10 parallel probing · R11 thread cap (only if R0/R2 show heat or lag)
 └── Phase E: output fixes     R12 keep date/location · R13 HDR10 headers
```
- Do Phases A, B and E **before T16** (app shell). They are mostly `compress.c` (pure, tested) plus small `main.c` edits.
- T17 rewrites the job runner in `main.c`. R2's launcher and R12's mtime copy must move into that runner, not be lost.
- Phase C can wait until after T20. `web-video` can then reuse the same QSV path with `h264_qsv`.
- Every `main.c` task runs one after another (same file). `compress.c` tasks can go in any order.

Shared commands: see [todo.md](todo.md) (build, test, fixtures, run).

---

## R0: Baseline measurement
**Description:** Fix a corpus and record numbers before any change, so every later task can show its gain.
Corpus in `build/perf/` (never committed): `noisy1080` and 720p fixtures from `make-fixtures.sh`, plus three real clips:
an iPhone HEVC clip, an iPhone HLG HDR clip, an Android H.264 clip (ideally the 200 MB+ clip T11 still needs).
For each file, run the app's exact argv (copy it from a Debug log or `vc_build_ffmpeg_argv`) under `/usr/bin/time -v`.

**Acceptance criteria:**
- [ ] Table in this file with, per clip: wall time, CPU %, max RSS, output size, SSIM vs source
- [ ] Note whether the desktop lags while encoding (drag the window, type in an editor)

**Verification:**
- `/usr/bin/time -v ffmpeg … 2>&1 | grep -E "Elapsed|Percent of CPU|Maximum resident"`
- `ffmpeg -i out.mp4 -i src.mp4 -lavfi ssim -f null - 2>&1 | grep All`

**Dependencies:** None · **Files:** this file · **Scope:** S

---

## Phase A: Quick wins

### R1: Fix 8-bit formats detected as 10-bit
**Description:** `vc_probe_is_10bit()` (`compress.c`) matches any `10`/`12` in the pix_fmt name, so 8-bit `nv12` and
`yuv410p` are encoded as 10-bit `main10`. Match the real 10/12-bit markers instead (`p10`, `p12`, `p010`, `p012`,
`10le`/`12le`, `10be`/`12be`).

**Acceptance criteria:**
- [ ] `nv12`, `yuv410p`, `yuv420p` → 8-bit; `yuv420p10le`, `yuv422p10le`, `p010le`, `yuv420p12le` → 10-bit

**Verification:** new cases in `tests/test_compress.c`; `ctest` green
**Dependencies:** None · **Files:** `src/compress.c`, `tests/test_compress.c` · **Scope:** XS

### R2: Run ffmpeg at low CPU and disk priority
**Description:** Start ffmpeg through a `GSubprocessLauncher` whose child-setup calls `setpriority(PRIO_PROCESS, 0, 10)`
and `syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, idle class)`. When nothing else runs, encode speed is the same.
When you use the laptop, the desktop wins. No new dependency, and the PID stays ffmpeg's, so `SIGSTOP`/`SIGCONT`/kill
keep working. (Fallback if child-setup is awkward: prefix argv with `ionice -c3 nice -n10`. Both exec, so the PID is kept.)
Only the encoder needs this. ffprobe is short.

**Acceptance criteria:**
- [ ] `ps -o ni,cmd -C ffmpeg` shows `10`; `ionice -p <pid>` shows `idle`
- [ ] Pause, resume, Stop and close-mid-encode behave exactly as before
- [ ] Encode time with an idle desktop is within 3% of R0

**Verification:** manual run of the fixture folder; ASan Debug build close-mid-encode exits 0
**Dependencies:** R0 · **Files:** `src/main.c` (`start_job`) · **Scope:** S

### R3: Default Speed `medium` (needs D1)
**Description:** Change `VC_DEFAULT_PRESET` to `"medium"`. First confirm on the R0 corpus that `medium` loses no visible
detail versus `slow`: SSIM difference ≤ 0.005 and VMAF difference ≤ 1 on every clip. If a real clip fails, keep `slow`
and record why here. Update SPEC Criterion 11, plan.md "Confirmed decisions" and the speed guide text.

**Acceptance criteria:**
- [ ] SSIM/VMAF table for `medium` vs `slow` in this file
- [ ] Corpus wall time ≤ 50% of R0

**Verification:** `ffmpeg -i out.mp4 -i src.mp4 -lavfi libvmaf -f null -`; `ctest` green
**Dependencies:** R0, D1 · **Files:** `src/compress.h`, `SPEC.md`, `tasks/plan.md` · **Scope:** XS

### R4: Tool check without blocking the window
**Description:** `check_tools()` runs `ffmpeg -encoders` with `g_spawn_sync` right after the window is shown, which freezes
it for about 0.2 s. Run it with `g_subprocess_communicate_utf8_async` and keep Choose Folder insensitive until it
passes. R9 extends this same check.

**Acceptance criteria:**
- [ ] Window paints and can be dragged immediately at startup
- [ ] `PATH=/nonexistent` still shows the error dialog and quits cleanly

**Verification:** manual launch, normal and with a broken PATH
**Dependencies:** None · **Files:** `src/main.c` · **Scope:** S

### Checkpoint A
- [ ] `ctest` green, ASan full fixture run clean, R0 table re-measured: time ≤ 50% (with D1), no desktop lag

---

## Phase B: Skip wasted work

### R5: Codec-aware size estimate, calibrated on real clips
**Description:** `vc_estimate_size()` caps the video at 60% of the source bitrate for every codec. For a source that is
already HEVC/AV1/VP9 that is optimistic, so a lean iPhone clip never shows "≈ no saving" and gets a full encode that is
then thrown away. Use a per-codec factor (H.264/MPEG-2/MPEG-4 ≈ 0.6, HEVC/VP9/AV1 ≈ 1.0) and recalibrate
`EST_VIDEO_KBPS` from the R0 real clips (this also finishes the open T11 calibration note).
`shortcut:` keep the factor table in `compress.c` next to `PRESET_FACTORS`. Upgrade to a 10 s sample encode only if
estimates are still off by more than 25% on real clips.

**Acceptance criteria:**
- [ ] For every R0 real clip, estimate vs actual is within 25%
- [ ] A lean HEVC source predicts "no saving"; a high-bitrate H.264 source still predicts a saving

**Verification:** table in this file; new `vc_estimate_size` cases in `tests/test_compress.c`
**Dependencies:** R0 · **Files:** `src/compress.c`, `tests/test_compress.c` · **Scope:** S

### R6: Start skips "no saving" files unless selected (needs D2)
**Description:** Add `vc_worth_encoding(probe, size, preset)` (estimate known and passes `vc_output_is_worth_keeping`).
Start with nothing selected leaves those rows as `Likely no saving – select to compress anyway` instead of queuing them.
Selecting a row and pressing Start encodes it as today. Unknown duration (estimate 0) is always encoded.

**Acceptance criteria:**
- [ ] Bulk Start skips predicted-no-saving rows, and their status says why
- [ ] Selected rows always encode
- [ ] The Trash confirmation count does not include skipped rows

**Verification:** `vc_worth_encoding` unit tests; manual run on a folder with one lean HEVC clip
**Dependencies:** R5, D2 · **Files:** `src/compress.c/.h`, `src/main.c` (`can_start`, `on_start`, `do_start`), tests · **Scope:** S

### R7: Copy AAC audio instead of re-encoding it
**Description:** Probe audio streams too (`codec_name`, `bit_rate`). When **every** audio stream is AAC at ≤ the class's
`audio_kbps` + 10%, use `-c:a copy`. Otherwise keep today's `-c:a aac -b:a`. Saves a little CPU and avoids a second
lossy pass. `VcProbe` gains `gboolean audio_copyable`. The parser already sees every stream in `commit_stream`.

**Acceptance criteria:**
- [ ] 128k AAC source → `-c:a copy`; 256k AAC, Opus, AC-3 or mixed → re-encode
- [ ] Output from a copied-audio file passes the R0 duration check (verify step unchanged)

**Verification:** parser + argv unit tests with captured ffprobe text; manual fixture run
**Dependencies:** None · **Files:** `src/compress.c/.h`, `tests/test_compress.c`, `tests/make-fixtures.sh` (one AAC 128k fixture) · **Scope:** M

### Checkpoint B
- [ ] `ctest` green; a folder of lean HEVC clips finishes in seconds instead of hours, with each row explained

---

## Phase C: Intel GPU encoder (needs D3)

### R8: Measure `hevc_qsv` quality before building anything
**Description:** On the R0 corpus, encode with
`-c:v hevc_qsv -preset slow -global_quality N` for N in 22–30. For each class, pick the N whose VMAF matches x265
`medium` at that class's CRF, and record size and time. If no N gets within 2 VMAF at ≤ 1.4× the x265 size, stop and
record "not worth it" here.

**Acceptance criteria:**
- [ ] Table per class: chosen N, VMAF, size vs x265, time vs x265
- [ ] Go / no-go written here

**Verification:** `libvmaf` runs as in R3
**Dependencies:** R0, D3 · **Files:** this file · **Scope:** S

### R9: "Intel GPU (fast)" Speed option
**Description:** A new last entry in the Speed dropdown. `vc_build_ffmpeg_argv` gets an encoder branch:
`-c:v hevc_qsv -preset slow -global_quality <N from R8>`, `-pix_fmt nv12` (8-bit) or `p010le` + `-profile:v main10`
(10-bit), colour tags, `-tag:v hvc1` and `+faststart` as today. The startup check (R4) does a 1-frame test encode
(`-f lavfi -i color -frames:v 1 -c:v hevc_qsv -f null -`). The encoder list shows QSV even without working hardware,
so the list alone isn't enough. The option is hidden when the test fails. Add its time/size factors to the speed guide and estimate.

**Acceptance criteria:**
- [ ] 8-bit and 10-bit HLG fixtures encode, play in mpv, and keep `hvc1` and their colour tags
- [ ] Option absent when `/dev/dri/renderD128` is unavailable (test by running inside `bwrap` without `/dev/dri`)
- [ ] Corpus wall time ≤ 15% of R0; ffmpeg CPU % far below x265

**Verification:** argv unit tests; manual fixture run; `ffprobe` of outputs
**Dependencies:** R4, R8 · **Files:** `src/compress.c/.h`, `src/main.c`, tests · **Scope:** M

---

## Phase D: Large folders

### R10: Probe up to 4 files at once
**Description:** `probe_step` runs one ffprobe at a time, so 300 files show "Reading…" for 15–30 s. Keep up to 4 in
flight. Pass the `Item *` as `user_data`, because `probe_done` can no longer use `app.probe_next` as the index. Keep the
cancelled-callback early return before any `Item` access. Count completions, and call `update_speed_guide()` +
`update_controls()` when all are done. `probing` in `update_controls` uses the completion count.

**Acceptance criteria:**
- [ ] 300-file folder (symlink one fixture 300 times with video names) lists in ≤ 30% of the old time
- [ ] Choosing a new folder mid-probe still never touches freed items (ASan clean)

**Verification:** manual timing; ASan Debug build, switch folders mid-probe twice
**Dependencies:** None · **Files:** `src/main.c` · **Scope:** S

### R11: Cap x265 threads (only if needed)
**Description:** Only if, after R2, R0 shows thermal throttling or fan noise you dislike: add `pools=12` to
`-x265-params`. Measure the time cost first and record it. Skip this task otherwise.

**Dependencies:** R2 · **Files:** `src/compress.c` · **Scope:** XS

---

## Phase E: Output fixes

### R12: Keep the original's date, and its location if you agree (needs D4)
**Description:** After the `.part` is moved into place in `finish_job`, copy `time::modified` from the source onto the
output (`g_file_query_info` + `g_file_set_attributes_from_info`), so galleries don't sort compressed copies as "today".
With D4 = yes, MP4 output uses `-movflags +faststart+use_metadata_tags`, which keeps iPhone `com.apple.quicktime.*` keys
(location, creation date, camera). With D4 = no, date only.

**Acceptance criteria:**
- [ ] `stat -c %y` of the output equals the original
- [ ] D4 = yes: `ffprobe -show_format` of an iPhone output lists `com.apple.quicktime.location.ISO6709`
- [ ] Verification and trash behave as before (the date copy happens before `check_then_trash`)

**Verification:** manual run on a real iPhone clip; `ctest` green
**Dependencies:** D4 · **Files:** `src/main.c`, `src/compress.c` (movflags) · **Scope:** S

### R13: HDR10 (PQ) headers for x265
**Description:** For `smpte2084` sources, add `hdr10=1:repeat-headers=1` to the existing `-x265-params` string (joined
with `:`), so players switch to HDR. HLG (iPhone) is unchanged.
`shortcut:` no mastering-display / MaxCLL passthrough. Add it if a real HDR10 file looks wrong in mpv.

**Acceptance criteria:**
- [ ] PQ fixture argv has `log-level=error:hdr10=1:repeat-headers=1`; HLG and 8-bit argv are unchanged

**Verification:** argv unit test; reuse the `hdr10-pq.mov` fixture planned in T19 (create it here if T19 hasn't run)
**Dependencies:** None · **Files:** `src/compress.c`, `tests/test_compress.c`, `tests/make-fixtures.sh` · **Scope:** XS

### Checkpoint E (plan complete)
- [ ] `ctest` green, ASan full fixture run clean, R0 table re-measured and every target in "Goal" met or explained here

---

## Risks
| Risk | Impact | Mitigation |
|---|---|---|
| `medium` loses detail on grainy phone footage | Med | R3 is gated on SSIM/VMAF from real clips; keep `slow` if it fails |
| A wrong estimate skips a file that would have shrunk | Med | R5 calibrates on real clips first; R6 never skips selected rows or unknown durations |
| QSV quality varies with driver versions | Med | R8 measures before R9; the startup test encode hides the option if the GPU is unavailable |
| T17 rewrites the runner and drops R2/R12 | Med | Listed under "Order"; add R2/R12 behaviour to T17's acceptance criteria |
| Keeping GPS in shared copies | Low | D4 is your explicit choice |
