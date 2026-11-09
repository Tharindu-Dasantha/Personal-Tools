---
title: Adopt — HDR10 mastering display and MaxCLL passthrough
tags: [adopt, hdr, hdr10, pq, mastering-display, maxcll, x265, ffprobe]
source: ../12-hdr-and-color.md (HandBrake encx265.c master-display / max-cll), Video Compressor SPEC.md Bit depth / HDR
related: [00-index, ../12-hdr-and-color, ../07-video-encoders, 07-10bit-for-8bit-sources, 11-robust-probing]
---

# 06 · HDR10 mastering display and MaxCLL

## What HandBrake does
For PQ (SMPTE 2084) + BT.2020 sources, x265 gets (`encx265.c`, see [../12-hdr-and-color](../12-hdr-and-color.md)):
- `hdr10-opt=1`
- `master-display=G(gx,gy)B(bx,by)R(rx,ry)WP(wx,wy)L(max,min)`: chromaticities × **50000**, luminance × **10000** (`MASTERING_CHROMA_DEN`/`MASTERING_LUMA_DEN`)
- `max-cll=MaxCLL,MaxFALL`

These come from the source's side data. Without them an HDR10 TV guesses the content's brightness range and may clip highlights or show the picture too dark.

## What we do now
The SPEC says: "PQ (HDR10) sources also get `hdr10=1:repeat-headers=1` … Mastering-display/MaxCLL values are not passed through." iPhone footage is **HLG**, which needs none of this.

## Change
1. During probing, **only for PQ files**, run one extra ffprobe:
   `ffprobe -v error -select_streams v:0 -read_intervals %+#1 -show_frames -show_entries frame=side_data_list -of default=nw=1 <file>`
   and parse "Mastering display metadata" (`red_x`, `red_y`, … `max_luminance`, `min_luminance` as rationals) and "Content light level metadata" (`max_content`, `max_average`).
2. Add the fields to `VcProbe`, plus a pure function that formats the x265 strings (with rounding identical to HandBrake's `hb_rescale_rational`). Unit-test it against a known HDR10 sample's values.
3. Append to the PQ `-x265-params`: `hdr10-opt=1:master-display=…:max-cll=…`.
4. GPU path: `hevc_vaapi` has no such options in ffmpeg as far as we know. Check `ffmpeg -h encoder=hevc_vaapi`. If it can't, PQ files could go to x265 the way odd files do in [04](04-gpu-to-cpu-fallback.md), or the limit is written down.

## Gain
Proper HDR10 output from HDR10 sources (downloaded HDR clips, some Android phones, cameras).

## Risk
Low. Values are only added when present and parsed correctly. Wrong values would tone-map badly, which is why the function is unit-tested.

## How to verify
Generate a PQ fixture with known mastering/CLL side data, encode it, and check `ffprobe -show_frames` on the output for the same values.

## Effort
M (probe parsing, formatting, tests). Do it only if real HDR10 files turn up. HLG doesn't need it.
