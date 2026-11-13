---
title: Adopt? — 10-bit HEVC for 8-bit sources
tags: [adopt, quality, size, 10-bit, main10, banding, x265, spec-decision]
source: note 13-presets (all HEVC presets use x265_10bit), note 12-hdr-and-color, Video Compressor SPEC.md (Criterion 12, "8-bit sources stay 8-bit")
related: [00-index, note 13-presets, note 12-hdr-and-color, 02-x265-texture-tuning, 06-hdr10-static-metadata]
---

# 07 · 10-bit HEVC for 8-bit sources

> **Needs a decision from the user.** It reverses a written SPEC decision ("8-bit sources stay 8-bit").

## What HandBrake does
**Every** HEVC CPU preset uses `x265_10bit` + `main10`, whatever the source bit depth (see
`13-presets`). The pipeline stays at the source depth (`hb_get_best_pix_fmt` never goes
above it), and only the final Format filter converts 8 → 10 bit for the encoder (see `12-hdr-and-color`).

The reason: x265's internal maths and reference frames are more precise at 10 bits. That reduces **banding in
gradients** (skies, walls) and often gives **the same quality at a smaller size**, even from 8-bit input.

## What we do now
8-bit sources get `-pix_fmt yuv420p` (Main). 10-bit sources get `yuv420p10le -profile:v main10`.

## Change (experiment first)
For 8-bit sources on the x265 path: `-pix_fmt yuv420p10le -profile:v main10`, with **no** colour tags added
(an 8-bit SDR source has no HDR tags to copy). The GPU path stays 8-bit unless measured separately (`hevc_vaapi` main10 with `format=p010`).

## Gain
Possibly a few % smaller at the same VMAF, plus less banding at our fairly high CRFs (24–28), which is exactly where banding shows.

## Risk
- **Playback:** Main10 HEVC plays on modern phones, TVs and browsers with HEVC support, but some older devices and hardware decoders only do Main (8-bit). That is the reason the SPEC chose 8-bit.
- **Speed:** 10-bit x265 is somewhat slower. Measure it.
- The `10-bit` chip in the UI would no longer mean "the source is 10-bit". Keep the chip tied to the source.

## How to verify
On the 8-bit clips in `build/perf/src`: size, VMAF, time and a visual banding check (a sky or gradient clip)
at `slow`, 8-bit against 10-bit. Write the result into `LOG.md`, then decide.

## Effort
XS to try. The argv change is small. The SPEC and test updates come only if adopted.
