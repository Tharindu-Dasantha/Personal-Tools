# Todo: Performance and resources

Plan: [plan-performance.md](plan-performance.md) · Main todo: [todo.md](todo.md) · Spec: [../SPEC.md](../SPEC.md)

Shared commands (build, test, fixtures, run) are in [todo.md](todo.md). Each task's full description, acceptance
criteria and verification are in the plan under the same ID. This list is the order to work in and the steps.

Order: decisions → R0 → Phase A → B → E (all before T16) → C → D.
If speed matters most, do Phase C right after R0: it is the only phase that turns hours into minutes.
Tasks that edit `main.c` run one after another. `compress.c` tasks can go in any order.

---

## Status (2026-11-09)
- **Done and verified:** every task except the items marked "needs you" below, R5's ±25% target and R10's 30%
  target (both explained in the plan's Results), and R11 (not needed: no lag or heat was reported, and R2's priority
  costs 0.5%).
- **Results are in [plan-performance.md](plan-performance.md#results-2026-110809-real-clips):** corpus wall time is 46%
  of R0 with `medium` (not adopted) and **4.3% with the GPU**.
- **Found and fixed while verifying:** GPU outputs of rotated phone clips came out sideways; ffmpeg 8 dropped
  `creation_time`; the estimate counted audio twice (lean recordings showed "no saving"); the guessed GPU size factor
  (1.5×) made most GPU estimates "no saving".
- **Checked in the app** (Xvfb, ASan build, no ASan reports): priority, pause/resume/stop, close mid-encode, skip then
  select, mtime, the GPU batch, Max 720p, broken PATH, GPU hidden without `/dev/dri`, and switching folders mid-probe.
- **Needs you:**
  - whether the desktop lags while encoding (R0, Checkpoint A)
  - playing GPU outputs on an iPhone (R9)
  - a camera-original iPhone clip that still has its location (R12)
  - whether to add the sample-encode estimate (R5)
- **Tools (not committed, `build/` is ignored):** `build/perf/perf.sh` replays the app's argv with SSIM/VMAF. The
  driver used for the argv checks is in the session scratchpad.

---

## Decisions (all answered yes, 2026-11-08)
- [x] D1: Default Speed `slow` → `medium`: yes, if R3's quality check passes
- [x] D2: Start skips files predicted "no saving" unless selected: yes
- [x] D3: Add the Intel GPU encoder option: yes, if R8 says go
- [x] D4: Keep GPS location and creation date in outputs: yes, both
- [x] D5: "Max frame rate: 30" option: yes
- [x] D6: "Max size: 1080p" option: yes
- [x] Record each answer in the plan's Decisions table

## Before T17
- [ ] Add to T17's acceptance criteria in [todo.md](todo.md) and [plan.md](plan.md): ffmpeg still starts at low
      priority (R2), and the output still gets the source's mtime (R12)

---

## - [ ] R0: Baseline (numbers done; lag needs you)
- [x] Corpus in `build/perf/src/` (symlinks to `~/Downloads`, read only): `iphone-hlg` (IMG_1211, HEVC 10-bit HLG
      1080p, 9.5 Mbps), `h264-1080-17m` (Kne.mp4), `h264-720-9m` (finalFinal.mp4), `hevc-portrait-lean`
      (Transport and Machinery.mp4, 7 Mbps), `screen-1080p60` (a 60 s cut of a CodeClub recording), `fixture-720p`.
      Both iPhone clips were re-encoded by an editor (Lavf) and have no location tag. There is no Android clip yet.
- [x] The app's argv is replayed by `build/perf/perf.sh`
- [x] Wall time, CPU %, max RSS, size, SSIM and VMAF for every clip (all 6; R0 total 567.7 s)
- [ ] Note desktop lag while encoding (drag the window, type in an editor). **Needs you** (no WM on Xvfb)
- [x] Results table in plan-performance.md

## Phase A: Quick wins

### - [x] R1: 8-bit formats detected as 10-bit
- [x] Tests first in `tests/test_compress.c` (`/probe/10bit-formats`): `nv12`, `yuv410p`, `yuv420p` → 8-bit;
      `yuv420p10le`, `yuv422p10le`, `p010le`, `yuv420p12le` → 10-bit
- [x] `vc_probe_is_10bit()` now matches `p10`/`p12`/`10le`/`10be`/`12le`/`12be` (`p010le` matches `10le`)
- [x] The other caller, the `10-bit` chip in `refresh_chips`, uses the same function, so it is fixed too
- [x] `ctest` green

### - [x] R2: ffmpeg at low CPU and disk priority
- [x] `start_job` spawns through a `GSubprocessLauncher` whose child setup calls `setpriority(…, 10)` and
      `ioprio_set` idle (`lower_priority()` in `main.c`)
- [x] Encoder only; ffprobe is unchanged
- [x] `ps -o ni,cmd -C ffmpeg` shows 10; `ionice -p <pid>` shows idle (also for the GPU encoder)
- [x] Pause (`T`) and resume on the same PID, Stop (no ffmpeg, no `.part` within 1 s), close mid-encode (exit 0, no ffmpeg, no `.part`), all on the ASan build
- [x] Idle-desktop encode time within 3%: 33.1 s normal vs 33.3 s low priority (+0.5%)

### - [x] R3: Default Speed `medium` (needs D1): gate failed, default stays `slow`
- [x] On the R0 corpus: `medium` is 1.6–2.7 VMAF below `slow` on 4 of 6 clips (limit 1.0); SSIM within 0.004. Table
      in the plan's Results
- [x] Kept `slow`, and wrote why in the plan, SPEC Decision 6 and the D1 row. No code change
- [x] (Would have met the time target: `medium` corpus time is 46% of R0)

### - [x] R4: Tool check without blocking the window
- [x] `check_tools()` is asynchronous: `ffmpeg -encoders` runs through `g_subprocess_communicate_utf8_async`
      (`encoders_listed()`). It then starts the R9 GPU test encode
- [x] Choose Folder (header and empty page) and drop stay insensitive until the check passes
- [x] The window paints at once, and a folder loaded after the check on Xvfb
- [x] `PATH=/nonexistent` shows the error with Choose Folder greyed out; Close quits with exit 0

### - [ ] Checkpoint A
- [x] `ctest` green (Debug and ASan); ASan fixture runs clean
- [ ] R0 re-measured: time ≤ 50% does not apply (D1 not adopted; GPU gives 4.3%); no desktop lag **needs you**

## Phase B: Skip wasted work

### - [ ] R5: Codec-aware size estimate (done except the ±25% target)
- [x] `SOURCE_CAP` table next to `PRESET_FACTORS` in `compress.c`: HEVC/VP9/AV1 1.0, everything else 0.6
- [x] Used in `vc_estimate_size()`. The preset size factor now scales the cap too, so the GPU's 1.5× isn't hidden
      on lean sources
- [x] Calibrated against R0: the class constants stay. Real 1080p output is 1.3–4.5 Mbps depending on content, so no
      constant fits all clips (closes the T11 calibration note; numbers in the plan)
- [x] Fixed: the cap now uses the video stream's own bitrate (it counted audio twice), and copied AAC uses its real
      bitrate; test in `/estimate/codec-aware`
- [x] Tests (`/estimate/codec-aware`): 2.5 Mbps HEVC → "no saving"; 10 Mbps H.264 → saving; unknown duration → encode
- [ ] Estimate vs actual within 25% for every R0 real clip: **not met** (−28% to +127% on high-bitrate clips; lean clips +5% to +17%). The plan's next step is a sample encode, which **needs your go-ahead**

### - [x] R6: Start skips "no saving" files (needs D2)
- [x] `vc_worth_encoding(probe, size, opts)` in `compress.c/.h`, with unit tests
- [x] Bulk Start (nothing selected) leaves those rows as `No saving · skipped`. The tooltip says to select the
      video to compress it anyway (`skip_in_bulk()` in `main.c`)
- [x] Selected rows, a row's own play button, and "Compress Now" always encode
- [x] The Trash confirmation count, the Start button and the "N videos ready" total exclude skipped rows. When only
      skipped rows are left, the total says `N videos likely won't get smaller · select to compress anyway`
- [x] On Xvfb: the lean HEVC fixture shows `no saving` and is not counted in "4 videos ready"
- [x] App run: bulk Start skipped the row (`No saving · skipped`, "1 of 5"), the batch finished with its summary, then selecting it and pressing Start encoded it ("Already efficient", so the prediction was right)

### - [x] R7: Copy AAC audio
- [x] Fixture: not needed. `make-fixtures.sh` already makes several AAC 128k clips (720p, 1080p, portrait…)
- [x] Parser reads audio `bit_rate`; `VcProbe.aac_kbps` is the highest AAC bitrate, or 0 if any stream is not AAC or
      has no bitrate
- [x] Argv: every audio stream AAC ≤ class `audio_kbps` + 10% → `-c:a copy`, else today's `-c:a aac -b:a`
- [x] Tests (`/ffmpeg/audio-copy`): 128k AAC → copy; 256k AAC, unknown bitrate, Opus, AC-3, mixed, no audio → re-encode
- [x] Real encode of the 720p fixture: audio copied (128 kbps kept), duration 60.0 s = source

### - [x] Checkpoint B
- [x] `ctest` green; the lean HEVC fixture is skipped with its reason, and the batch finishes without it

## Phase E: Output fixes

### - [ ] R12: Keep date and location (D4 = yes; location on a real camera clip needs you)
- [x] `copy_mtime()` in `finish_job` copies `time::modified` (+usec) from source to output, after the rename and
      before `check_then_trash`. It is best effort
- [x] MP4 movflags `+faststart+use_metadata_tags`, plus `-map_metadata 0` on every output; argv tests updated
- [x] Checked with ffmpeg on a synthetic `.mov` holding `com.apple.quicktime.location.ISO6709` and `creation_time`.
      With the new flags both survive; without them both are lost
- [x] Through the app: `stat -c %y` of every output == its original (2025-05-01 10:00)
- [ ] The location key on a camera-original iPhone clip: **needs you** (both clips in Downloads have lost it)
- [x] Verification unchanged (outputs pass the check); Trash itself was not re-run, but the code path is untouched

### - [x] R13: HDR10 (PQ) headers
- [x] `hdr10-pq.mov` fixture in `make-fixtures.sh`
- [x] `smpte2084` source → `-x265-params log-level=error:hdr10=1:repeat-headers=1`
- [x] Argv tests (`/ffmpeg/argv-hdr10`): PQ gets it; HLG and 8-bit unchanged
- [x] Encoded: Main 10, `smpte2084`, and the stream carries the HDR10 "Content light level" SEI

## Phase C: Intel GPU encoder (needs D3)

### - [x] R8: Measure the GPU on real clips (no code): **go**
- [x] R0 corpus with the VA-API `low_power` argv at QP 26/28/30/32 (`build/perf/r8-gpu.csv`)
- [x] QP 28 matches `medium` for 720p and 1080p, at about 1.1× its size and 8% of its time; QP 26 ≈ `slow`
- [x] 10-bit HLG → `Main 10`, `hvc1`, all three colour tags
- [x] CPU-decode fallback: `480p.webm` (VP8) encodes fine
- [x] `scale_vaapi` with the `if(gt(iw,ih),…)` expressions works, including portrait
- [x] Found: rotated phone clips came out sideways. Fixed with `-noautorotate` + `transpose_vaapi` (90, −90, 180 checked against the CPU path frame by frame)
- [x] Go, written in the plan

### - [ ] R9: "GPU" Speed option (done except iPhone playback, which needs you)
- [x] `VC_PRESET_GPU` is the first entry in `VC_PRESETS`; the button is labelled `GPU`. `VcClass.gpu_qp` from R8:
      28 for 720p/1080p, the other classes at the same offset (26/26/27/…/29)
- [x] `hevc_vaapi` branch in `vc_build_ffmpeg_argv`: GPU decode, `format=nv12|vaapi,hwupload` (p010 for 10-bit),
      `-low_power 1 -rc_mode CQP -qp N`, `main10` for 10-bit, colour tags, `hvc1`, `+faststart`
- [x] Argv unit tests (`/ffmpeg/argv-gpu`)
- [x] The startup check runs a 1-frame test encode (`vc_build_gpu_test_argv`). The GPU button is hidden unless it
      passes; it passed on Xvfb
- [x] Factors in `PRESET_FACTORS` from R8 (0.08× time, 1.15× size); the speed guide hides the GPU row when it's unavailable
- [x] 8-bit, portrait, rotated and 10-bit HLG outputs decode with no errors and carry `hvc1` + colour tags
- [ ] Play them on an iPhone / QuickTime: **needs you**
- [x] Option absent under `bwrap --tmpfs /dev/dri` (test encode exit 234); the app works on x265
- [x] GPU batch in the app: 5 files in 10 s at nice 10; Pause/Stop/close use the same code as x265 (checked there). Corpus wall time 4.3% of R0

### - [x] R14: "Max 30 fps" (D5 = yes)
- [x] `VcProbe.fps` from `avg_frame_rate` (T21 can reuse it)
- [x] `Max 30 fps` switch, off by default; source > 30.5 fps → `-fpsmax 30`
- [x] Argv tests (`/ffmpeg/frame-rate-cap`): 60 and 59.94 capped; 29.97, 30, 24, unknown unchanged
- [x] Real encode of `screen-1080p60`: 30 fps out, 60.00 → 60.07 s, x265 time 54%, GPU 59%
- [x] The estimate ignores the cap, so it is pessimistic there (`shortcut:` comment in `vc_estimate_size`)

### - [x] R15: "Max size" (D6 = yes)
- [x] `Original size / Max 1080p / Max 720p` dropdown, default `Original size`; downscale only, by the short side
- [x] CPU `scale`, GPU `scale_vaapi`, with `w='if(gt(iw,ih),-2,N)':h='if(gt(iw,ih),N,-2)'` so rotated portrait
      clips work. The class (CRF, QP, audio) and the estimate come from the output size
- [x] Argv tests (`/ffmpeg/max-size`): 4K → 1080p, portrait, 1080p → 720p, odd sizes, no upscale
- [x] Real encodes: 4K → 1920×1080 (x265 time 32%), rotated phone clips → 720×1280 on CPU and GPU, 720p unchanged; the dropdown re-estimates rows in the app
- [x] SPEC updated (Decision 4 now allows it as an opt-in)

## Phase D: Large folders

### - [ ] R10: Probe up to 4 files at once (done; the 30% target is not met)
- [x] `probe_step` keeps up to 4 in flight; `Item *` is the `user_data`; the cancelled early return comes before
      any `Item` access; `probes_done` counts completions, and `probing` in `update_controls` uses it
- [ ] 300-file folder lists in ≤ 30% of the old time: **48%** (39.2 → 18.8 s; 8 at once gives 39%). ffprobe costs about 90 ms CPU each, so kept at 4
- [x] ASan: switched folders twice with 4 probes in flight; no reports, the final list is right, exit 0

### - [ ] R11: Cap x265 threads (only if R0/R2 show heat or lag)
- [ ] Not started, and not needed so far: no lag or heat reported, and R2 costs 0.5%. Revisit if you notice lag

---

### - [ ] Checkpoint E (plan complete)
- [x] `ctest` green, ASan runs clean
- [x] Every target in the plan's "Goal" met or explained in Results, except desktop lag (**needs you**)
