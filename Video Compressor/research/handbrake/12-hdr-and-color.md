---
title: HandBrake HDR and colour handling
tags: [handbrake, hdr, hdr10, hdr10plus, dolby-vision, hlg, pq, color, colorspace, tonemap, bit-depth, pixel-format]
source: libhb/stream.c, libhb/decavcodec.c, libhb/common.c, libhb/encx265.c, libhb/colorspace.c, libhb/work.c, libhb/rpu.c, libhb/hdr10plus.c, libhb/dovi_common.c
related: [00-index, 02-scan-and-probe, 04-decoding, 06-filters, 07-video-encoders, 11-muxing-and-metadata, 15-lessons-for-video-compressor]
---

# HDR and colour handling

## Where colour info comes from
1. **Scan**: the video stream's `codecpar` primaries/trc/space/range are read **before** `avformat_find_stream_info` can overwrite them (`stream.c` `ffmpeg_open`). See [02-scan-and-probe](02-scan-and-probe.md).
2. **Side data**: mastering display (`job->mastering`), content light level (`job->coll`), ambient viewing environment, DOVI configuration (`title->dovi`).
3. **Decoder**: every decoded frame is **stamped with the title's colour tags** so the pipeline agrees (see [04-decoding](04-decoding.md)). An unspecified range becomes **limited**.

## Defaults when tags are missing (`common.c`, `hb_get_color_*`)
- **Matrix**: guessed from resolution. **≥1280 wide or ≥720 tall (or >720×576) → BT.709**, otherwise **BT.601 (SMPTE 170M)**. Primaries and transfer follow the same logic.
- **Range**: unspecified → limited (MPEG).

## Output colour
`hb_output_color_prim/transfer/matrix(job)` give the output tags. They follow the source unless the
**Colorspace** filter converts, or the user overrides them in the encoder options (x265 values are read back
into `job->color_*_override`, see [07-video-encoders](07-video-encoders.md)). The tags are written:
- in the **bitstream VUI** (x265 `colorprim/transfer/colormatrix/range`)
- in the **container**: MP4 `colr` atom (`write_colr`), MKV colour elements. See [11-muxing-and-metadata](11-muxing-and-metadata.md).

## Bit depth and pixel format
- `hb_get_best_pix_fmt()` (`common.c:7723`) walks a list from richest to poorest (`P416/YUV444P16 … P010/YUV420P10 … NV12/YUV420P`) and takes the first format that:
  - **does not exceed the source bit depth**, so 8-bit sources are never processed at 10-bit inside the pipeline
  - **does not have more chroma resolution than the source**
  - is biplanar (NV12/P010) **only if a hw decoder is active**
  - is planar if any native CPU filter (decomb, denoise, nlmeans, sharpen, …) is in the chain.
- The **encoder** can still have a higher bit depth than the pipeline: the presets use **`x265_10bit` even for 8-bit sources**, so the final Format filter converts 8 → 10 bit. Encoding 8-bit content in 10-bit HEVC gives better compression efficiency and less banding, and that is why HandBrake's HEVC presets are all 10-bit (see [13-presets](13-presets.md)).
- The VAAPI encoder path in HandBrake uploads **NV12 only**, so it is 8-bit. See [14-hardware-acceleration](14-hardware-acceleration.md).

## HDR10 (PQ) static metadata — x265
When transfer is **SMPTE 2084 (PQ)** and the matrix is **BT.2020-NCL** (`encx265.c`):
- `hdr10-opt=1` (10-bit and up)
- `master-display=G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)`, rescaled to x265's units (chroma /50000, luma /10000)
- `max-cll=MaxCLL,MaxFALL`

The source's mastering-display and CLL values are carried through, so HDR10 displays tone-map correctly.

## HLG
No special encoder flags. Correct colour tags (BT.2020 + ARIB STD-B67) are enough, as with iPhone HLG.

## Dynamic HDR metadata (`VideoPasshtruHDRDynamicMetadata = "all"` in presets)
`sanitize_dynamic_hdr_metadata_passthru()` (`work.c:1545`) decides whether it can be kept. Filters that change geometry can invalidate it.

| Type | How |
|---|---|
| **HDR10+** | per-frame `AV_FRAME_DATA_DYNAMIC_HDR_PLUS` → ITU-T T.35 SEI on each x265 picture (`hdr10plus.c`) |
| **Dolby Vision** | per-frame `AV_FRAME_DATA_DOVI_RPU_BUFFER` → `pic_in.rpu`. The **RPU filter** (`rpu.c`, libdovi) rewrites the RPU after crop/scale. x265 gets `dolby-vision-profile`, VBV/HRD is forced, the level is recomputed (`update_dolby_vision_level`), and MP4 gets the `dvh1` tag |

Some DV profiles (for example profile 10 with compat id 0) are handled specially at scan (`scan.c:1337`).

## Tone mapping (Colorspace filter, `colorspace.c`)
Settings: `primaries`, `transfer`, `matrix`, `range`, **`tonemap`**, `param`, `desat`, `npl`.
- HDR → SDR conversion is a libavfilter chain:
  `zscale=transfer=linear:npl=…` → `format=gbrpf32le` → **`tonemap=hable`** (the default; `param`, `desat` and
  `peak` can be set) → `zscale=primaries/transfer/matrix/range` → `format=<pipeline pix_fmt>`.
- **Signal peak** (`determine_signal_peak`): `MaxCLL / 100`, else mastering max luminance / 100, else a fallback of
  **100 for PQ and 10 otherwise**. Reference white is 100 nits.
- It fails if the input colour space is undefined ("colorspace: input color space undefined").
- It is **off by default in every preset** (`PictureColorspacePreset = "off"`), so HDR stays HDR.

## Related
- Our HDR handling compared (we pass HDR10 flags but not mastering/CLL): [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)
