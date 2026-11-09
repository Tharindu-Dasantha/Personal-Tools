---
title: HandBrake decoding
tags: [handbrake, decoding, libavcodec, threads, error-concealment, hwaccel, decavcodec]
source: libhb/decavcodec.c, libhb/hb.c, libhb/hwaccel.c
related: [00-index, 03-demux-and-reader, 05-sync-and-timing, 06-filters, 12-hdr-and-color, 14-hardware-acceleration]
---

# Decoding

All video decoding (except some disc subtitle formats) goes through **libavcodec**, wrapped by the
`decavcodecv` work object (`decavcodec.c`). Audio uses `decavcodeca` in the same file.

## Video decoder setup (`decavcodecvInit`, `decavcodec.c:1510`)

| Setting | Value | Why |
|---|---|---|
| Decoder | `avcodec_find_decoder(codec_id)`, or `hw_accel->find_decoder()` when a hw device is in use | for AV1 the hw path forces the native `av1` decoder (`hwaccel.c:67`) |
| Threads | `HB_FFMPEG_THREADS_AUTO`, which means **`cpu_count / 2 + 1`** (`hb.c:93`) | leaves CPU for the encoder; Windows-on-ARM uses `cpu+1` |
| Thread type | `FF_THREAD_FRAME \| FF_THREAD_SLICE` | both kinds of parallelism |
| Threading off | when the title has a **resolution change** mid-stream (found during scan) | frame threading doesn't cope well with that |
| `workaround_bugs` | `FF_BUG_AUTODETECT` | works around broken encoders |
| `err_recognition` | `AV_EF_CRCCHECK` | |
| `error_concealment` | `FF_EC_GUESS_MVS \| FF_EC_DEBLOCK` | hides corrupt macroblocks instead of failing |
| `flags=output_corrupt` | only when the title has **no IDR frames** (`HBTF_NO_IDR`) | some broadcast streams never send IDRs |
| Parameters | `avcodec_parameters_to_context()` from the scan's AVFormatContext; `pkt_timebase` = stream time base | |
| Raw ES with no container | `av_parser_init()` splits the stream into frames | |

Opening goes through `hb_avcodec_open()` (`hb.c:80`). It also sets `FF_COMPLIANCE_EXPERIMENTAL` for experimental codecs.

## The decode loop (`decodeFrame`, `decavcodec.c:1360`)

Uses the modern send/receive API:
```
avcodec_send_packet(ctx, pkt)
loop: avcodec_receive_frame(ctx, frame) until EAGAIN/EOF
```

**The PTS trick:** HandBrake doesn't trust the decoder to reorder timestamps. It sends a **sequence number** as
`pkt->pts/dts`, and keeps a hash (`reordered_hash_add`) from sequence to `{real pts, scr_sequence, new_chap}`.
When a frame comes out, its `pts` (the sequence number) is looked up to get the real timestamp and **chapter
mark**. Timestamps and chapter marks therefore follow frames correctly through B-frame reordering.

Decode errors are counted (`decode_errors`), not treated as fatal.

After each decoded frame:
1. **hw frames**: if a hw decoder produced a GPU surface and the pipeline isn't fully on the GPU,
   `av_hwframe_transfer_data()` downloads it into system memory. If the hwaccel quietly fell back to software, the frame is just referenced.
2. `compute_frame_duration()`: the duration is recomputed on every frame ("because sometimes it changes"), using field-repeat flags.
3. **Colour tagging**: every frame is stamped with the **title's** primaries/trc/matrix, so the whole pipeline agrees. `AVCOL_SPC_IPT_C2` is cleared (poorly supported).
4. **Range default**: an unspecified range is set to **limited (MPEG)**.
5. `sanitize_deprecated_pix_fmts()` maps `yuvj*` to `yuv*` with full range ("J pixel formats … breaking some filters").
6. `wrap_frame()` turns the AVFrame into an `hb_buffer_t`. Side data such as HDR10+, DOVI RPU and closed captions is kept.

## Closed captions
If a CC608 subtitle track is selected, CC data in the video bitstream is pulled out during decode and sent to the CC decoder (`cc_send_to_decoder`).

## Audio decoding (`decavcodecaInit`/`Work`)
- `av_parser_parse2()` splits raw audio streams into frames.
- **Downmix at decode** for AC-3, DTS and TrueHD: the decoder is asked to downmix internally (`ac3_downmix_mask`, `dca_downmix_mask`, `truehd_downmix_mask`) when the requested mixdown allows it. That is cheaper and uses the codec's own downmix coefficients.
- `bsinfo()` probes a few packets during scan to learn the real sample rate, layout and bitrate (it parses ADTS extradata for AAC).

See [09-audio](09-audio.md) for what happens next.

## Hardware decoding (summary)
- A registry of `hb_hwaccel_t` entries: NVDEC (CUDA), QSV, VideoToolbox, Media Foundation (D3D11) and AMF. **There is no VAAPI decoder entry.** Details are in [14-hardware-acceleration](14-hardware-acceleration.md).
- `hw_hwaccel_get_hw_format()` picks the hw surface format, or returns `sw_pix_fmt` for system-memory hwaccels.
- With a **full GPU pipeline**, frames stay on the GPU for filters and encoder. Otherwise they are downloaded after decode.

## Related
- Input: [03-demux-and-reader](03-demux-and-reader.md)
- Output goes to sync: [05-sync-and-timing](05-sync-and-timing.md)
- Colour tags: [12-hdr-and-color](12-hdr-and-color.md)
- HW: [14-hardware-acceleration](14-hardware-acceleration.md)
