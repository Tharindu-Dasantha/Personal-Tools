---
title: Lessons from HandBrake for Video Compressor
tags: [handbrake, video-compressor, comparison, recommendations, x265, vaapi, scaling, hdr]
source: this research (notes 01–14) compared against ../../SPEC.md and ../../src/compress.c as of 2026-11-09
related: [00-index, 06-filters, 07-video-encoders, 08-rate-control-and-quality, 12-hdr-and-color, 13-presets, 14-hardware-acceleration]
---

# Lessons for Video Compressor

A comparison of HandBrake's behaviour with our app (`SPEC.md`, `src/compress.c`, 2026-11-09). Nothing here
has been implemented. Each idea that changes output needs the same VMAF/size check used in
`tasks/plan-performance.md` before it is adopted.

## Already on par (or ahead)

| Area | HandBrake | Video Compressor | Verdict |
|---|---|---|---|
| CRF by resolution | RF rises with resolution (x265 MKV: 20/21/22/24). See [08](08-rate-control-and-quality.md) | class table, CRF 24–28 | **Same idea.** Ours is 4–6 RF higher, because it aims at size, not archival quality |
| HEVC tag in MP4 | `hvc1` unless inline parameter sets. See [11](11-muxing-and-metadata.md) | `-tag:v hvc1` | Same |
| Web-optimised MP4 | `faststart` only when `Optimize` is on | always `+faststart` | Same or better |
| Metadata and location | maps `creation_time`, ISO6709 `Location`, Android dates | `use_metadata_tags`, keeps date/location/camera (D4) | Same |
| Audio copy | `AudioCopyMask=copy:aac` + fallback `aac` 160k | copy AAC if ≤ class bitrate +10%, else AAC at class bitrate | Same idea, and ours also checks bitrate |
| Frame-rate cap | **PFR 30**: drops frames only above the peak. See [05](05-sync-and-timing.md) | `-fpsmax 30`, only when the source is > 30.5 fps | Similar |
| Rotation | Adapter filter rotates pixels physically | ffmpeg autorotate (CPU) / `transpose_vaapi` (GPU) | Same result |
| Thread priority | every thread `HB_LOW_PRIORITY` | low priority (R2) | Same |
| HLG / 10-bit sources | 10-bit pipeline, colour tags in VUI + `colr` | `yuv420p10le main10` + colour tags | Same |
| PQ (HDR10) flags | `hdr10-opt=1` | `hdr10=1:repeat-headers=1` | Same base. See the gap on mastering data below |
| VAAPI rate control | `rc_mode=CQP`, `qp=RF`. See [07](07-video-encoders.md) | `-rc_mode CQP -qp gpu_qp` | **Identical approach** |
| VAAPI pipeline | **CPU decode → NV12 upload → 8-bit only**, and not built by default on Linux. See [14](14-hardware-acceleration.md) | GPU decode, `p010` for 10-bit, GPU rotate/scale | **Ours is ahead** |
| Size estimate in CQ mode | **none** in libhb; users run a live-preview clip | 3 × 4 s GPU sample at 20/50/80 % | **Ours is ahead.** HB's preview picks evenly spaced points the same way |
| Safe output | writes the output file; no "is it smaller?" check | compressed copy checked and confirmed smaller before an original is trashed | Ours is stricter |

## Worth trying, ranked by value per effort

### 1. Use a lanczos scaler for "Max size" downscales (small change, likely small quality gain)
- **HandBrake**: `zscale filter=lanczos` (if AVX2) or `scale flags=lanczos+accurate_rnd`, never the default scaler. See [06-filters](06-filters.md).
- **Us**: `scale=w=…:h=…` with no flags, which is **swscale's default bicubic**.
- **Change**: in the CPU branch of `vc_build_ffmpeg_argv`, add `:flags=lanczos+accurate_rnd` to the `scale` filter.
  On the GPU branch, check whether `scale_vaapi` in this ffmpeg build has a `mode=hq` option (`ffmpeg -h filter=scale_vaapi`) before using it.
- **Check**: VMAF at the same CRF on one 1080p → 720p clip. Lanczos keeps more detail, which can cost a little size.

### 2. Try HandBrake's x265 tuning string on the corpus (one-line change, needs measuring)
- HB's x265 presets all add (see [13-presets](13-presets.md)):
  `strong-intra-smoothing=0:rect=0:aq-mode=1:rd=4:psy-rd=0.75:psy-rdoq=4.0:rdoq-level=1:rskip=2`
- It keeps texture and grain (weaker psy-rd, stronger psy-rdoq, no strong intra smoothing) and wins back speed (`rect=0`, `rskip=2`).
- **Change**: append these to our `-x265-params` (after `log-level=error`).
- **Check**: VMAF, size and time on the `build/perf` corpus at `slow`. Keep it only if VMAF per byte is no worse. Psy tuning often **lowers VMAF while looking better to the eye**, so also judge the grainy phone clip by eye.

### 3. Pass HDR10 mastering-display and MaxCLL through (medium effort, only matters for PQ sources)
- **HandBrake** sets `master-display=G(..)B(..)R(..)WP(..)L(..)` and `max-cll=` from the source side data. See [12-hdr-and-color](12-hdr-and-color.md).
- **Us**: the SPEC says "Mastering-display/MaxCLL values are not passed through".
- **Change**: read them with `ffprobe -select_streams v:0 -read_intervals %+#1 -show_frames -show_entries frame=side_data_list` (side data types "Mastering display metadata" and "Content light level metadata"). Convert the rationals into x265 units (chroma ×50000, luma ×10000, the same as HB's `MASTERING_CHROMA_DEN/LUMA_DEN`) and append them to the PQ `-x265-params`.
- iPhone footage is **HLG**, which needs neither, so this only helps PQ (HDR10) files.

### 4. Measure 10-bit encoding for 8-bit sources (needs a SPEC decision)
- **HandBrake** encodes **every** HEVC preset as `x265_10bit main10`, even from 8-bit sources. 10-bit precision reduces banding and usually improves efficiency.
- **Us**: SPEC decision "8-bit sources stay 8-bit" (Criterion 12).
- **If tried**: encode the 8-bit corpus with `-pix_fmt yuv420p10le -profile:v main10` and compare VMAF/size/time. It costs some encode time, and very old devices may not play Main10. **The user decides** before any change, because it reverses a written SPEC decision.

### 5. Selective deinterlacing for old camcorder files (only if such files appear)
- **HandBrake** always runs comb detect + selective decomb, so only combed frames are touched.
- **Us**: no deinterlacing. Phone and modern camera footage is progressive, so this is only for old 480i/576i clips.
- **Option**: add `bwdif=deint=interlaced` to the CPU filter chain. It only deinterlaces frames flagged as interlaced and passes progressive frames through.

## Looked at and not worth doing

| HandBrake feature | Why skip |
|---|---|
| Keyint `min=fps`, `max=10×fps` | x265's default `keyint=250` is close (300 at 30 fps). Negligible size effect |
| Decoder threads `cpu/2+1` | The ffmpeg CLI already balances its threads. We have no libavcodec code to tune |
| Auto-crop (median of previews, see [02](02-scan-and-probe.md)) | Phone videos have no letterbox. A wrong crop would destroy content |
| Own A/V sync repair (gap fill, dejitter) | The ffmpeg CLI handles normal phone/camera files. HB needs it for DVD/TS sources |
| Multi-pass / ABR | We use quality mode, and HB also turns multi-pass off for CQ |

## Related
- All findings start from [00-index](00-index.md)
- App spec: [../../SPEC.md](../../SPEC.md), performance plan: [../../tasks/plan-performance.md](../../tasks/plan-performance.md)
