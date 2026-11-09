---
title: Adopt? — auto-crop black bars
tags: [adopt, size, optimization, autocrop, cropdetect, letterbox, black-bars]
source: ../02-scan-and-probe.md (HandBrake smooth-border detection, median over previews, loose-crop switch), Video Compressor src/compress.c vc_build_sample_argv
related: [00-index, ../02-scan-and-probe, ../06-filters, 01-lanczos-scaling, 12-selective-deinterlace]
---

# 13 · Auto-crop black bars

> High risk if it's wrong (it cuts away picture), and phone videos rarely have bars. Only consider it if
> letterboxed files (screen recordings, movie rips, vertical video with bars) are common in your folders.

## What HandBrake does
Its auto-crop is careful (`scan.c:449-570`, `1384-1460`, see [../02-scan-and-probe](../02-scan-and-probe.md)):
- A row or column counts as border only if its **average luma < 32 and every pixel is within ±16 of that average**: a smooth border, not just a dark scene.
- It ignores a thin outer line (1 % of the height), searches at most ¼ of the frame per side, and **throws away a preview's result if any side would be ≥ ¼** (black title cards and fades).
- It takes the **median** over ≥ 3 previews, and **switches to the loosest crop** when many previews disagree (mixed aspect ratio). Values are rounded to even.

## What we do now
No cropping. The bars are encoded. They cost few bits (flat black compresses well) but still cost pixels, so encode time goes up.

## Change
1. Run `cropdetect` on the **same windows the sample encode already decodes** (20/50/80 %): add a `cropdetect=limit=24:round=2` branch and parse the last `crop=w:h:x:y` per window from stderr.
2. Use HandBrake's acceptance rules: crop only if all windows agree within ~9 px, no side is ≥ ¼, and take the **loosest** (smallest crop) of the three.
3. Apply `crop=` **before** `scale` (HandBrake's order: crop+scale is one step).
4. Make it opt-in (an "Auto-crop black bars" switch, off by default), since a wrong crop destroys picture.

## Gain
On letterboxed files: 5–25 % fewer pixels, which means proportionally faster encodes and a slightly smaller file. Nothing for normal phone clips.

## Risk
**High** if it gets the crop wrong. That is why it's opt-in, uses the loosest crop, and requires agreement across windows.

## How to verify
Fixtures: a letterboxed clip (pad a test source with black bars), a dark-scene clip with no bars (must not crop), and a clip with a black intro (must not over-crop).

## Effort
M.
