# Project log: Video Compressor → Media Tools

A rough log of what was built, decided and measured, kept after the planning files were removed on 2026-11-11.
For how the app works now, read [HANDOFF.md](HANDOFF.md) and the specs ([SPEC.md](SPEC.md) and `SPEC-*.md`).
Planned work is in [PERFORMANCE.md](PERFORMANCE.md) and [review/](review/findings.md).

**Removed files** (all still in git history; read one with
`git show 84398e5:"Video Compressor/<path>"`):
- `tasks/plan.md`, `tasks/todo.md`: the phase plan and checklist for Phases 1–10 (T1–T27).
- `tasks/plan-performance.md`, `tasks/todo-performance.md`: the performance plan R0–R15.
- `CAPABILITIES.md`: the module map (video-compress, app-shell, web-video, video-frames, image-compress).
- `research/handbrake/00-index.md` … `15-lessons-for-video-compressor.md`: notes on how HandBrake works, used to write
  the adoption list that is still kept in [research/handbrake/adopt/](research/handbrake/adopt/00-index.md).
- The "Verified so far" section of HANDOFF.md, moved to [Verification sessions](#verification-sessions) below.

## Timeline

| Date | Commit | What |
|---|---|---|
| 2026-03-17 | `47a9757` | Spec, plan and todo for Video Compressor (dates before 2026-11-08 are backdated on purpose) |
| 2026-03-27 | `d32fd96` | Phases 1–3: folder → H.265 copies in `compressed/` (MVP) |
| 2026-11-08 | `e0a1656` | Phase 4: move originals to Trash after the copy is checked |
| 2026-11-08 | `aac0ae7` | Phase 5: sanitizers, T11 results, preset factors |
| 2026-11-08 | `da1cc2e`, `4145076` | Phase 6: icon controls, pause, per-row and selected actions, size estimate, speed guide; v2 UI from Figma |
| 2026-11-08 | `41ffbe1`, `ac3eefd` | Media Tools planned: four module specs, Phases 7–10 |
| 2026-11-08 … 11-10 | `6a58c98` … `1bd0164` | Performance plan R1–R15: GPU encoder, GPU size samples, AAC copy, skip "no saving", low priority, Max size / Max 30 fps, HDR10 headers, date and location kept, x265 speed tuning |
| 2026-11-10 | `7541405` | Phase 7: app shell (modes, multi-step runner, Choose Files, title Media Tools) |
| 2026-11-10 | `7de014b` | Phase 8: Web video (Safari-ready H.264) |
| 2026-11-10 | `d785d0a` | Phase 9: Video → frames |
| 2026-11-10 | `8921588`, `492a347` | Phase 10: Compress images, twin names |
| 2026-11-10 | `8d4b100` | Performance and security review ([review/](review/findings.md)) |
| 2026-11-11 | `b1f4378` | v3 UI from Figma: mode sidebar, header toolbar, per-mode inspector, footer, accent per mode, dark mode, custom icons |
| 2026-11-11 | `84398e5` | Default speed Medium, Use GPU switch, footer time estimate learned per machine, Inter font embedded |
| 2026-11-11 | (not committed) | Docs cleanup: this log and PERFORMANCE.md; finished plans and research notes removed |

## Decisions

- **Scope (2026-03):** no recursive scan; subtitles dropped by default (Keep subtitles writes `.mkv`); Linux only; x265
  through the ffmpeg CLI (argv only, `GSubprocess`), never linked as libraries; `.part` then rename; never overwrite;
  originals only ever go to the Trash, after a check.
- **Media Tools (2026-11-08):** one app with a mode switch; input from a folder or chosen files; images get a same-type
  copy plus `.webp`; web video is H.264 MP4 only; web modes have a Max width (downscale only) and web video a Remove audio
  switch; frames are a JPG/WebP sequence with start/end trim. Build order app-shell → web-video → video-frames →
  image-compress.
- **Performance decisions D1–D6 (2026-11-08, all yes):** D1 default Medium *if* it passes the quality gate (it didn't,
  see R3); D2 Start skips files predicted to save nothing unless selected; D3 Intel GPU encoder; D4 keep GPS location and
  creation date; D5 Max 30 fps option; D6 Max 1080p option.
- **v3 UI (2026-11-11):** follow the Figma "v3 – Media Tools" page; window buttons on the left as drawn; default speed
  Medium at the user's request, overriding R3 (see [PERFORMANCE.md](PERFORMANCE.md#decision-needed-first-default-speed));
  GPU as a "Use GPU" switch; Inter embedded so the app works offline; the three packages (Flatpak, AppImage, plain
  folder) will bundle ffmpeg, ffprobe, ImageMagick and pngquant.

## Measurements

These numbers are cited by code comments (`compress.c`, `compress.h`, `main.c`) and SPEC.md. Laptop: i5-1240P,
16 threads, 7 GB RAM, Iris Xe.

### T11: first results (2026-11-08)


| File | Class | Preset | Before | After | % of source | SSIM | fps |
|---|---|---|---|---|---|---|---|
| `720p.mp4` (fixture) | 720p | slow | 60.9 MB | 1.64 MB | 2.7% | 0.9992 | 48 |
| `720p.mp4` (fixture) | 720p | faster | 60.9 MB | 1.50 MB | 2.5% | 0.9990 | 122 |
| `1080p.mkv` (fixture) | 1080p | slow | 20.2 MB | 0.62 MB | 3.1% | 0.9992 | 22 |
| `1080p.mkv` (fixture) | 1080p | faster | 20.2 MB | 0.58 MB | 2.8% | 0.9990 | 56 |
| real 200 MB+ phone clip | – | slow / faster | – | – | – | – | – |


  camera footage, so these numbers say nothing about the CRF table. No table change is proposed.
- Criterion 4 is still open: there is no phone recording on this machine (`~/Videos/Camera` is empty).
- `slow` is about 2.5× slower than `faster` at both sizes, for about 10% smaller files on `testsrc`.


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

The `slow` time factor in `PRESET_FACTORS` is now 1.7, after the x265 speed tuning of 2026-11-10 (below).

### First GPU and encoder comparison (2026-11-08)

10 s 1080p synthetic clip with heavy noise (worst case, sizes not representative):

| Encoder | Time | Size |
|---|---|---|
| libx265 `slow` (default) | 199 s | 64 MB |
| libx265 `medium` | 79 s | 51 MB |
| `hevc_qsv` (Intel iGPU) | 19 s | 67 MB |
| `hevc_vaapi` | 18 s | 113 MB |
| libsvtav1 preset 8 | 33 s | 106 MB |

Plus the preset table in [Preset factors](#preset-factors-2026-11-08): `slow` is 2.1× the time of `medium` and
1.08× the size. `libvmaf` and `ssim` filters are available in the installed ffmpeg.

**Second run, closer to phone footage** (10 s 1080p60, light grain, 16 Mbps H.264, 600 frames):

| Pipeline | fps | Size | SSIM |
|---|---|---|---|
| x265 `medium` CRF 27 (the app today) | 20–28 | 6.9 MB | 0.9870 |
| x264 `fast` RF 22 (HandBrake-like) | 47 | 13.9 MB | — |
| `hevc_qsv`, default mode (CPU or GPU decode) | 9–19 | 18.5 MB | — |
| **`hevc_vaapi -low_power 1`, GPU decode, QP 30** | **261** | 10.4 MB | **0.9873** |
| `hevc_vaapi -low_power 1`, GPU decode, QP 32 | 265 | 7.0 MB | 0.9846 |


### Medium vs Slow and the GPU on real clips (R0, R3, R8)

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
  within 25%**: real 1080p output ranges from 1.3 to 4.5 Mbps depending on content.
- **R5 sample encode (approved 2026-11-09, with limits):** one 6 s window from the middle was off by up to 2× (the middle
  of a clip isn't typical); three 4 s windows at 20/50/80% in one ffmpeg (1.7 s for the same bytes as three runs) land
  at GPU −10% to +15% and x265 −18% to +28% against the real sizes, for 0.6–2.0 s per file. Limits as in SPEC "Size
  estimate". Checked in the app (ASan): 24 samples on a 301-file folder, biggest first, never overlapping; none during
  a 20 s encode, resumed after Stop; a folder switch kills the one in flight (0.5 s) with no ASan reports.

- **x265 speed tuning (2026-11-10, after this plan):** `slow`/`slower` add `rect=0:rskip=2` (HandBrake's tuning):
  1080p 88.9 → 71.0 s, 720p 76.0 → 59.6 s, VMAF −0.2, size ±1%. Outputs with a short side ≤ 576 add `ctu=32` (x265
  used only ~4 of 16 threads there): a 324p cut at `slow` 64.2 → 33.1 s. `slow` time factor 2.1 → 1.7. Details in
  [16-encode-speed.md](research/handbrake/adopt/16-encode-speed.md). The R0 table above predates this.

**Bugs found by these measurements, all fixed:**
- GPU outputs of phone portrait clips (stored landscape + rotation tag) came out sideways: ffmpeg neither rotates GPU
  frames nor keeps the tag. Fixed with `-noautorotate` + `transpose_vaapi` from the probed rotation.
- ffmpeg 8 drops the source's `creation_time` unless metadata is mapped: every output now gets `-map_metadata 0`.
- The estimate's lean-source cap used the whole file's bitrate and then added audio again, so lean recordings with
  rich audio showed "no saving" (a lecture clip: predicted 0%, real −38%). It now uses the video stream's own bitrate.
- The GPU size factor guessed from one synthetic clip (1.5×) made most GPU estimates "no saving"; measured 1.15×.


### Encode speed seed for the time estimate (2026-11-11)

`SEED_MEDIUM_PPS` in `main.c` (2.8e7 output pixel-frames per second at x265 medium) comes from the speed tuning
measurements in [16-encode-speed.md](research/handbrake/adopt/16-encode-speed.md): a 576×324 cut at medium + `ctu=32`
ran at about 2.4e7, and the 1080p and 720p clips at slow + `rect=0:rskip=2` at 1.5–1.8e7 (× 1.7 for medium). After each
real encode the app replaces the seed with its own measurement (`~/.config/media-tools/speed.ini`). The first measured
run on the fixtures gave 7.7e7, because synthetic clips encode fast.

### PERFORMANCE.md items 1, 2, 4, 6, 7, 8 (2026-11-11)

- **Several images at once (item 2):** 100 images (60 JPEG 2400×1600, 40 PNG 1600×1000), Original width, ASan build on
  Xvfb: one at a time 71.5 s, eight at once 22.5–23.2 s (3.1×). All 200 outputs byte-identical (`sha256sum`). Pause froze
  all 8 processes (state `T` in `/proc`), resume finished the batch; Stop all and Stop and Quit mid-batch left no `.part`
  and no magick/pngquant running; every session exit 0, no sanitizer report. Compress video still ran 1 job.
- **Two GPU encodes at once (item 7), rejected:** the app's GPU argv at nice 10 / idle I/O, one at a time vs two at once.
  6 perf clips: 23.2 / 27.4 s vs 28.0 / 25.4 s. 11 short fixtures: 19.0 / 16.9 s vs 17.2 / 16.3 s. Under 1.1×, below the 1.3×
  bar in research note 08.
- **Decoder thread budget (item 8), rejected:** `-threads 9` before `-i`, x265 `faster`, 3 clips (h264 1080p, hevc
  portrait, iPhone HLG), two A/B rounds: 82.6 / 87.5 s without, 84.7 / 92.6 s with. Slower, so `medium` wasn't run.
- **Image output planning (item 4, review #8):** 4,000 images 7.56 s → 0.009 s.
- **Release build (item 1):** `-O2`, `_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, stripped install: 485 kB vs the
  Debug binary's 714 kB.
- **Inter subset (item 6):** four weights 1.6 MB → 312 kB.

## HandBrake research (summary)

Sixteen notes (2026-11-09) read HandBrake's source to learn how it scans, decodes, filters, encodes and muxes, and why it
is fast. The main lessons: HandBrake's speed came from H.264, downscaling and frame-rate caps rather than better
engineering; x265 used only ~4 of 16 threads on small video (fixed with `ctu=32`); its x265 tuning `rect=0:rskip=2` saves
20 % at slow. The actionable results live on in [research/handbrake/adopt/](research/handbrake/adopt/00-index.md): 13
techniques still to try, what is already adopted, and what was rejected and why.

## Verification sessions

Moved from HANDOFF.md on 2026-11-11. Newest first.

Phase 10, on Xvfb, `build/asan` (ASan + UBSan, `G_DEBUG=fatal-criticals`), run copies under `build/asan/p10/{a,b,c,d,e}`.
Five sessions, every one exit 0 with no sanitizer report:
- Listing: 11 rows (no `anim.gif`), `fake.png` `Unreadable`, rows `JPEG · 2400×1600`; Start pulses the row bar.
- Original width: every JPEG/PNG/BMP/TIFF gave its same-type (BMP → png with alpha, TIFF → jpg) plus `.webp`, `banner.webp`
  one `.webp`. Every output decodes (`magick identify`) and Files rendered thumbnails. `rotated.jpg` (stored 1600×1200,
  EXIF orientation 6 + GPS) → 1200×1600, no orientation, no EXIF at all (exiftool). `alpha.png` and `logo.bmp` outputs keep
  alpha. `already-tiny.png` → `Skipped – already optimized`, nothing kept. No kept output is bigger than its source.
- A re-run: every finished row `Already compressed`; `already-tiny.png` is retried and skipped again (see Open issues).
- Max 1280 + Trash on: `photo.*` 1280×853, `odd-1001x667.*` stay 1001×667, `scan.*` 1280×1656, `screenshot.*` 1280×800. 9
  originals trashed (`rotated.jpg` passed through the transposed check); `already-tiny.png` and `fake.png` stayed. All 12
  restored byte-identical (`sha256sum -c`).
- Wrappers on PATH (slow magick/pngquant, pngquant exit 99 for `alpha`, magick failing on `logo.bmp`, ffprobe sleeping 30 s on
  `compressed/` paths): `alpha.png` → `png already optimized · webp 8.7 kB −39%`, `Original kept (no png copy)`, not
  trashed; `logo.bmp` → `Failed`, original kept. One batch Stop during `screenshot.png`'s pngquant step (intermediate present)
  and `odd-1001x667.jpg`'s output check: no `.part` or `.mid.part` left, row `Stopped`; the checked one `Original kept`.
  Close mid-job → Stop and Quit: exit 0, no magick/pngquant, no part. 7 trashed originals restored byte-identical.
- pngquant hidden from PATH: Compress images shows "Compress images needs pngquant: sudo dnf install pngquant" with Start
  greyed (10 images ready); Compress video has no banner. Two chosen videos then Compress images: `0 files · 2 hidden (not
  images)`.
- Compress video, Web video and Video → frames each still finished a 2-file run in the same build.
- After the twin and wording fixes (a sixth session, exit 0, clean): `twin.jpg` + `twin.png` → `twin.jpg.jpg`, `twin.jpg.webp`,
  `twin.png.png`, `twin.png.webp`, while `photo.jpg`/`screenshot.png` keep plain names; a re-run shows all four `Already
  compressed`. Start tooltip "Compress every waiting image", row menu `Compress`, close dialog "Keep working in the
  background?", web summary `Smaller`, frames summary `vs the videos`.
- Headless (throwaway driver linked against `vc_core`): the same per-output results on every fixture.

Phase 9, on Xvfb, `build/asan` (ASan + UBSan, `G_DEBUG=fatal-criticals`), run copies under `build/asan/p9/{f,g,h,i,j}`. Every
session exited 0 with no sanitizer report:
- Defaults (15 fps, WebP, q80, Max 1920) on 5 fixtures: `720p` → `frame_0001.webp` … `frame_0900.webp`, and every folder's count
  equals the `≈` hint shown before Start (300/901/900/150/300). Rows end `900 frames · 8.7 MB`. A re-run: all `Already extracted`.
- 12 fps, Start `10`, End `0:12`, Max 640, JPG: 24 frames each, `1080p` 640×360, portrait 640×1138. `hlg-10bit.mov` (10 s) showed
  `Starts after the end` before Start and was the only one skipped. Hints update as FPS/Start/End change.
- Start `abc`: the field turns red and Start greys.
- Stop mid-extraction (164 frames written): ffmpeg gone, no `.part` folder, no final folder. Start again: extracted (1802 frames).
- Close mid-extraction → Stop and Quit: exit 0, no ffmpeg, `frames/` empty.
- Headless (throwaway driver): the same counts; the first trimmed frame matches the source at 10 s (same counter, best SSIM at
  10.0 s); the tone-mapped HLG JPG looks neither washed out nor dark next to the source (testsrc only).
- Compress video and Web video each still finished a 2-file run in the same build.

Phase 8, on Xvfb, `build/asan` (ASan + UBSan), run copies under `build/asan/p8/f/`. Every session exited 0 with no sanitizer report:
- Web video at Max 1280 on the fixture folder: 12 outputs, all `h264`/High/`avc1`/`yuv420p`, even sizes, `aac` LC, `moov` before
  `mdat`, no subtitle stream, no format tags. 1080p → 1280×720, 720p stays, portrait stays 1080×1920, `odd-853x479` → 852×478.
  HLG and PQ outputs are `bt709`; a frame next to the source looks neither washed out nor dark (testsrc only).
  `lowbitrate.mp4` (+178%) and `odd-853x479` (+26%) are kept with a `+N%` pill.
- Original width + Remove audio: 1080p stays 1920×1080, outputs have no audio stream. Existing outputs show `Already converted`.
- Stop mid-file: ffmpeg gone in 175 ms, no `.part`; Start again finished the rest. Close mid-encode → Stop and Quit: exit 0, no
  ffmpeg, no `.part`. While running, the Mode dropdown is greyed and both Choose buttons are hidden (app-shell criterion 4).
- Mode switch with a folder loaded relists it and resets rows, both ways. This found and fixed a heap-use-after-free in
  `load_source()` (a reload passed `app.folder`, which it freed and then read).
- An `ffmpeg` wrapper that drops `libx264` from `-encoders`: Web video shows the banner with Start greyed; Compress video in the
  same session has no banner and compresses.
- Headless (throwaway driver linked against `vc_core`): the same results on every fixture.

Phase 7, on Xvfb, `build/asan` (ASan + UBSan), run copies under `build/asan/p7/`. Every session exited 0 with no sanitizer report:
- Choose Files with `two/a/clip-720.mp4` and `two/b/clip-240.avi`: only those two listed, header `Chosen files · 2 files`, outputs
  in `two/a/compressed/` and `two/b/compressed/`. Both header buttons hidden while running. Choose Folder afterwards replaced the list.
- Full fixture run at `faster` with subtitles and auto-trash: pause gave ffmpeg state `T`, resume `S` (same PID); batch Stop
  mid-file killed ffmpeg within 1 s with no `.part`; a restart finished the rest. ffmpeg ran at nice 10 and outputs got the
  source mtime. 10 originals trashed, all 13 files restored byte-identical (`sha256sum -c`). Subtitle outputs are `subrip`.
- A second Start with every output present: all rows `Already compressed` (through `vc_mode_steps()`). `lowbitrate.mp4` was
  encoded, rejected and its part removed.
- Close mid-encode at `slower` → Stop and Quit: exit 0, no ffmpeg, empty `compressed/`.
- Not exercised in the GUI: a job with more than one step (no mode has one until images), and the pulsing bar.

Earlier phases (2026-11-08):

Phase 6, on Xvfb, `build/asan` (ASan + UBSan), run copies under `build/asan/run*`: all T14/T15 checks in `todo.md`, plus
the full run at `faster` with subtitles and auto-trash (9 trashed and restored byte-identical, subtitle outputs `subrip`). Every
session exited 0 with no sanitizer report. Use `ASAN_OPTIONS=detect_leaks=0` (GTK noise).

Phase 5, on Xvfb, `build/san` (see Build): the full fixture run at `faster` with subtitles and trash on, a cancel,
a restart, all 9 originals restored byte-identical, a second Start and a close gave exit 0 with nothing on stderr.
Closing mid-encode **was** a SIGSEGV (exit 139); now it exits 0 with no `.part`. Closing during the output check
(slow `ffprobe` wrapper) exits 0, the original is untouched and not trashed, and the output stays. The plain Debug
build also exits 0 on close mid-encode. Always check the app's exit code: wrap it as
`/bin/sh -c '"$0"; echo $? > rc' "$APP"` inside `dbus-run-session`.

Phase 2 GUI, on Xvfb: the fixture folder lists exactly the 11 videos (`notes.txt` and `compressed/` are absent).
The classes match ffprobe dimensions, portrait shows `1080p`, `fake.mp4` shows `Unreadable` and HLG shows
`10-bit HDR`. Choosing another folder replaces the list, including mid-probe (no stale rows). While
probes run, rows show `Reading…`, Start is disabled and the Speed dropdown still opens.

Phase 4 GUI, on Xvfb, on copies of the fixtures:
- The option is on and the dialog says "Up to 10 originals…". Its Cancel starts nothing. Confirming at `faster`
  takes 60 s: 9 rows show `Done – original trashed`, `lowbitrate.mp4` shows `already efficient` and stays,
  and `fake.mp4` stays. `gio list trash://` lists all 9. `gio trash --restore` brings them back, with
  matching checksums.
- Cancel during 1080p: the original and the pending file are still there, and the Trash is empty.
- Read-only folder: `Compressed – original kept (trash failed)`.
- An ffprobe wrapper that fails on `compressed/` paths gives `177.3 kB · −98% – original kept`.
- With the option off there is no dialog, the checksums are unchanged and the Trash is empty.

Phase 3 GUI, on Xvfb (default `slow`): the full fixture run took 112 s. All 9 outputs decode without
errors, `ps` showed `-preset slow`, and the bars advance with the header showing `n / 10`. Cancel kills
ffmpeg in 66 ms and leaves no `.part`. Start works again after a cancel, and closing the window
mid-encode kills ffmpeg and removes the `.part`. A second Start skips everything. Keep subtitles plus
`faster` gives `.mkv` files with `subrip` (`mov_text` becomes `-c:s srt`), and both show in VLC.

Phase 3, headless (earlier):

The app's exact argv was run on all fixtures through a throwaway driver linked against `vc_core`:

- All outputs are produced. 8-bit outputs are `hevc`/`hvc1`/`yuv420p`; the HLG output is `Main 10`,
  `yuv420p10le`, `arib-std-b67`/`bt2020`/`bt2020nc`.
- 720p: 60.9 MB → 1.64 MB, SSIM 0.9992. `testsrc` compresses far better than real footage, so
  Criterion 4 (a real 200 MB phone clip) is still the real test.
- `lowbitrate.mp4` came out at 183% of its source, so it was rejected. `fake.mp4` shows as Unreadable.
  A re-run skips everything. Original checksums are unchanged.
- With Keep subtitles on, both subtitle fixtures give `.mkv` files with a `subrip` track (`mov_text`
  is converted to `srt`). With it off, outputs have no subtitle stream.
- On 1080p, `faster` takes 10.3 s and `slow` takes 26.9 s.
