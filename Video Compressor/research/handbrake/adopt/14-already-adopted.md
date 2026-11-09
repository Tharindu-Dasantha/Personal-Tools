---
title: HandBrake techniques the app already uses
tags: [adopt, already-adopted, video-compressor, comparison]
source: ../15-lessons-for-video-compressor.md, Video Compressor SPEC.md and src/compress.c as of 2026-11-09
related: [00-index, 15-rejected, ../15-lessons-for-video-compressor]
---

# Already adopted

HandBrake techniques Video Compressor already has, so they aren't proposed again. Details in [../15-lessons-for-video-compressor](../15-lessons-for-video-compressor.md).

| HandBrake technique | HandBrake source | Our equivalent |
|---|---|---|
| Quality ladder by resolution (lower RF for small video) | presets, docs ([../08](../08-rate-control-and-quality.md)) | class table by short side, CRF 24–28 |
| Constant quality, not bitrate | `VideoQualityType=2` | `-crf` / `-qp`, no multi-pass |
| Skip the scaler when the size doesn't change | `sanitize_filter_list_pre` ([../06](../06-filters.md)) | `scale` only when `side < short side` |
| Downscale only, never upscale | `PictureAllowUpscaling=false` | Max size never upscales (D6) |
| Peak frame-rate cap | PFR 30 ([../05](../05-sync-and-timing.md)) | `-fpsmax 30` only above 30.5 fps (D5) |
| Bake rotation into pixels | Adapter filter | ffmpeg autorotate / `transpose_vaapi` |
| Keep the source's bit depth and colour tags | `hb_get_best_pix_fmt`, VUI tags ([../12](../12-hdr-and-color.md)) | `main10` + colour tags for 10-bit |
| PQ signalling | `hdr10-opt` | `hdr10=1:repeat-headers=1` |
| `hvc1` tag in MP4 | `muxavformat.c` ([../11](../11-muxing-and-metadata.md)) | `-tag:v hvc1` |
| Web-optimised MP4 | `faststart` when Optimize | `+faststart` always |
| Keep date and location metadata | `metadata_keys`, ISO6709 | `-map_metadata 0` + `use_metadata_tags` (D4) |
| Copy AAC instead of re-encoding | `AudioCopyMask=copy:aac` | copy when ≤ class bitrate + 10 % |
| Test-open the hw encoder before offering it | `hb_vaapi_encoder_available` ([../14](../14-hardware-acceleration.md)) | 1-frame GPU test at startup |
| VAAPI constant QP | `rc_mode=CQP`, `qp` | `-rc_mode CQP -qp gpu_qp` |
| Low-priority work | `HB_LOW_PRIORITY` threads | nice 10 + idle I/O (R2) |
| Bounded parallelism | bounded FIFOs | up to 4 ffprobes, 1 sample, 1 encode |
| Each encode isolated from the UI | `handbrake.worker.exe` (Windows) | each encode is a `GSubprocess` |
| Evenly spaced sampling | previews at `i/(N+1)` | sample windows at 20/50/80 % |
| Temporary output, then rename | stats files in temp dir | `.part` then rename |
| Scale with lanczos | `zscale`/`swscale lanczos` | **not yet**, see [01](01-lanczos-scaling.md) |

**Where we go beyond HandBrake:** GPU decode + 10-bit on the VAAPI path (HandBrake's is CPU decode, 8-bit,
and not built by default on Linux); measured size estimates in constant-quality mode (libhb has none); an
"only keep it if it's smaller" check; and trashing originals only after verification.
