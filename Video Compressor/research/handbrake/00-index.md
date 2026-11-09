---
title: HandBrake internals — research index
tags: [handbrake, research, index, video-compression, decoding]
source: HandBrake git master b3d5769 (2026-11-07), release line 1.12.0
related: [01-architecture-pipeline, 15-lessons-for-video-compressor]
---

# HandBrake internals: research index

How HandBrake (libhb) reads, decodes, filters, encodes and muxes video. Everything here comes from reading
the HandBrake source (`github.com/HandBrake/HandBrake`, master `b3d5769`, 2026-11-07, release line
**1.12.0**) plus the official docs at handbrake.fr. File references like `libhb/work.c` point into that tree.

## Why this exists

The [Video Compressor](../../SPEC.md) does a HandBrake-style "keep the quality, cut the size" job by calling
`ffmpeg`. These notes show what HandBrake does at each stage, so we can copy what is worth copying. The
ranked list is in [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md).

## Map of the notes

| # | Note | Stage | One-line summary |
|---|------|-------|------------------|
| 01 | [Architecture & pipeline](01-architecture-pipeline.md) | all | Threaded work objects linked by bounded FIFOs, a 90 kHz clock, and one job per pass |
| 02 | [Scan & probe](02-scan-and-probe.md) | input | libavformat probe (15 MB), 10 preview decodes, auto-crop, interlace and frame-rate detection |
| 03 | [Demux & reader](03-demux-and-reader.md) | input | Own TS/PS demuxer or libavformat, packets go to per-stream FIFOs, seeking for previews |
| 04 | [Decoding](04-decoding.md) | decode | libavcodec with CPU/2+1 threads, frame+slice threading, error concealment, optional hw decode |
| 05 | [Sync & timing](05-sync-and-timing.md) | timing | A/V alignment, dejitter, gap fill and overlap drop; CFR/VFR/PFR frame-rate shaper |
| 06 | [Filters](06-filters.md) | filter | Fixed filter order, comb-detect + decomb, lanczos crop/scale, avfilter graph merging |
| 07 | [Video encoders](07-video-encoders.md) | encode | x265/x264/SVT-AV1 native; all hardware encoders through libavcodec (`encavcodec`) |
| 08 | [Rate control & quality](08-rate-control-and-quality.md) | encode | Constant Quality (RF) vs average bitrate, multi-pass, recommended RF ranges |
| 09 | [Audio](09-audio.md) | audio | Passthru, copy mask and fallback, mixdown, default bitrate formula, AAC encoder choice |
| 10 | [Subtitles](10-subtitles.md) | subs | Foreign audio search, burn-in vs passthru, SSA merging |
| 11 | [Muxing & metadata](11-muxing-and-metadata.md) | output | libavformat MP4/MKV/WebM, `hvc1` tag, faststart, interleaving, metadata mapping |
| 12 | [HDR & colour](12-hdr-and-color.md) | colour | Colour tag passthrough, HDR10 static metadata, Dolby Vision and HDR10+, tone mapping |
| 13 | [Presets](13-presets.md) | config | Built-in preset values (RF, encoder preset, frame rate, audio) and the custom x265 tuning string |
| 14 | [Hardware acceleration](14-hardware-acceleration.md) | hw | hwaccel registry, full-GPU pipeline rules, VAAPI encode-only and off by default on Linux |
| 15 | [Lessons for Video Compressor](15-lessons-for-video-compressor.md) | ours | What we already match and what is worth trying, ranked |
| → | [adopt/](adopt/00-index.md) | ours | **One note per technique to adopt**, each with the change, gain, risk, verification and effort, ranked in [adopt/00-index](adopt/00-index.md) |

## The pipeline at a glance

```
file ──► Reader ──► Decoder ──► Sync ──► Filters ──► Encoder ──► Muxer ──► output
          (demux)   (libavcodec) (A/V     (adapter,   (x265 /     (libavformat
                                 align)   decomb,     encavcodec)  mp4/mkv)
                                          crop/scale,
                                          vfr)
          audio ──► audio decoder ──► Sync ──► audio filters ──► audio encoder ──┘
          subs  ──► subtitle decoder ──► Sync ──► (burn into video | subtitle encoder) ─┘
```

Every arrow is a bounded FIFO and every box is its own thread. See [01](01-architecture-pipeline.md).

## Key concepts (graph anchors)

- **Work object** (`hb_work_object_t`): one pipeline stage with `init/work/close`, run by `hb_work_loop`. See [01](01-architecture-pipeline.md).
- **Filter object** (`hb_filter_object_t`): a video or audio filter stage, run by `filter_loop`. See [06](06-filters.md).
- **FIFO** (`hb_fifo_t`): a bounded queue with a wake threshold; it provides backpressure. See [01](01-architecture-pipeline.md).
- **Title**: one scanned source (a file, or one DVD/Blu-ray title). See [02](02-scan-and-probe.md).
- **Job**: one encode request. It is split into passes (subtitle scan, analysis, final). See [01](01-architecture-pipeline.md) and [08](08-rate-control-and-quality.md).
- **90 kHz timebase**: every internal timestamp uses MPEG's 90 kHz clock. See [03](03-demux-and-reader.md) and [05](05-sync-and-timing.md).
- **RF (Rate Factor)**: HandBrake's name for CRF/CQ. See [08](08-rate-control-and-quality.md).
- **hwaccel**: a registered hardware decoder, which can enable a full GPU pipeline. See [14](14-hardware-acceleration.md).
- **Preset**: a JSON bundle of job settings. See [13](13-presets.md).

## Sources

- HandBrake source: <https://github.com/HandBrake/HandBrake> (read at commit `b3d5769`)
- Docs, Adjust quality: <https://handbrake.fr/docs/en/1.6.0/workflow/adjust-quality.html>
- Docs, Frame rates: <https://handbrake.fr/docs/en/1.6.0/technical/frame-rates.html>
