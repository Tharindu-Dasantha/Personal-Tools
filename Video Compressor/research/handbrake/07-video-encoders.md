---
title: HandBrake video encoders
tags: [handbrake, encoding, x265, hevc, x264, svt-av1, encavcodec, vaapi, nvenc, qsv, vce, keyframes, gop]
source: libhb/work.c, libhb/encx265.c, libhb/encx264.c, libhb/encsvtav1.c, libhb/encavcodec.c, libhb/vaapi_common.c
related: [00-index, 08-rate-control-and-quality, 12-hdr-and-color, 13-presets, 14-hardware-acceleration, 11-muxing-and-metadata]
---

# Video encoders

## Dispatch (`hb_video_encoder`, `work.c:239`)

| Encoder family | Work object | Library |
|---|---|---|
| x264 8/10-bit | `WORK_ENCX264` (`encx264.c`) | libx264, linked directly |
| **x265 8/10/12/16-bit** | `WORK_ENCX265` (`encx265.c`) | libx265, linked directly, one build per bit depth |
| SVT-AV1 8/10-bit | `WORK_ENCSVTAV1` (`encsvtav1.c`) | SVT-AV1, linked directly |
| Theora | `WORK_ENCTHEORA` | libtheora |
| VideoToolbox (macOS) | `WORK_ENCVT` | Apple VT |
| MPEG-2/4, VP8/VP9, FFV1, ProRes, DNxHR | `WORK_ENCAVCODEC` | libavcodec |
| **All other hardware**: QSV, NVENC, VCE/AMF, **VAAPI**, Media Foundation | `WORK_ENCAVCODEC` | libavcodec's hw encoders (`hevc_vaapi`, `hevc_nvenc`, `hevc_qsv`, …) |

So the CPU encoders that matter are called through their own APIs, for full control. Hardware encoders all
go through libavcodec, which is the same path as the `ffmpeg` CLI.

---

## x265 in detail (`encx265.c`)

### Init sequence (`encx265Init`)
1. `x265_api_query(depth, X265_BUILD)` picks the **8, 10 or 12-bit libx265** build at runtime.
2. `param_default_preset(preset, tune)` applies the x265 preset (`slow`, `medium`, …) and tune.
3. If tune contains `psnr` or `ssim`, the matching metric is turned on.
4. **HandBrake defaults** (the user can override them):
   - `fpsNum/fpsDenom` = `orig_vrate`.
   - **`keyframeMin` (min-keyint) = fps rounded**. **`keyframeMax` (keyint) = 10 × fps**, so 300 frames at 30 fps.
5. **Colour VUI**: `colorprim`, `transfer` and `colormatrix` from `hb_output_color_*(job)`. `range` = limited or full.
6. **HDR10** (if transfer is PQ **and** matrix is BT.2020-NCL):
   - `hdr10-opt=1` when bit depth > 8.
   - `master-display=G(..)B(..)R(..)WP(..)L(max,min)` from the source's mastering-display side data.
   - `max-cll=MaxCLL,MaxFALL` from the content-light-level side data.
7. Ambient viewing environment SEI and `chromaloc` are passed through.
8. **User options** (`encoder_options`, the preset's `VideoOptionExtra`) are parsed with `x265_param_parse`. Colour, mastering and chroma-location values are read back afterwards in case the user overrode them.
9. `bRepeatHeaders = 1` when `inline_parameter_sets` is set (needed for the `hev1` tag and streaming).
10. **Fixed, not overridable**: source width/height, internal colourspace (I420/I422/I444 from the output pix_fmt), `sar`.
11. **Rate control**:
    - Quality mode: `X265_RC_CRF`, `rfConstant = job->vquality`.
    - Bitrate mode: `X265_RC_ABR`, `bitrate = vbitrate`. For multi-pass it adds `stats=<tmp>/x265.log`, `pass=1|2`, and on the analysis pass `slow-firstpass = !fastanalysispass` (that is the "turbo first pass").
12. Profile (`main`, `main10`, …) and level are applied **last**.
13. `job->areBframes` is recorded for the muxer (B-pyramid → `negative_cts` and so on).
14. **Dolby Vision**: `dolby-vision-profile=<profile><compat>`, plus VBV max rate and buffer set to the level's max if not given (DV needs HRD). The DV level is computed from the final size and fps.
15. `x265_cleanup()` before `encoder_open()`, because "x265 has got some global variables that prevents multiple encode with different settings in the same process".
16. `encoder_headers()` writes VPS/SPS/PPS as **extradata (Annex-B)**, and the muxer converts it.

### Per-frame (`x265_encode`)
- Feeds plane pointers and strides **without copying** (`pic_in.planes[] = buf->plane[].data`).
- `pts = s.start` on the 90 kHz clock, and `poc = frames_in++`.
- **HDR10+ dynamic metadata**: `AV_FRAME_DATA_DYNAMIC_HDR_PLUS` → ITU-T T.35 SEI (`hb_dynamic_hdr10_plus_to_itu_t_t35`) → `userSEI`.
- **DOVI RPU**: `AV_FRAME_DATA_DOVI_RPU_BUFFER` → `pic_in.rpu`.
- **Chapter starts force an IDR** (`sliceType = X265_TYPE_IDR`). The chapter goes into a queue and is attached when that frame leaves the encoder (up to about 16 frames later).
- Logs an "input continuity err" if `start != last_stop`.
- Frame type → `HB_FRAME_IDR/I/P/B/BREF` for the muxer. At EOF the encoder is **flushed** of delayed frames.

### Quality scale (`common.c:1675`)
| Encoder | RF range | Step |
|---|---|---|
| x265 8-bit, x264 8-bit | 0 – 51 | 0.1 |
| x265 / x264 10-bit | −12 – 51 | 0.1 |
| x265 12-bit | −24 – 51 | 0.1 |
| SVT-AV1 | 0 – 70 | 0.25 |
| NVENC / VAAPI H.264/H.265 | 1 – 51 | 0.1 |
| VAAPI / NVENC AV1 | 1 – 63 | 0.1 |
| VideoToolbox | 0 – 100, higher = better | 1 |

---

## Hardware encoders through `encavcodec.c`

Common parts:
- `gop_size = 10 × fps` (the same idea as x265's keyint). **VCE/AMF uses `min(2 × fps, 120)`** "since we do not have scene change detection … to help avoid stale references".
- Frame rate is snapped to the nearest supported rate if the codec has a fixed list (MPEG-2).
- Preset, tune, level and options are applied, then the rate control that users may not override.

| Encoder | Constant-quality mode | Bitrate mode |
|---|---|---|
| **VAAPI** (`h264/hevc/av1/vp8/vp9_vaapi`) | **`rc_mode=CQP`**, with **`qp=<RF>`** for H.264/HEVC and `global_quality=<RF>` for VP8/VP9/AV1. The comment says ICQ isn't supported by some drivers (AMD RDNA3 reports "supported modes: CQP, CBR, VBR, QVBR"), so CQP is the portable choice | `rc_mode=VBR` + `bit_rate` |
| **NVENC** | `rc=vbr`, `cq=<RF>`, plus `init_qpI = RF−2`, `init_qpP = RF`, `init_qpB = RF+2` | `rc=vbr` |
| **QSV** | `global_quality=<RF>` with ICQ BRC | look_ahead, maxrate |
| **VCE/AMF** | CQP with **P/B offsets that grow with RF** (`qualityOffsetP` up to 2 or 8 depending on the codec, B = 2×P) | `rc=vbr_peak` |
| **libvpx VP9** | `crf` (`global_quality` for VP9 RF 0 = lossless) | |

### VAAPI frame path (`vaapi_common.c`)
- One global VAAPI device: `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_VAAPI, NULL, …)`, which uses the default render node.
- The encoder gets an `hw_frames_ctx` with **`sw_format = NV12`** and `initial_pool_size = 20` (`encavcodec.c:967`).
- Each CPU frame: `av_hwframe_get_buffer` → `av_hwframe_transfer_data` (upload) → `avcodec_send_frame`.
- So **HandBrake's VAAPI path is CPU decode → CPU filters → upload as NV12 (8-bit) → GPU encode**. There is no VAAPI decode and no VAAPI filters. See [14-hardware-acceleration](14-hardware-acceleration.md).

---

## x264 (`encx264.c`) and SVT-AV1 (`encsvtav1.c`), briefly
- x264: the same pattern (preset/tune → HB defaults → user opts → fixed values → profile/level last). It has a large table for `VideoOptionExtra`. Presets use `ref=5:bframes=5` for Super HQ.
- SVT-AV1: 10-bit presets with `enable-variance-boost=1:enable-qm=1:ac-bias=1:tf-strength=1:qp-scale-compress-strength=1:sharpness=1:keyint=10s`. HDR10 and DV are supported as well.

## Related
- How RF and multi-pass are picked: [08-rate-control-and-quality](08-rate-control-and-quality.md)
- The preset values that feed these encoders: [13-presets](13-presets.md)
- HDR metadata sources: [12-hdr-and-color](12-hdr-and-color.md)
- Extradata, hvc1/hev1 and mux: [11-muxing-and-metadata](11-muxing-and-metadata.md)
