# Todo: Performance and resources

Plan: [plan-performance.md](plan-performance.md) · Main todo: [todo.md](todo.md) · Spec: [../SPEC.md](../SPEC.md)

Shared commands (build, test, fixtures, run) are in [todo.md](todo.md). Each task's full description, acceptance
criteria and verification are in the plan under the same ID. This list is the order to work in and the steps.

Order: decisions → R0 → Phase A → B → E (all before T16) → C → D.
If speed matters most, do Phase C right after R0: it is the only phase that turns hours into minutes.
Tasks that edit `main.c` run one after another. `compress.c` tasks can go in any order.

---

## Status (2026-11-08, uncommitted)
- **Code written for every task except R3 and R11.** The Debug build is clean with `-Werror`, and `ctest` is green
  (26 tests, 7 new).
- **Shown on Xvfb:** the folder lists, the GPU speed button appears (the startup test encode passed), the Max 30 fps
  switch and Max size dropdown fit in the bottom bar, and the lean HEVC fixture is left out of "4 videos ready".
- **Not verified yet:** no encode has run through the app since the changes, there has been no ASan run, and most
  of the manual checks below are still open.
- **Measurements:** R0 slow is done for 5 of 6 clips. R0 medium is half done. The HLG reruns and the R8 GPU sweep
  are queued in the background (`build/perf/`).
- **Changes beyond the plan:**
  - Encode settings now go in a `VcOptions` struct (preset, keep_subs, max_short_side, max_fps). It is passed to
    `vc_build_ffmpeg_argv` and `vc_estimate_size`, because four positional settings were getting error-prone.
  - Every output gets `-map_metadata 0`. Without it, ffmpeg 8 drops the source's `creation_time`, which D4 needs.
  - The default window is 1160 px wide, and the totals text truncates, so the new controls don't force a wider window.
  - SPEC.md is updated for D2–D6, AAC copy, HDR10, metadata, priority and probing. The default speed text waits on R3.
- **Measurement script:** `build/perf/perf.sh <slow|medium|gpu:QP> [clips]` replays the app's argv and prints wall
  time, CPU %, max RSS, size, SSIM and VMAF. It is not committed, because `build/` is ignored.

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

## - [ ] R0: Baseline
- [x] Corpus in `build/perf/src/` (symlinks to `~/Downloads`, read only): `iphone-hlg` (IMG_1211, HEVC 10-bit HLG
      1080p, 9.5 Mbps), `h264-1080-17m` (Kne.mp4), `h264-720-9m` (finalFinal.mp4), `hevc-portrait-lean`
      (Transport and Machinery.mp4, 7 Mbps), `screen-1080p60` (a 60 s cut of a CodeClub recording), `fixture-720p`.
      Both iPhone clips were re-encoded by an editor (Lavf) and have no location tag. There is no Android clip yet.
- [x] The app's argv is replayed by `build/perf/perf.sh`
- [ ] Wall time, CPU %, max RSS, size and SSIM for every clip. Slow is done for 5 of 6. `iphone-hlg` failed because
      of a script bug (ffprobe's CSV field order), which is fixed, and its rerun is queued.
- [ ] Note desktop lag while encoding (drag the window, type in an editor)
- [ ] Results table in plan-performance.md

## Phase A: Quick wins

### - [x] R1: 8-bit formats detected as 10-bit
- [x] Tests first in `tests/test_compress.c` (`/probe/10bit-formats`): `nv12`, `yuv410p`, `yuv420p` → 8-bit;
      `yuv420p10le`, `yuv422p10le`, `p010le`, `yuv420p12le` → 10-bit
- [x] `vc_probe_is_10bit()` now matches `p10`/`p12`/`10le`/`10be`/`12le`/`12be` (`p010le` matches `10le`)
- [x] The other caller, the `10-bit` chip in `refresh_chips`, uses the same function, so it is fixed too
- [x] `ctest` green

### - [ ] R2: ffmpeg at low CPU and disk priority
- [x] `start_job` spawns through a `GSubprocessLauncher` whose child setup calls `setpriority(…, 10)` and
      `ioprio_set` idle (`lower_priority()` in `main.c`)
- [x] Encoder only; ffprobe is unchanged
- [ ] `ps -o ni,cmd -C ffmpeg` shows 10; `ionice -p <pid>` shows idle
- [ ] Pause, resume, Stop, close mid-encode unchanged (ASan build exits 0)
- [ ] Idle-desktop encode time within 3% of R0

### - [ ] R3: Default Speed `medium` (needs D1)
- [ ] On the R0 corpus: `medium` vs `slow` SSIM ≤ 0.005 and VMAF ≤ 1 apart on every clip; table in the plan.
      **So far it fails on VMAF:**

      | Clip | VMAF slow → medium | SSIM slow → medium | Time medium / slow |
      |---|---|---|---|
      | fixture-720p | 97.63 → 97.51 | 0.9992 → 0.9990 | 62% |
      | h264-1080-17m | 94.50 → 92.10 (−2.4) | 0.9845 → 0.9820 | 39% |
      | h264-720-9m | 96.40 → 93.73 (−2.7) | 0.9817 → 0.9779 | 38% |
      | hevc-portrait-lean | 95.08 → 93.50 (−1.6) | 0.9916 → 0.9899 | 43% |

      SSIM passes on every clip. Part of the VMAF drop comes from `medium` files being 7–11% smaller at the same CRF.
- [ ] If a real clip fails: keep `slow`, write why in the plan, stop here. **Likely outcome; waiting on the last two clips.**
- [ ] `VC_DEFAULT_PRESET` → `"medium"` ([compress.h](../src/compress.h)), only if the gate passes
- [ ] Update SPEC Criterion 11, plan.md "Confirmed decisions", speed guide text
- [ ] `ctest` green; corpus wall time ≤ 50% of R0

### - [ ] R4: Tool check without blocking the window
- [x] `check_tools()` is asynchronous: `ffmpeg -encoders` runs through `g_subprocess_communicate_utf8_async`
      (`encoders_listed()`). It then starts the R9 GPU test encode
- [x] Choose Folder (header and empty page) and drop stay insensitive until the check passes
- [x] The window paints at once, and a folder loaded after the check on Xvfb
- [ ] `PATH=/nonexistent` still shows the error and quits cleanly

### - [ ] Checkpoint A
- [ ] `ctest` green, ASan full fixture run clean
- [ ] R0 re-measured: time ≤ 50% (with D1), no desktop lag

## Phase B: Skip wasted work

### - [ ] R5: Codec-aware size estimate
- [x] `SOURCE_CAP` table next to `PRESET_FACTORS` in `compress.c`: HEVC/VP9/AV1 1.0, everything else 0.6
- [x] Used in `vc_estimate_size()`. The preset size factor now scales the cap too, so the GPU's 1.5× isn't hidden
      on lean sources
- [ ] Recalibrate `EST_VIDEO_KBPS` from the R0 real clips (closes the T11 calibration note). Waiting on R0
- [x] Tests (`/estimate/codec-aware`): 2.5 Mbps HEVC → "no saving"; 10 Mbps H.264 → saving; unknown duration → encode
- [ ] Estimate vs actual within 25% for every R0 real clip; table in the plan

### - [ ] R6: Start skips "no saving" files (needs D2)
- [x] `vc_worth_encoding(probe, size, opts)` in `compress.c/.h`, with unit tests
- [x] Bulk Start (nothing selected) leaves those rows as `No saving · skipped`. The tooltip says to select the
      video to compress it anyway (`skip_in_bulk()` in `main.c`)
- [x] Selected rows, a row's own play button, and "Compress Now" always encode
- [x] The Trash confirmation count, the Start button and the "N videos ready" total exclude skipped rows. When only
      skipped rows are left, the total says `N videos likely won't get smaller · select to compress anyway`
- [x] On Xvfb: the lean HEVC fixture shows `no saving` and is not counted in "4 videos ready"
- [ ] Manual Start run on that folder: the row is skipped, then selecting it and pressing Start encodes it

### - [ ] R7: Copy AAC audio
- [x] Fixture: not needed. `make-fixtures.sh` already makes several AAC 128k clips (720p, 1080p, portrait…)
- [x] Parser reads audio `bit_rate`; `VcProbe.aac_kbps` is the highest AAC bitrate, or 0 if any stream is not AAC or
      has no bitrate
- [x] Argv: every audio stream AAC ≤ class `audio_kbps` + 10% → `-c:a copy`, else today's `-c:a aac -b:a`
- [x] Tests (`/ffmpeg/audio-copy`): 128k AAC → copy; 256k AAC, unknown bitrate, Opus, AC-3, mixed, no audio → re-encode
- [ ] Copied-audio output passes the duration check (real encode)

### - [ ] Checkpoint B
- [ ] `ctest` green; a folder of lean HEVC clips finishes in seconds, each row explained

## Phase E: Output fixes

### - [ ] R12: Keep date and location (D4 = yes)
- [x] `copy_mtime()` in `finish_job` copies `time::modified` (+usec) from source to output, after the rename and
      before `check_then_trash`. It is best effort
- [x] MP4 movflags `+faststart+use_metadata_tags`, plus `-map_metadata 0` on every output; argv tests updated
- [x] Checked with ffmpeg on a synthetic `.mov` holding `com.apple.quicktime.location.ISO6709` and `creation_time`.
      With the new flags both survive; without them both are lost
- [ ] Through the app: `stat -c %y` output == original; the location key is present (needs a camera-original
      iPhone clip, because both clips in Downloads have lost it)
- [ ] Verification and trash unchanged

### - [ ] R13: HDR10 (PQ) headers
- [ ] `hdr10-pq.mov` fixture (not made yet)
- [x] `smpte2084` source → `-x265-params log-level=error:hdr10=1:repeat-headers=1`
- [x] Argv tests (`/ffmpeg/argv-hdr10`): PQ gets it; HLG and 8-bit unchanged
- [ ] Encode the PQ fixture and check the output's HDR10 SEI with ffprobe

## Phase C: Intel GPU encoder (needs D3)

### - [ ] R8: Measure the GPU on real clips (no code)
- [ ] R0 corpus with the VA-API `low_power` argv at QP 26/28/30/32. Queued in the background (`r8-gpu.csv`)
- [ ] Per class: QP matching x265 VMAF, size and fps vs x265
- [ ] 10-bit HLG → `main10` with colour tags? If not, 10-bit stays on x265
- [ ] CPU-decode fallback with a codec the GPU can't decode (`480p.webm`, VP8)
- [ ] `scale_vaapi` with the `if(gt(iw,ih),…)` size expressions works (R15)
- [ ] Go / no-go in the plan

### - [ ] R9: "GPU" Speed option (built ahead of R8; QPs are provisional)
- [x] `VC_PRESET_GPU` is the first entry in `VC_PRESETS`; the button is labelled `GPU`. `VcClass.gpu_qp` is
      provisionally CRF + 3 (QP 30 for 1080p, from the earlier synthetic test). **Replace with R8's numbers**
- [x] `hevc_vaapi` branch in `vc_build_ffmpeg_argv`: GPU decode, `format=nv12|vaapi,hwupload` (p010 for 10-bit),
      `-low_power 1 -rc_mode CQP -qp N`, `main10` for 10-bit, colour tags, `hvc1`, `+faststart`
- [x] Argv unit tests (`/ffmpeg/argv-gpu`)
- [x] The startup check runs a 1-frame test encode (`vc_build_gpu_test_argv`). The GPU button is hidden unless it
      passes; it passed on Xvfb
- [x] Factors in `PRESET_FACTORS` (0.1× time, 1.5× size); the speed guide hides the GPU row when it's unavailable
- [ ] 8-bit, portrait, 10-bit HLG outputs play in mpv and iOS with `hvc1` + colour tags
- [ ] Option absent under `bwrap` without `/dev/dri`
- [ ] Pause, Stop, close mid-encode work; corpus wall time ≤ 15% of R0

### - [ ] R14: "Max 30 fps" (D5 = yes)
- [x] `VcProbe.fps` from `avg_frame_rate` (T21 can reuse it)
- [x] `Max 30 fps` switch, off by default; source > 30.5 fps → `-fpsmax 30`
- [x] Argv tests (`/ffmpeg/frame-rate-cap`): 60 and 59.94 capped; 29.97, 30, 24, unknown unchanged
- [ ] Real encode of `screen-1080p60`: 30 fps out, same duration ±1 s, time ≈ half
- [ ] The estimate ignores the cap, so it is pessimistic there (`shortcut:` comment in `vc_estimate_size`)

### - [ ] R15: "Max size" (D6 = yes)
- [x] `Original size / Max 1080p / Max 720p` dropdown, default `Original size`; downscale only, by the short side
- [x] CPU `scale`, GPU `scale_vaapi`, with `w='if(gt(iw,ih),-2,N)':h='if(gt(iw,ih),N,-2)'` so rotated portrait
      clips work. The class (CRF, QP, audio) and the estimate come from the output size
- [x] Argv tests (`/ffmpeg/max-size`): 4K → 1080p, portrait, 1080p → 720p, odd sizes, no upscale
- [ ] Real encodes: 4K → 1080p, rotated iPhone portrait → 1080×1920, 720p unchanged; time ≈ a quarter
- [x] SPEC updated (Decision 4 now allows it as an opt-in)

## Phase D: Large folders

### - [ ] R10: Probe up to 4 files at once
- [x] `probe_step` keeps up to 4 in flight; `Item *` is the `user_data`; the cancelled early return comes before
      any `Item` access; `probes_done` counts completions, and `probing` in `update_controls` uses it
- [ ] 300-file folder lists in ≤ 30% of the old time
- [ ] ASan: switch folders mid-probe twice, clean

### - [ ] R11: Cap x265 threads (only if R0/R2 show heat or lag)
- [ ] Not started. Measure the time cost of `pools=12`, record it, then decide

---

### - [ ] Checkpoint E (plan complete)
- [ ] `ctest` green, ASan full fixture run clean
- [ ] R0 table re-measured; every target in the plan's "Goal" met or explained
