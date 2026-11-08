# Todo: Performance and resources

Plan: [plan-performance.md](plan-performance.md) · Main todo: [todo.md](todo.md) · Spec: [../SPEC.md](../SPEC.md)

Shared commands (build, test, fixtures, run) are in [todo.md](todo.md). Each task's full description, acceptance
criteria and verification are in the plan under the same ID. This list is the order to work in and the steps.

Order: decisions → R0 → Phase A → B → E (all before T16) → C → D.
If speed matters most, do Phase C right after R0: it is the only phase that turns hours into minutes.
Tasks that edit `main.c` run one after another. `compress.c` tasks can go in any order.

---

## Decisions (ask before the task that needs them)
- [ ] D1: Default Speed `slow` → `medium` (blocks R3)
- [ ] D2: Start skips files predicted "no saving" unless selected (blocks R6)
- [ ] D3: Add the Intel GPU encoder option (blocks R8, R9)
- [ ] D4: Keep GPS location and creation date in outputs (R12: date only if no)
- [ ] D5: "Max frame rate: 30" option (blocks R14)
- [ ] D6: "Max size: 1080p" option (blocks R15)
- [ ] Record each answer in the plan's Decisions table

## Before T17
- [ ] Add to T17's acceptance criteria in [todo.md](todo.md) and [plan.md](plan.md): ffmpeg still starts at low
      priority (R2), and the output still gets the source's mtime (R12)

---

## - [ ] R0: Baseline
- [ ] Build `build/perf/` (not committed): `noisy1080` + 720p fixtures, iPhone HEVC, iPhone HLG HDR, Android H.264 (200 MB+)
- [ ] Copy the app's exact argv for each clip (Debug log or `vc_build_ffmpeg_argv`)
- [ ] Run each under `/usr/bin/time -v`; record wall time, CPU %, max RSS, output size
- [ ] SSIM of each output vs source
- [ ] Note desktop lag while encoding (drag the window, type in an editor)
- [ ] Results table in plan-performance.md

## Phase A: Quick wins

### - [ ] R1: 8-bit formats detected as 10-bit
- [ ] Tests first in `tests/test_compress.c`: `nv12`, `yuv410p`, `yuv420p` → 8-bit; `yuv420p10le`, `yuv422p10le`,
      `p010le`, `yuv420p12le` → 10-bit
- [ ] Fix `vc_probe_is_10bit()` ([compress.c:200](../src/compress.c#L200)) to match `p10`/`p12`/`p010`/`p012`/`10le`/`12le`/`10be`/`12be`
- [ ] Check the other caller still reads right ([main.c:385](../src/main.c#L385))
- [ ] `ctest` green

### - [ ] R2: ffmpeg at low CPU and disk priority
- [ ] In `start_job` ([main.c:1109](../src/main.c#L1109)), spawn through a `GSubprocessLauncher` whose child-setup calls
      `setpriority(…, 10)` and `ioprio_set` idle (fallback: prefix argv with `ionice -c3 nice -n10`)
- [ ] Encoder only, not ffprobe
- [ ] `ps -o ni,cmd -C ffmpeg` shows 10; `ionice -p <pid>` shows idle
- [ ] Pause, resume, Stop, close mid-encode unchanged (ASan build exits 0)
- [ ] Idle-desktop encode time within 3% of R0

### - [ ] R3: Default Speed `medium` (needs D1)
- [ ] On the R0 corpus: `medium` vs `slow` SSIM ≤ 0.005 and VMAF ≤ 1 apart on every clip; table in the plan
- [ ] If a real clip fails: keep `slow`, write why in the plan, stop here
- [ ] `VC_DEFAULT_PRESET` → `"medium"` ([compress.h:27](../src/compress.h#L27))
- [ ] Update SPEC Criterion 11, plan.md "Confirmed decisions", speed guide text
- [ ] `ctest` green; corpus wall time ≤ 50% of R0

### - [ ] R4: Tool check without blocking the window
- [ ] `check_tools()`: `g_spawn_sync` → `g_subprocess_communicate_utf8_async`
- [ ] Choose Folder insensitive until the check passes
- [ ] Window paints and drags at once; `PATH=/nonexistent` still shows the error and quits cleanly

### - [ ] Checkpoint A
- [ ] `ctest` green, ASan full fixture run clean
- [ ] R0 re-measured: time ≤ 50% (with D1), no desktop lag

## Phase B: Skip wasted work

### - [ ] R5: Codec-aware size estimate
- [ ] Per-codec factor table next to `PRESET_FACTORS` in `compress.c`: H.264/MPEG-2/MPEG-4 ≈ 0.6, HEVC/VP9/AV1 ≈ 1.0
- [ ] Use it in `vc_estimate_size()` instead of the fixed `0.6` ([compress.c:346](../src/compress.c#L346))
- [ ] Recalibrate `EST_VIDEO_KBPS` from the R0 real clips (closes the T11 calibration note)
- [ ] Tests: lean HEVC → "no saving"; high-bitrate H.264 → saving
- [ ] Estimate vs actual within 25% for every R0 real clip; table in the plan

### - [ ] R6: Start skips "no saving" files (needs D2)
- [ ] `vc_worth_encoding(probe, size, preset)` in `compress.c/.h` + unit tests (unknown duration → always encode)
- [ ] `can_start` / `on_start` / `do_start`: bulk Start leaves those rows as
      `Likely no saving – select to compress anyway`
- [ ] Selected rows always encode
- [ ] Trash confirmation count excludes skipped rows
- [ ] Manual run on a folder with one lean HEVC clip

### - [ ] R7: Copy AAC audio
- [ ] Fixture: one AAC 128k clip in `tests/make-fixtures.sh`
- [ ] Parser: read audio `codec_name` + `bit_rate` in `commit_stream`; `VcProbe.audio_copyable`
- [ ] Argv: every audio stream AAC ≤ class `audio_kbps` + 10% → `-c:a copy`, else today's `-c:a aac -b:a`
- [ ] Tests: 128k AAC → copy; 256k AAC, Opus, AC-3, mixed → re-encode
- [ ] Copied-audio output passes the duration check

### - [ ] Checkpoint B
- [ ] `ctest` green; a folder of lean HEVC clips finishes in seconds, each row explained

## Phase E: Output fixes

### - [ ] R12: Keep date (and location with D4)
- [ ] In `finish_job` ([main.c:1001](../src/main.c#L1001)), after the `.part` rename and before `check_then_trash`: copy
      `time::modified` from source to output
- [ ] D4 = yes: MP4 movflags `+faststart+use_metadata_tags` ([compress.c:272](../src/compress.c#L272)) + argv test
- [ ] `stat -c %y` output == original; D4 = yes: iPhone output lists `com.apple.quicktime.location.ISO6709`
- [ ] Verification and trash unchanged

### - [ ] R13: HDR10 (PQ) headers
- [ ] `hdr10-pq.mov` fixture (if T19 hasn't made it)
- [ ] `smpte2084` source → `-x265-params log-level=error:hdr10=1:repeat-headers=1` ([compress.c:252](../src/compress.c#L252))
- [ ] Argv tests: PQ gets it; HLG and 8-bit unchanged

## Phase C: Intel GPU encoder (needs D3)

### - [ ] R8: Measure the GPU on real clips (no code)
- [ ] R0 corpus with the VA-API `low_power` argv from the plan, QP 26–34
- [ ] Per class: QP matching x265 `medium` VMAF, size and fps vs x265
- [ ] 10-bit HLG → `main10` with colour tags? If not, 10-bit stays on x265
- [ ] CPU-decode fallback with an MPEG-4 `.avi`
- [ ] Go / no-go in the plan

### - [ ] R9: "GPU – fastest" Speed option (only if R8 = go)
- [ ] `VcClass.gpu_qp` from R8; `hevc_vaapi` branch in `vc_build_ffmpeg_argv` keeping colour tags, `hvc1`, `+faststart`
      (and `main10` if R8 passed)
- [ ] Argv unit tests
- [ ] Startup check (R4) runs the 1-frame test encode; option hidden if it fails
- [ ] Time/size factors in the speed guide and estimate
- [ ] 8-bit, portrait, 10-bit HLG outputs play in mpv and iOS with `hvc1` + colour tags
- [ ] Option absent under `bwrap` without `/dev/dri`
- [ ] Pause, Stop, close mid-encode work; corpus wall time ≤ 15% of R0

### - [ ] R14: "Max frame rate: 30" (needs D5)
- [ ] `VcProbe.fps` from `avg_frame_rate` (shared with T21; do it once)
- [ ] Checkbox, off by default; source > 30 fps → `-fpsmax 30`
- [ ] Argv tests; 60 fps → 30 fps, same duration ±1 s; 24/30 fps unchanged; time ≈ half

### - [ ] R15: "Max size: 1080p" (needs D6)
- [ ] Dropdown `Original / 1080p / 720p`, default `Original`; downscale only, by the short side
- [ ] CPU `scale` with `-2`; GPU `scale_vaapi`; class chosen from the output size
- [ ] Argv tests incl. portrait and odd sizes → even dimensions
- [ ] 4K → 1080p, 2160×3840 → 1080×1920, 720p unchanged; time ≈ a quarter
- [ ] Update SPEC (reverses "no downscale option", as opt-in)

## Phase D: Large folders

### - [ ] R10: Probe up to 4 files at once
- [ ] `probe_step` / `probe_done` ([main.c:707-749](../src/main.c#L707-L749)): 4 in flight, `Item *` as `user_data`,
      cancelled early return before any `Item` access
- [ ] Count completions; `update_speed_guide()` + `update_controls()` when all done
- [ ] 300-file folder lists in ≤ 30% of the old time
- [ ] ASan: switch folders mid-probe twice, clean

### - [ ] R11: Cap x265 threads (only if R0/R2 show heat or lag)
- [ ] Measure the time cost of `pools=12`, record it, then decide

---

### - [ ] Checkpoint E (plan complete)
- [ ] `ctest` green, ASan full fixture run clean
- [ ] R0 table re-measured; every target in the plan's "Goal" met or explained
