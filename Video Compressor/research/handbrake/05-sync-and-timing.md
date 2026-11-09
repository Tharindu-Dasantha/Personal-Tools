---
title: HandBrake sync and frame-rate handling
tags: [handbrake, sync, av-sync, timestamps, vfr, cfr, pfr, framerate, dejitter]
source: libhb/sync.c, libhb/vfr.c, libhb/work.c
related: [00-index, 03-demux-and-reader, 04-decoding, 06-filters, 09-audio, 13-presets]
---

# Sync and frame-rate handling

Two parts:
1. **Sync** (`sync.c`, about 3,500 lines) repairs timestamps and aligns video, audio and subtitle streams **before** filtering.
2. The **framerate shaper** filter (`vfr.c`) runs at the **end** of the video filter chain and enforces the output frame-rate mode.

## Sync work objects

There is one sync work object per stream type (`syncVideoWork`, `syncAudioWork`, `syncSubtitleWork`). They
share one `sync_common_t` under a lock. Each stream has an `in_queue` with limits:

| Stream | max / min queue | Why |
|---|---|---|
| Video | 40 / 20 | |
| Audio | 200 / 30 | "Audio is small, buffer a lot", so the first PTS of every stream is seen before choosing the zero point |
| Subtitle | unlimited / 0 | SSA files can deliver every subtitle at once, and blocking would deadlock the reader |

## What sync does

| Function | Job |
|---|---|
| `computeInitialTS` / `checkFirstPts` | Find the earliest PTS across all streams and treat it as **zero**. Video durations are derived from start-time differences |
| `alignStreams` / `alignStream` | Streams that start late get a **gap filler**: `CreateBlackBuf` for video, `CreateSilenceBuf` for audio. Passthru audio is trimmed by **dropping** frames instead (it can't be synthesised) |
| `UpdateSCR` / `ProcessSCRDelayQueue` | Applies the fine SCR offset for TS/PS clock jumps (see [03-demux-and-reader](03-demux-and-reader.md)) |
| `dejitterVideo` / `removeVideoJitter` | Finds runs where the timestamp spacing jitters around the nominal frame duration (soft telecine, bad muxers) and **smooths** them |
| `fixVideoOverlap` | A frame whose whole duration lies in the past (time went backwards) is **dropped**, and the dropped duration is logged |
| `dejitterAudio` / `fixAudioOverlap` | Audio overlap: frames are dropped or timestamps shifted |
| `fixAudioGap` (`sync.c:1056`) | A gap of **more than 20 ms and less than 60 s** is **filled with silence**. A gap of 60 s or more is treated as a corrupt timestamp and is not filled |
| `addDelta` / `applyDeltas` | Spreads timestamp corrections across streams so A/V stay locked |
| `checkCadence` | Tracks the telecine/progressive cadence for logging |
| `sanitizeSubtitle`, `mergeSubtitleOverlaps`, `merge_ssa` | Subtitle overlaps are merged into combined events |
| `OutputBuffer` | Emits buffers in timestamp order and enforces `frame_to_stop` / `pts_to_stop` |

The `AlignAVStart` preset key (off by default) pads streams so audio and video **both start at 0**, which some players need.

## Framerate shaper (`vfr.c`)

It is always the **last** filter in the render chain, because other filters may drop or duplicate frames.

Three modes (`vfr.c:195-210`, the `mode` setting, from the preset's `VideoFramerateMode`):

| Mode | Value | Behaviour |
|---|---|---|
| **VFR**, "same as source" | 0 | Frame times are left as they are |
| **CFR**, constant | 1 | Frames are placed exactly `vrate.den` ticks apart, **dropping or duplicating** as needed |
| **PFR**, peak | 2 | `vrate` is a **ceiling**: frames are dropped only when the running average would go over the peak. Slower sections stay untouched |

How drops are chosen (`frame-drop-mode`, reworked in 1.12.0 "to be more deterministic"):
- `NEAREST` (0) is the default: it drops the frame nearest the CFR boundary.
- `MOTION_ANALYSIS` (1) keeps a window of frames (`frame_rate_list`) and uses a **motion metric** (`motion_metric.c`) to drop the frame **most similar to its neighbour**, which is the least visible drop.
- `AUTO` (2).

Duplicates are **shallow copies** (`hb_buffer_shallow_dup`), so they cost no extra pixel memory.

The shaper logs `drops`, `dups` and lost/gained time.

### How the presets use it
- The General/Device/Matroska presets use **PFR at 30 fps** (or 60 or 25): a 60 fps phone clip is capped at 30, while 24/25/30 fps content is left alone. See [13-presets](13-presets.md).
- The hardware presets use **VFR, same as source**.
- The Production presets use **CFR** (editors need it).

The official docs say the same: "Same as source + Variable" copies the timing; "Same as source + Constant"
makes variable parts constant at the detected rate; "Peak" limits higher-rate sections to the peak.

## Interjob frame-rate correction
In multi-pass encodes, pass 1 measures the real output frame count and `correct_framerate()` (`work.c:874`)
gives pass 2 an exact rate. See [08-rate-control-and-quality](08-rate-control-and-quality.md).

## Related
- [06-filters](06-filters.md) shows where the shaper sits
- [09-audio](09-audio.md) covers silence fill and passthru drops
- Our `-fpsmax 30` compared with PFR: [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)

Source: <https://handbrake.fr/docs/en/1.6.0/technical/frame-rates.html>
