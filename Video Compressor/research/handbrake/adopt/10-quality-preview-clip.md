---
title: Adopt — quality preview clip before the batch
tags: [adopt, ux, preview, live-preview, sample-encode, quality-check]
source: ../03-demux-and-reader.md (start_at_preview / seek_points, pts_to_stop), ../02-scan-and-probe.md (evenly spaced previews), Video Compressor src/compress.c vc_build_sample_argv
related: [00-index, ../03-demux-and-reader, ../08-rate-control-and-quality, 02-x265-texture-tuning, 07-10bit-for-8bit-sources]
---

# 10 · Quality preview clip

## What HandBrake does
**Live Preview**: the user picks one of the evenly spaced preview points (scan default 10; the reader seeks to
`(k-1)/(seek_points+1)` of the duration) and a length, and HandBrake encodes **just that clip with the real
settings** (`start_at_preview`, `pts_to_stop`, see [../03-demux-and-reader](../03-demux-and-reader.md)), then plays
it. Since libhb can't predict quality or size in constant-quality mode, this is how users decide on an RF or
preset before a long encode (see [../08-rate-control-and-quality](../08-rate-control-and-quality.md)).

## What we do now
- Sample encodes already exist, but only for **size** (3 × 4 s, GPU, piped to null, video only).
- To see what `slow` CRF 27 looks like, the user has to compress a whole file.

## Change
A **Preview** action on a row (right-click and a hover button):
1. Encode **10 s from the middle** with the **exact** argv for the current speed and options (`vc_build_ffmpeg_argv` plus `-ss mid-5 -t 10` before `-i`), into a temp `.mp4` in `g_get_user_cache_dir()`.
2. Open it with the default player (`gtk_file_launcher_launch`), or better, open the **same 10 s of the original** too, so they can be compared.
3. Show the clip's size scaled to the full duration as a third, real estimate.
4. Delete the temp files when the app closes.

Reuse the sample-encode rules: low priority, one at a time, never while a batch runs.

## Gain
The user can judge speed presets, Max size and future options ([02](02-x265-texture-tuning.md), [07](07-10bit-for-8bit-sources.md)) by eye in 10–60 s, instead of after a full encode.

## Risk
Low. It is an extra action and the batch flow doesn't change. A 10 s x265 `slow` clip at 1080p takes about 30–60 s
on this laptop, so the row needs a progress state and a way to cancel.

## How to verify
Preview at GPU and `slow`: the clip plays, its colour tags and rotation match the full encode's, and temp files are gone after the app closes.

## Effort
M (UI state, temp file handling, launching).
