---
title: HandBrake demux and reader
tags: [handbrake, demux, reader, libavformat, timestamps, 90khz, seeking, preview-encode]
source: libhb/reader.c, libhb/stream.c, libhb/demuxmpeg.c
related: [00-index, 01-architecture-pipeline, 02-scan-and-probe, 04-decoding, 05-sync-and-timing]
---

# Demux and reader

## Reader work object (`reader.c`)

`hb_reader` is the **source** of the pipeline. It has no input FIFO. Each `reader_work()` call
(`reader.c:452`):
1. Checks the chapter. Past `chapter_end` or at end of title, it sends EOF to every FIFO and returns `HB_WORK_DONE`.
2. Reads one block: `hb_bd_read` (Blu-ray), `hb_dvd_read` (DVD) or `hb_stream_read` (file).
3. Runs the title's **demuxer** (`hb_demux[title->demuxer]`), which splits it into elementary-stream buffers tagged with `buf->s.id`.
4. For each buffer: `GetFifoForId()` finds the destination (the video decoder FIFO, an audio decoder FIFO or a subtitle FIFO). Streams that weren't selected are dropped right here.
5. **SCR/PCR discontinuity handling** (TS/PS sources): when the system clock jumps, the reader adds a rough
   offset (`last_pts + 90000 - start`) so timestamps keep increasing, tags `scr_sequence`, and sync makes
   the fine correction later (see [05-sync-and-timing](05-sync-and-timing.md)). `splice_discontinuity()` repairs frames split across a discontinuity.
6. Before the requested start is reached (`start_found == 0`), packets earlier than `pts_to_start` are thrown away.

## libavformat packet read (`hb_ffmpeg_read`, `stream.c:~6416`)

- `av_read_frame()`, retried on `EAGAIN`. Any error other than EOF raises `HB_ERROR_READ`.
- **Keyframe gate after a seek**: it skips video packets until a keyframe, for up to 50 frames.
- Zero-size packets are kept as empty buffers (AVI "packed B-frames" null frames, Theora duplicates).
- Packets of 128 MB or more are dropped as bogus.
- Subtitle packets get an extra NUL terminator ("Some ffmpeg subtitle decoders expect a null terminated string").
- Palette side data is carried along (needed by palettized codecs).
- `AV_PKT_FLAG_DISCARD` becomes `HB_FLAG_DISCARD`.
- **Timestamp conversion to 90 kHz**: `tsconv = 90000 * time_base.num / time_base.den`, minus the stream's
  initial timestamp, so **every stream starts near zero on HandBrake's 90 kHz clock**.

> **90 kHz everywhere.** All of libhb (sync, filters, encoders, muxer) works in 90 kHz ticks. The MP4/MOV
> muxer also uses a `1/90000` time base. See [11-muxing-and-metadata](11-muxing-and-metadata.md).

## Seeking modes (`hb_reader_open`, `reader.c:92-240`)

| Request | How |
|---|---|
| `start_at_preview = k` (live preview encode) | Seek to `(k-1) / (seek_points+1)` of the duration (11 points by default) with `hb_stream_seek_ts`. If that fails (TS/PS have no index), it falls back to a **byte-fraction seek** |
| `pts_to_start` (start time) | `hb_stream_seek_ts` lands on the I-frame **before** the target. Sync then drops frames until the exact start |
| `chapter_start` | `hb_stream_seek_chapter` |
| `pts_to_stop` / `frame_to_stop` | The reader stops once duration is passed. Sync cuts output at the exact frame count (`sync.c:1596-1626`) |

The **live preview** feature encodes a short clip from one of the scan's evenly spaced preview points. It is the
same idea as our app's sample encodes for size estimation (see [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)).

## Own MPEG PS/TS demuxer

Disc and broadcast sources (`demuxmpeg.c`, the TS paths in `stream.c`) go through HandBrake's own demuxer,
because those streams lack an index and have PCR resets. libavformat is used for everything else (MP4, MKV, MOV, AVI, WebM …).

## Related
- Previous: [02-scan-and-probe](02-scan-and-probe.md)
- Next: [04-decoding](04-decoding.md)
- Timestamps repaired downstream: [05-sync-and-timing](05-sync-and-timing.md)
