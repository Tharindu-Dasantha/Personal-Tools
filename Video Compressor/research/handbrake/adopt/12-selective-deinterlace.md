---
title: Adopt — selective deinterlacing
tags: [adopt, quality, deinterlace, comb-detect, decomb, bwdif, camcorder, interlaced]
source: ../06-filters.md (comb detect + decomb selective mode), ../02-scan-and-probe.md (interlace detection over previews), ../13-presets.md
related: [00-index, ../06-filters, ../02-scan-and-probe, 13-auto-crop, 11-robust-probing]
---

# 12 · Selective deinterlacing

## What HandBrake does
- Every General/Device/Matroska preset enables **Comb Detect + Decomb** (see [../13-presets](../13-presets.md)).
- With comb detect present, decomb runs in **selective mode** (`MODE_DECOMB_SELECTIVE`): only frames that actually show combing are deinterlaced, and progressive frames pass through untouched (see [../06-filters](../06-filters.md)).
- The scan flags a title as interlaced when ≥ half of its previews comb (`scan.c:1552`).

## What we do now
No deinterlacing. Interlaced sources (old DV/HDV camcorders, 480i/576i/1080i TV recordings) are encoded with their combing, which looks bad **and** costs bits: x265 spends bitrate on the comb teeth.

## Change
Only if such files are found in real use:
1. Probe `field_order` (`stream=field_order`). Values `tt`, `bb`, `tb` and `bt` mean interlaced.
2. For those files, CPU path: add `bwdif=mode=send_frame:deint=interlaced` first in `-vf`.
   `deint=interlaced` touches only frames flagged as interlaced, which is ffmpeg's version of "selective".
3. GPU path: try `deinterlace_vaapi` (check its options in this build), or send such files to x265.
4. Optional: a `Interlaced` chip in the row, like the `10-bit` chip.

## Gain
Clean motion and a smaller file for interlaced sources. Nothing changes for progressive ones (phones).

## Risk
Low, because it only runs when probing says the file is interlaced. Some files are flagged interlaced but are really progressive; with `deint=interlaced` those frames still pass through if the frame flags say progressive.

## How to verify
Make an interlaced fixture (`ffmpeg -f lavfi -i testsrc2=size=720x576:rate=50 -vf tinterlace=interleave_top,fieldorder=tff -flags +ildct+ilme`), encode it, and check for combing by eye. Check that a progressive fixture's argv has no bwdif.

## Effort
S.
