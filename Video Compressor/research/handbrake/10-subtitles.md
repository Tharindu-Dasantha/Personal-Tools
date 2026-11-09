---
title: HandBrake subtitles
tags: [handbrake, subtitles, burn-in, foreign-audio-search, ssa, pgs, vobsub, cc608, tx3g]
source: libhb/work.c (analyze_subtitle_scan, sanitize_subtitles), libhb/rendersub.c, libhb/sync.c, libhb/decavsub.c
related: [00-index, 01-architecture-pipeline, 05-sync-and-timing, 06-filters, 11-muxing-and-metadata]
---

# Subtitles

Our app only drops or keeps subtitle streams, so this note is short.

## Two destinations
| Destination | Meaning |
|---|---|
| **Burn-in** (`RENDERSUB`) | Rendered into the video by the `rendersub` filter (libass for SSA/ASS/SRT, bitmap blend for PGS/VobSub) |
| **Passthru / soft** (`PASSTHRUSUB`) | Kept as a separate track. It may be converted (for example text → `tx3g` for MP4, or SSA for MKV) |

## Sanitizing (`sanitize_subtitles`, `work.c:996`)
- **Only one burned track** is allowed. Extra burn requests become soft subtitles, or are dropped if they can't be soft either.
- A track that can't be rendered is changed to soft.
- A track that **can't go into the container** as soft (for example PGS bitmaps in MP4) is **burned** if no track is burned yet, otherwise dropped.
- The preset keys `SubtitleBurnBehavior` (`foreign` / `first` / `none`), `SubtitleBurnBDSub` and `SubtitleBurnDVDSub` control the defaults.

## Foreign Audio Search (`SubtitleAddForeignAudioSearch`)
Finds the subtitle track that only appears when characters speak another language:
1. An extra **subtitle scan pass** runs first (`HB_PASS_SUBTITLE`, the pipeline ends at sync, see [01-architecture-pipeline](01-architecture-pipeline.md)).
2. It counts **hits** and **forced hits** for each candidate track (`analyze_subtitle_scan`, `work.c:910`).
3. It picks the track with forced subs if one exists (when forced-only is set), or else a track whose hit count is **under about 10%** of the most frequent track.
4. The result is passed to the encode pass through the interjob struct.

## Timing
- SSA tracks can deliver every event at once, so subtitle FIFOs are **unbounded**. Bounded queues would deadlock the reader. See [05-sync-and-timing](05-sync-and-timing.md).
- Sync **merges overlapping SSA events** (`mergeSubtitleOverlaps`, `merge_ssa`) and fixes durations.
- Closed captions (CC608) are pulled from the video bitstream during decode.

## Related
- Burn-in filter position (before crop/scale): [06-filters](06-filters.md)
- Container rules: [11-muxing-and-metadata](11-muxing-and-metadata.md)
