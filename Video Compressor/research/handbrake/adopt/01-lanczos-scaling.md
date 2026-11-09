---
title: Adopt — lanczos scaler for downscales
tags: [adopt, quality, scaling, lanczos, swscale, scale_vaapi, max-size]
source: ../06-filters.md (HandBrake cropscale.c), Video Compressor src/compress.c add_video_output
related: [00-index, ../06-filters, 02-x265-texture-tuning, 13-auto-crop]
---

# 01 · Lanczos scaler for downscales

## What HandBrake does
It never uses the default scaler (`libhb/cropscale.c`, see [../06-filters](../06-filters.md)):
- CPU with AVX2: `zscale` with `filter=lanczos`
- CPU otherwise: `scale` with `flags=lanczos+accurate_rnd`
- GPU paths: the vendor scaler in its high-quality mode (`scale_cuda interp_algo=lanczos`, `vpp_qsv scale_mode=hq`)

It also **skips the scaler entirely** when the size doesn't change. We already do that.

## What we do now
`src/compress.c` `add_video_output` (around line 345) adds
`scale=w='if(gt(iw,ih),-2,N)':h='if(gt(iw,ih),N,-2)'` with **no `flags`**, so swscale uses its default,
**bicubic**. The GPU path uses `scale_vaapi` with its default mode.

## Change
- CPU path: append `:flags=lanczos+accurate_rnd` to the `scale` filter string.
- GPU path: check `ffmpeg -h filter=scale_vaapi` for a `mode` option. If this build has it, add `:mode=hq`.
  This only matters when **Max size** is on.
- Update the argv unit test in `tests/test_compress.c` that checks the scale filter string.

## Gain
Sharper downscales (1080p → 720p and so on) with less aliasing. Lanczos is the standard choice for
downscaling. Usually a small VMAF gain at the same CRF.

## Risk
Lanczos keeps more detail, and detail costs bits: the file may grow by a few percent at the same CRF.
`accurate_rnd` costs a little CPU, which is negligible next to x265.

## How to verify
Encode two clips with Max 720p at `slow`, with and without the flag. Compare VMAF (computed against the
source scaled to 720p with the same lanczos flags), size and time.

## Effort
XS: one string and one test.
