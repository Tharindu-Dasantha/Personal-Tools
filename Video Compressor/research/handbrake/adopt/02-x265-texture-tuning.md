---
title: Adopt — HandBrake's x265 texture-preserving tuning
tags: [adopt, quality, speed, x265, psy-rd, psy-rdoq, aq-mode, rskip, tuning]
source: ../13-presets.md (preset_builtin.json VideoOptionExtra), ../07-video-encoders.md, Video Compressor src/compress.c add_video_output
related: [00-index, ../13-presets, ../07-video-encoders, 01-lanczos-scaling, 07-10bit-for-8bit-sources, 05-per-encode-logs]
---

# 02 · HandBrake's x265 texture tuning

## What HandBrake does
Every x265 preset (H.265 MKV, 4K HEVC, the Apple/Chromecast/Roku HEVC presets) adds this string
(see [../13-presets](../13-presets.md)):
```
strong-intra-smoothing=0:rect=0:aq-mode=1:rd=4:psy-rd=0.75:psy-rdoq=4.0:rdoq-level=1:rskip=2
```
- **Keeps texture and grain:** no strong intra smoothing, weaker `psy-rd` (less ringing and fake sharpness),
  much stronger `psy-rdoq` (keeps grain energy during quantisation), and plain variance AQ.
- **Wins back speed:** `rect=0` (no rectangular partitions) and `rskip=2` (edge-based recursion skip).

## What we do now
`src/compress.c` (around line 367): `-c:v libx265 -preset <speed> -crf <class>` with
`-x265-params log-level=error` (plus `hdr10=1:repeat-headers=1` for PQ). Otherwise x265's defaults.

## Change
Append the string to `-x265-params`, after `log-level=error` and before the PQ flags. Keep it in one
`static const char` next to the class table so it's easy to remove. Update the argv tests.

## Gain
Unknown until measured. Two possible wins:
1. **Speed:** `rect=0` + `rskip=2` should make `slow` noticeably faster. That matters because `slow` was kept as
   default only for quality (SPEC decision 6: `medium` = 46 % of the time, 1.6–2.7 VMAF lower).
2. **Look:** phone footage keeps its grain and texture instead of turning waxy.

## Risk
- Psy tuning usually **lowers VMAF and PSNR while looking better to the eye**, so the numbers can mislead. Judge the grainy phone clip by eye as well.
- Stronger psy-rdoq can **raise the bitrate** at the same CRF. That goes against our size goal. Size per VMAF point is the deciding number.
- HandBrake tuned this for RF 20–24 (archival). Our CRF 24–28 is a different regime.

## How to verify
On `build/perf/src`, at `slow` and at `medium`, with and without the string: time, size, VMAF and a visual
check of the grainy clip. Adopt only if time drops and size-per-VMAF is no worse. Also try the speed-only part
(`rect=0:rskip=2`) by itself. It may be the safer half.

## Effort
XS to try, S including measurement and the SPEC update.
