---
title: HandBrake hardware acceleration
tags: [handbrake, hardware, gpu, hwaccel, vaapi, nvenc, nvdec, qsv, vce, amf, videotoolbox, linux, flatpak]
source: libhb/hwaccel.c, libhb/common.c, libhb/work.c, libhb/vaapi_common.c, libhb/encavcodec.c, libhb/nvenc_common.c, make/configure.py
related: [00-index, 04-decoding, 06-filters, 07-video-encoders, 12-hdr-and-color, 15-lessons-for-video-compressor]
---

# Hardware acceleration

## Two separate things
1. **Hardware decoding**: an `hb_hwaccel_t` registered in `hwaccel.c`.
2. **Hardware encoding**: libavcodec hw encoders through `encavcodec.c` (see [07-video-encoders](07-video-encoders.md)).

They are independent. A full GPU pipeline needs both, plus GPU-capable filters.

## hwaccel registry (`hb_common_global_hw_init`, `common.c:~621`)
| hwaccel | `HB_DECODE_*` | Device | Platform / build flag |
|---|---|---|---|
| VideoToolbox | `VIDEOTOOLBOX` | VT | macOS |
| **NVDEC** | `NVDEC` | CUDA (`AV_PIX_FMT_CUDA`) | `--enable-nvdec`, **off by default** |
| AMF decode | `AMFDEC` | AMF surfaces | Windows (experimental in 1.12.0) |
| Media Foundation | `MF` | D3D11VA | Windows (ARM) |
| **QSV** | `QSV` | Intel oneVPL | Windows by default. Linux needs `--enable-qsv` (the Flatpak has an Intel Media SDK plugin) |
| **VAAPI** | none | none | **No VAAPI hw decoder exists in HandBrake** |

Each hwaccel declares:
- `encoders[]`: the encoders it can feed **without leaving the GPU**
- `can_filter(list)`: whether every enabled filter has a GPU version (the default `can_filter` returns 0)
- `caps`: `SCAN` (usable for scan previews), `ROTATE`, `COLOR_RANGE`, `FORMAT_REQUIRED`, `SYSTEM_MEMORY`
- `find_decoder`, and `upload` (CPU → GPU)

## Full GPU pipeline decision (`hb_hwaccel_can_use_full_hw_pipeline`, `hwaccel.c:178`)
Frames stay on the GPU **only if all of these hold**:
1. a hwaccel was chosen and the scan says the title supports it
2. `can_filter()`: every active filter has a GPU version (for example NVDEC has `scale_cuda`/`colorspace_cuda`, see [06-filters](06-filters.md))
3. rotation is 0, or the hwaccel has `CAP_ROTATE`
4. no range conversion, or it has `CAP_COLOR_RANGE`
5. the chosen encoder is in its `encoders[]` list (for example NVDEC → NVENC)

Otherwise frames are **downloaded to system memory after decode** (`av_hwframe_transfer_data`) and the
rest runs on the CPU. With `HB_DECODE_FORCE_HW`, hw decode is still used, just with a download.

If initialising the device fails, it logs "Failure to initialise hwaccel" and **falls back to software**. The encode still runs.

## Pixel formats with hw
Biplanar NV12/P010 pipeline formats are allowed only when a hw decoder is active. See [12-hdr-and-color](12-hdr-and-color.md).

## VAAPI specifics (`vaapi_common.c`, `encavcodec.c`)
- **Build**: `--enable-vaapi` exists but is **`default=False`** in `make/configure.py`. The Flatpak manifest (`fr.handbrake.ghb.json`) only passes `--flatpak`. So **stock HandBrake on Linux usually has no VAAPI encoder**, and the hardware preset list has no VAAPI entries.
- **Device**: one global context, `av_hwdevice_ctx_create(VAAPI, NULL, …)`, which is the default DRM render node. `hb_vaapi_available()` checks it, and `hb_vaapi_encoder_available()` also **test-opens the encoder**.
- **Frames**: `hw_frames_ctx` with `sw_format = NV12` and `initial_pool_size = 20`. Each frame is uploaded through `av_hwframe_transfer_data`. So it is **8-bit only**, with CPU decode and CPU filters.
- **Rate control**: CQ means `rc_mode=CQP` + `qp=<RF>` (H.264/HEVC) or `global_quality` (AV1/VP8/VP9). Bitrate means `rc_mode=VBR`. ICQ is avoided because some drivers (AMD RDNA3) don't support it.
- **Quality range**: 1–51 for H.264/HEVC, 1–63 for AV1.

## NVENC specifics
- CQ: `rc=vbr` + `cq=RF`, with `init_qpI/P/B = RF−2 / RF / RF+2`.
- With NVDEC enabled and only CUDA-capable filters, the **whole pipeline stays on the GPU**.

## Why HandBrake calls GPU encoders "speed, not efficiency"
The hardware presets use **VFR, no crop, no deinterlace, no multi-pass**, with RF values tuned per vendor
(NVENC HEVC 27 at 1080p, QSV 25, VCN 25). GPU encoders are much faster but need more bits than x265 `slow`
for the same quality. Our own measurement agrees: VAAPI at QP 28 ≈ x265 `medium` quality at about 1.1× the
size (see [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)).

## Related
- Decoder internals: [04-decoding](04-decoding.md)
- Encoder rate control: [07-video-encoders](07-video-encoders.md)
