---
title: HandBrake video filters
tags: [handbrake, filters, deinterlace, decomb, comb-detect, crop, scale, lanczos, zscale, avfilter, denoise, rotation]
source: libhb/work.c, libhb/hbavfilter.c, libhb/cropscale.c, libhb/adapter.c, libhb/decomb.c, libhb/comb_detect.c, libhb/handbrake/common.h
related: [00-index, 01-architecture-pipeline, 02-scan-and-probe, 05-sync-and-timing, 07-video-encoders, 12-hdr-and-color, 14-hardware-acceleration]
---

# Video filters

## Fixed filter order

Filters always run in the order of the `HB_FILTER_*` enum (`handbrake/common.h:1752-1797`), whatever order
the user enabled them in:

```
ADAPTER → DETELECINE → COMB_DETECT → DECOMB / YADIF / BWDIF → VFR(*) → DEBLOCK → DEBAND
→ DENOISE(hqdn3d) / BM3D / NLMEANS → CHROMA_SMOOTH → ROTATE → RENDER_SUB → CROP_SCALE
→ LAPSHARP / UNSHARP → GRAYSCALE → PAD → COLORSPACE → FORMAT → RPU → AVFILTER
```
(*) In practice the **framerate shaper runs last**, at the end of render. See [05-sync-and-timing](05-sync-and-timing.md).
Most filters also have a `_VT` variant for VideoToolbox (the Apple GPU path).

The order makes sense for quality: deinterlace on the original fields, then denoise, then burn subtitles,
then **scale**, then sharpen at the final size, then pad, then colour conversion.

## Adapter (always added, `adapter.c`)
Inserted by `sanitize_filter_list_pre()` (`work.c:1384`). It:
- **Physically rotates** frames by `title->rotation` from the display matrix (with width/height swapped for 90/270). The output therefore needs no rotation flag.
- Converts the decoder's pixel format to the pipeline pixel format (it reads the real `sw_format` out of a hw frames context).
- Re-initialises when the input format or size changes mid-stream.

## Deinterlacing: comb detect + decomb
- **Comb Detect** (`comb_detect.c`, defaults `3:2:3:3:2:40:16:16`) marks frames that show combing artefacts. It uses slice threads through `taskset`.
- **Decomb** (`decomb.c`) mode bits: 1 = yadif, 2 = blend, 4 = cubic, 8 = EEDI2, 16 = bob, **32 = selective**.
- When comb detect is present, `sanitize_filter_list_pre` sets `MODE_DECOMB_SELECTIVE` on decomb/yadif/bwdif, so **only combed frames are deinterlaced**. Progressive frames pass through untouched.
- Every General/Device/Matroska preset has `PictureDeinterlaceFilter=decomb` + `PictureCombDetectPreset=default`. Progressive phone or camera footage is unaffected.
- Detelecine (`pullup`) handles 3:2 pulldown.

## Crop and scale (`cropscale.c`)
- **Skipped entirely** when the output size equals the source and there is no crop (`work.c:1440`, "skipping crop/scale filter").
- Which scaler is used (`cropscale.c:~130-195`):

| Condition | Filter |
|---|---|
| CUDA frames | `scale_cuda` with `interp_algo=lanczos` (`colorspace_cuda` first for range) |
| D3D11 frames | `scale_d3d11` |
| AMF frames | `vpp_amf`, bilinear |
| QSV (Windows) | `vpp_qsv` with `scale_mode=hq` |
| CPU, **AVX2 present**, even sizes, a supported pix_fmt (`hb_av_can_use_zscale`, `hbffmpeg.c:870`) | **`zscale` with `filter=lanczos`** (the zimg library) |
| CPU, otherwise | **`scale` (swscale) with `flags=lanczos+accurate_rnd`** |

- The preset key `VideoScaler` exists (default `"swscale"`), but the code decides as shown above.
- PAR is recomputed after crop+scale (`hb_limit_rational`, limited to 65535).
- `PictureModulus=2` keeps dimensions even, as 4:2:0 requires.
- Presets set `PictureUseMaximumSize=true` and `PictureAllowUpscaling=false`: **downscale to fit the box, never upscale**.

## avfilter merging (`hbavfilter.c:518`)
HandBrake filters that are libavfilter underneath (yadif, bwdif, deblock, BM3D, deband, crop/scale, pad,
rotate, colorspace, grayscale, format) are **merged into one libavfilter graph** when they sit next to each
other (`hb_avfilter_combine`). That saves frame copies and thread hand-offs. Native filters (decomb,
nlmeans, hqdn3d, lapsharp, rendersub, the vfr shaper) break the chain.

## Other filters (short list)
| Filter | Notes |
|---|---|
| Denoise **hqdn3d** | fast spatio-temporal denoiser; preset `light`/`medium`/`strong` |
| **NLMeans** | slow, high quality; tunes for film/grain/animation; x86 SIMD (`nlmeans_x86.c`) |
| **BM3D**, **Deband** | new in 1.12.0 (through avfilter) |
| Chroma smooth, Lapsharp, Unsharp | sharpening and chroma clean-up |
| Rotate | user rotate/flip through avfilter `transpose`/`hflip`/`vflip` |
| Render sub | burns in subtitles (libass for SSA, bitmap for PGS/VobSub). See [10-subtitles](10-subtitles.md) |
| Pad, Grayscale | |
| Colorspace | converts primaries/transfer/matrix/range, **optional tone mapping**. See [12-hdr-and-color](12-hdr-and-color.md) |
| Format | the final pixel format conversion for the encoder |
| RPU | rewrites Dolby Vision RPU after crop/scale. See [12-hdr-and-color](12-hdr-and-color.md) |

## Pixel format negotiation
`hb_get_best_pix_fmt()` picks the richest pipeline format that does not exceed the source's bit depth or
chroma resolution. **Biplanar formats (NV12/P010) are only allowed with a hw decoder**, and most native
filters need planar input. See [12-hdr-and-color](12-hdr-and-color.md).

## Filter init failure
If a filter's `init()` fails, it is **removed with a log line and the encode goes on** (the filter-init loop in `do_job`, `work.c`). This is a deliberate degrade, not an abort.

## Related
- Pipeline context: [01-architecture-pipeline](01-architecture-pipeline.md)
- GPU filter variants: [14-hardware-acceleration](14-hardware-acceleration.md)
- Our scaler compared with HandBrake's: [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)
