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

**Second run, closer to phone footage** (10 s 1080p60, light grain, 16 Mbps H.264, 600 frames):

| Pipeline | fps | Size | SSIM |
|---|---|---|---|
| x265 `medium` CRF 27 (the app today) | 20–28 | 6.9 MB | 0.9870 |
| x264 `fast` RF 22 (HandBrake-like) | 47 | 13.9 MB | — |
| `hevc_qsv`, default mode (CPU or GPU decode) | 9–19 | 18.5 MB | — |
| **`hevc_vaapi -low_power 1`, GPU decode, QP 30** | **261** | 10.4 MB | **0.9873** |
| `hevc_vaapi -low_power 1`, GPU decode, QP 32 | 265 | 7.0 MB | 0.9846 |

The GPU's dedicated encoder block (`low_power`/VDENC), with decoding also on the GPU, is about 10× faster than x265 at
the same SSIM, and about 1.5× the size. `hevc_qsv` in its default mode is *slower* than x265, and `-low_power` with
`global_quality` is rejected, so R8/R9 use VA-API.

**Why HandBrake takes minutes:** its logs (`~/.var/app/fr.handbrake.ghb/config/ghb/EncodeLogs`) show **H.264 (x264)
`veryfast`/`fast`**, output **downscaled to 720p/1080p** and **capped at 24–30 fps**, at 60–450 fps. The app does
H.265 at full resolution and full frame rate. Nearly all of the time is spent inside ffmpeg. The app's own code
costs almost nothing.

## Results (2026-11-08/09, real clips)
Corpus in `build/perf/src/`: symlinks to `~/Downloads` plus one cut, replayed with the app's argv by
`build/perf/perf.sh` (not committed). Chrome was using 1–2 cores during some runs, so times are ±5%.
`fixture-720p` is synthetic `testsrc` and compresses unrealistically well; ignore its sizes.

**R0 baseline, x265 `slow`** · **R3 `medium`** · **R8 GPU `hevc_vaapi` QP 28** (time s · size MB · VMAF · SSIM):

| Clip | Source | `slow` (R0) | `medium` | GPU QP 28 |
|---|---|---|---|---|
| fixture-720p (60 s) | 60.9 MB | 35.1 · 1.64 · 97.6 · .9992 | 21.9 · 1.50 · 97.5 · .9990 | 3.1 · 1.88 · 97.2 · .9985 |
| h264-1080-17m (Kne, 21 s) | 47.3 MB | 91.3 · 12.0 · 94.5 · .9845 | 35.6 · 11.2 · 92.1 · .9820 | 2.3 · 15.8 · 93.3 · .9837 |
| h264-720-9m (finalFinal, 33 s) | 37.2 MB | 77.2 · 10.6 · 96.4 · .9817 | 29.1 · 9.4 · 93.7 · .9779 | 1.9 · 10.0 · 94.4 · .9746 |
| hevc-portrait (Transport…, 33 s) | 30.1 MB | 107.2 · 7.6 · 95.1 · .9916 | 46.4 · 7.0 · 93.5 · .9899 | 3.3 · 7.4 · 93.9 · .9888 |
| iphone-hlg (IMG_1211, 26 s) | 30.5 MB | 89.3 · 4.6 · 94.8 · .9880 | 37.8 · 3.8 · 92.8 · .9863 | 2.9 · 4.1 · 92.6 · .9816 |
| screen-1080p60 (60 s cut) | 3.5 MB | 167.6 · 2.2 · 96.0 · .9970 | 87.6 · 2.2 · 95.7 · .9969 | 11.0 · 2.5 · 95.3 · .9961 |
| **Corpus wall time** | | **567.7 s** | 258.4 s (46%) | **24.5 s (4.3%)** |

- CPU: x265 870–1290% (all cores), GPU 33–58%. Peak RSS: `slow` 561–1316 MB, `medium` 537–1242 MB, GPU 126–204 MB.
- **R3 fails its gate → default stays `slow`.** `medium` is 1.6–2.7 VMAF below `slow` on 4 of 6 clips (limit 1.0);
  SSIM is within 0.004 everywhere. Part of the drop is that `medium` files are 7–18% smaller at the same CRF.
- **R8 go.** QP 28 matches `medium`'s VMAF for 720p and 1080p (4 of 5 real clips equal or better, the 5th 0.3 lower),
  at 1.06–1.41× `medium`'s size (median ≈ 1.1) and 6–14% of its time. HLG comes out `Main 10`, `hvc1`, with all three
  colour tags. VP8 (not GPU-decodable) falls back to CPU decode and works. The full sweep (QP 26–32) is in
  `build/perf/r8-gpu.csv`; QP 26 ≈ `slow`'s VMAF at 1.2–1.8× its size.
- **R2:** idle-desktop encode (720p clip, `medium`, two runs each): 33.1 s normal, 33.3 s at nice 10 + idle I/O (+0.5%).
- **R14:** 60 fps clip with the cap: x265 56.4 → 30.7 s (54%), GPU 11.3 → 6.7 s; 30 fps out, duration 60.00 → 60.07 s.
- **R15:** 4K → 1080p on x265 `faster`: 31.8 → 10.3 s (32%; 4K decode keeps it above a quarter).
- **R10:** 300-file folder, old one-at-a-time build (`a78e643`) 39.2 s; 4 at once 18.8 s (48%); 8 at once 15.3 s (39%).
  The ≤ 30% target is not met: each ffprobe costs about 90 ms of CPU, mostly loading its libraries, and turning row
  updates off did not help. Kept at 4.
- **R5 estimate vs actual** (after the fixes below): `slow` h264-1080 −27%, h264-720 −28%, hevc-portrait +81%,
  iphone-hlg +127%, screen +17%; GPU lecture clip +5%. Lean files are now close, but **no per-class constant can be
  within 25%**: real 1080p output ranges from 1.3 to 4.5 Mbps depending on content. Per this task's own rule, the next
  step is a sample encode (on the GPU, about 1 s per file). That is a new behaviour, so it waits for your go-ahead.

**Bugs found by these measurements, all fixed:**
- GPU outputs of phone portrait clips (stored landscape + rotation tag) came out sideways: ffmpeg neither rotates GPU
  frames nor keeps the tag. Fixed with `-noautorotate` + `transpose_vaapi` from the probed rotation.
- ffmpeg 8 drops the source's `creation_time` unless metadata is mapped: every output now gets `-map_metadata 0`.
- The estimate's lean-source cap used the whole file's bitrate and then added audio again, so lean recordings with
  rich audio showed "no saving" (a lecture clip: predicted 0%, real −38%). It now uses the video stream's own bitrate.
- The GPU size factor guessed from one synthetic clip (1.5×) made most GPU estimates "no saving"; measured 1.15×.

## Decisions (SPEC "Ask first"), all answered yes on 2026-11-08
| # | Decision | Task | Why it needs you | Answer |
|---|---|---|---|---|
| D1 | Default Speed `slow` → `medium` | R3 | SPEC Criterion 11 and plan.md say the default is `slow` | Yes, gated on R3, which **failed: stays `slow`** (see Results) |
| D2 | Start skips files predicted "no saving" unless selected | R6 | Changes what Start does | Yes |
| D3 | Add an Intel GPU encoder option (VA-API, see R8) | R9 | Same codec (HEVC), different encoder; treat as "changing the codec" | Yes, gated on R8, which **passed** |
| D4 | Keep GPS location and creation date in outputs | R12 | Today location is dropped. Keeping it matters if you share compressed copies | Yes: date and location |
| D5 | Optional "Max frame rate: 30" (60 fps clips do half the work) | R14 | New option; SPEC has no frame-rate rule | Yes (opt-in, off by default) |
| D6 | Optional "Max size: 1080p" (4K does a quarter of the work) | R15 | SPEC resolved "no downscale option"; this reverses it, as an opt-in | Yes (opt-in, default `Original`) |

Not planned, on purpose: parallel encodes (x265 already uses all 16 threads; 7 GB RAM is tight for two 4K encodes),
AV1 (many iPhones can't play it smoothly), GPU decode (`-hwaccel`; decoding is not the bottleneck while x265 runs),
`GtkListView`, worker threads, caching. Revisit only if R0 numbers show a need.

## Order and how it fits Media Tools
```
R0 baseline
 ├── Phase A: quick wins       R1 10-bit fix · R2 low priority · R3 default medium · R4 async tool check
 ├── Phase B: skip waste       R5 codec-aware estimate → R6 skip "no saving" · R7 copy AAC audio
 ├── Phase C: GPU encoder      R8 measure GPU on real clips → R9 GPU speed option · R14 30 fps cap · R15 1080p cap
 ├── Phase D: big folders      R10 parallel probing · R11 thread cap (only if R0/R2 show heat or lag)
 └── Phase E: output fixes     R12 keep date/location · R13 HDR10 headers
```
- Do Phases A, B and E **before T16** (app shell). They are mostly `compress.c` (pure, tested) plus small `main.c` edits.
- T17 rewrites the job runner in `main.c`. R2's launcher and R12's mtime copy must move into that runner, not be lost.
- **If speed is the goal, do Phase C first.** It is the only change that turns hours into minutes: about 10× from the
  GPU, then 2× from the 30 fps cap and 4× from the 1080p cap, for the files where those apply.
  `web-video` can later reuse the same path with `h264_vaapi`.
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

### R8: Measure the GPU encoder on real clips before building anything
**Description:** On the R0 corpus, encode with
`-hwaccel vaapi -hwaccel_output_format vaapi -i IN -vf "format=nv12|vaapi,hwupload" -c:v hevc_vaapi -low_power 1 -rc_mode CQP -qp N`
for N in 26–34. The `format=…,hwupload` filter keeps it working when a codec can't be decoded on the GPU (ffmpeg then
decodes on the CPU and uploads). For each class, pick the QP whose VMAF matches x265 `medium` at that class's CRF, and
record size and fps. Also check that a 10-bit HLG clip comes out `main10` with its colour tags. If `low_power` main10
fails, record it here: 10-bit sources then stay on x265. Synthetic result so far: QP 30 matches x265 SSIM at 10× the speed.

**Acceptance criteria:**
- [ ] Table per class: chosen QP, VMAF, size vs x265, fps vs x265
- [ ] 10-bit / HLG result and decode fallback (e.g. an MPEG-4 `.avi`) recorded
- [ ] Go / no-go written here

**Verification:** `libvmaf` runs as in R3
**Dependencies:** R0, D3 · **Files:** this file · **Scope:** S

### R9: "GPU – fastest" Speed option
**Description:** A new first entry in the Speed dropdown. `VcClass` gains a `gpu_qp` column (from R8), and
`vc_build_ffmpeg_argv` gets an encoder branch with the R8 argv. It keeps colour tags (`-color_*` flags), `-tag:v hvc1`
and `+faststart` as today, and adds `-profile:v main10` for 10-bit if R8 passed. The startup check (R4) does a 1-frame
test encode (`-f lavfi -i color=s=256x256 -frames:v 1 -vf format=nv12,hwupload -c:v hevc_vaapi -low_power 1 -f null -`
with `-init_hw_device vaapi`). The encoder list shows hardware encoders even when they don't work, so the list alone isn't
enough. The option is hidden when the test fails. Add its time/size factors to the speed guide and estimate.
`shortcut:` uses the first render node `/dev/dri/renderD128`. Pick the device another way only on a multi-GPU machine.

**Acceptance criteria:**
- [ ] 8-bit, portrait and 10-bit HLG fixtures encode, play in mpv and QuickTime/iOS, and keep `hvc1` and colour tags
- [ ] Option absent when `/dev/dri/renderD128` is unavailable (test by running inside `bwrap` without `/dev/dri`)
- [ ] Corpus wall time ≤ 15% of R0; ffmpeg CPU % far below x265
- [ ] Pause (SIGSTOP), Stop and close-mid-encode still work (no stuck GPU state after SIGKILL)

**Verification:** argv unit tests; manual fixture run; `ffprobe` of outputs
**Dependencies:** R4, R8 · **Files:** `src/compress.c/.h`, `src/main.c`, tests · **Scope:** M

### R14: "Max frame rate: 30" option (needs D5)
**Description:** Checkbox, off by default. When on and the source is above 30 fps, add `-fpsmax 30` (works for both
CPU and GPU paths). Phone clips are often 60 fps, so this halves the frames to encode. Needs `VcProbe.fps`, which T21
also adds from `avg_frame_rate`: do it once and share it. Verification compares durations, so it still passes.

**Acceptance criteria:**
- [ ] 60 fps source → 30 fps output with the same duration (±1 s); 24/30 fps sources unchanged
- [ ] Encode time for a 60 fps clip ≈ half

**Verification:** argv unit tests; `ffprobe -show_entries stream=avg_frame_rate` on output
**Dependencies:** D5 · **Files:** `src/compress.c/.h`, `src/main.c`, tests · **Scope:** S

### R15: "Max size: 1080p" option (needs D6)
**Description:** Dropdown `Original / 1080p / 720p`, default `Original`. Downscale only, never upscale (SPEC "Never").
Scale by the short side so portrait works: CPU path `scale` with `-2` for the long side, GPU path `scale_vaapi`
(`w=-2:h=1080` or the reverse for portrait). The class (CRF/QP/audio) is chosen from the **output** size. Reuse the
"Max width" idea from [SPEC-web-video.md](../SPEC-web-video.md) so both modes behave the same.

**Acceptance criteria:**
- [ ] 4K → 1080p, portrait 2160×3840 → 1080×1920, 720p source unchanged
- [ ] Encode time for a 4K clip ≈ a quarter

**Verification:** argv unit tests (including odd sizes → even dimensions); `ffprobe` on outputs
**Dependencies:** D6 · **Files:** `src/compress.c/.h`, `src/main.c`, tests, `SPEC.md` · **Scope:** M

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
| GPU quality or support varies with driver versions | Med | R8 measures before R9; the startup test encode hides the option if the GPU is unavailable |
| GPU files are ~1.5× the size of x265 at the same quality | Low | Still far smaller than phone originals; x265 stays in the dropdown for maximum savings |
| T17 rewrites the runner and drops R2/R12 | Med | Listed under "Order"; add R2/R12 behaviour to T17's acceptance criteria |
| Keeping GPS in shared copies | Low | D4 is your explicit choice |
