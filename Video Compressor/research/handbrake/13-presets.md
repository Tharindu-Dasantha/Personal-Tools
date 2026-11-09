---
title: HandBrake built-in presets
tags: [handbrake, presets, x265-tuning, rf-values, defaults, json]
source: preset/preset_builtin.json, preset/preset_template.json, libhb/preset.c
related: [00-index, 05-sync-and-timing, 07-video-encoders, 08-rate-control-and-quality, 09-audio, 15-lessons-for-video-compressor]
---

# Built-in presets

Presets are JSON (`preset/preset_builtin.json`), with the field defaults in `preset_template.json`.
`preset.c` turns a preset plus a title into a job. These values show what HandBrake's developers think are
good defaults.

## Defaults shared by nearly all General/Device/Matroska presets
| Key | Value | Meaning |
|---|---|---|
| `VideoQualityType` | 2 | Constant quality (RF). See [08-rate-control-and-quality](08-rate-control-and-quality.md) |
| `VideoFramerateMode` / `VideoFramerate` | `pfr` / 30 (or 60, 25) | **Peak** frame rate: caps at 30, leaves lower rates alone. See [05-sync-and-timing](05-sync-and-timing.md) |
| `PictureUseMaximumSize` / `PictureAllowUpscaling` | true / false | Downscale to fit `PictureWidth×PictureHeight`, never upscale |
| `PictureKeepRatio`, `PicturePAR=auto`, `PictureModulus=2` | | keep the aspect ratio, even dimensions |
| `PictureCropMode` | 0 (auto/smart) | hardware and pro presets use 2 (none). See [02-scan-and-probe](02-scan-and-probe.md) |
| `PictureDeinterlaceFilter` + `PictureCombDetectPreset` | `decomb` + `default` | selective deinterlace, only on combed frames. See [06-filters](06-filters.md) |
| `PictureDenoiseFilter`, sharpen, deblock, chroma smooth, colorspace | `off` | no processing by default |
| Audio | `aac` 160 kbps stereo (+ `copy:ac3` for Surround) | See [09-audio](09-audio.md) |
| `VideoPasshtruHDRDynamicMetadata` | `all` | keep HDR10+ and DV when possible. See [12-hdr-and-color](12-hdr-and-color.md) |
| `ChapterMarkers` | true | |
| `Optimize` (faststart) | false, except **Web/Creator/Social and Chromecast** presets | See [11-muxing-and-metadata](11-muxing-and-metadata.md) |

## HEVC presets in detail
| Preset | Encoder | Speed | RF | Size cap | FPS |
|---|---|---|---|---|---|
| Very Fast 2160p60 4K HEVC | x265_10bit | superfast | 26 | 3840×2160 | PFR 60 |
| Fast 2160p60 4K HEVC | x265_10bit | faster | 24 | 3840×2160 | PFR 60 |
| HQ 2160p60 4K HEVC Surround | x265_10bit | medium | 22 | 3840×2160 | PFR 60 |
| Super HQ 2160p60 4K HEVC Surround | x265_10bit | slow | 20 | 3840×2160 | PFR 60 |
| **H.265 MKV 2160p60 4K** | x265_10bit | slow | 24 | 3840×2160 | PFR 60 |
| **H.265 MKV 1080p30** | x265_10bit | slow | **22** | 1920×1080 | PFR 30 |
| **H.265 MKV 720p30** | x265_10bit | slow | **21** | 1280×720 | PFR 30 |
| H.265 MKV 576p25 / 480p30 | x265_10bit | slow | 20 | 720×576 / 720×480 | PFR 25/30 |
| Apple / Chromecast / Roku / Amazon 4K HEVC | x265_10bit | slow | 24 | 3840×2160 | PFR 60 |
| H.265 NVENC 1080p / 4K | nvenc_h265 | medium | 27 / 30 | | VFR |
| H.265 QSV 1080p / 4K | qsv_h265 | speed | 25 / 28 | | VFR |
| H.265 VCN 1080p / 4K | vce_h265 | balanced | 25 / 28 | | VFR |

**There is no VAAPI preset.** VAAPI is not built by default on Linux. See [14-hardware-acceleration](14-hardware-acceleration.md).

All HEVC CPU presets use **10-bit x265 (`main10`) even for 8-bit sources**.

## The custom x265 tuning string
Every x265 preset sets `VideoOptionExtra`:
```
strong-intra-smoothing=0:rect=0:aq-mode=1:rd=4:psy-rd=0.75:psy-rdoq=4.0:rdoq-level=1:rskip=2
```
| Option | x265 `slow` default | HB value | Effect |
|---|---|---|---|
| `strong-intra-smoothing` | 1 | **0** | keeps fine detail and grain in large intra blocks (less blur) |
| `rect` | 1 (on at slow) | **0** | turns off rectangular partitions, which is faster. HandBrake trades this speed for the psy settings below |
| `aq-mode` | 2 (auto-variance) | **1** | plain variance AQ |
| `rd` | 4 at slow | **4** | pinned, so it doesn't change with the preset |
| `psy-rd` | 2.0 | **0.75** | weaker psycho-visual RD: less "fake sharpness", fewer ringing artefacts, more efficient at mid RF |
| `psy-rdoq` | 1.0 at slow | **4.0** | stronger psy RDOQ: keeps texture and grain energy in quantisation |
| `rdoq-level` | 2 at slow | **1** | |
| `rskip` | 1 | **2** | edge-density-based recursion skip (rskip mode 2), faster with little quality loss |

(The x265 defaults are from x265's preset table as we know it and may vary by x265 version. The HB values are exact from `preset_builtin.json`.)

The design is a **texture-preserving tune**: weaker psy-rd plus stronger psy-rdoq, no strong intra
smoothing, and simple AQ, with some speed won back (`rect=0`, `rskip=2`).

## Other notable tuning strings
| Presets | String |
|---|---|
| SVT-AV1 (all) | `enable-variance-boost=1:enable-qm=1:ac-bias=1:tf-strength=1:qp-scale-compress-strength=1:sharpness=1:keyint=10s` |
| x264 Super HQ | `ref=5:bframes=5` |
| x264 Creator (editing-friendly) | `keyint=30:min-keyint=15:ref=1:bframes=2:b-pyramid=none` |
| x264 Social (fixed size, 2-pass ABR) | `qcomp=0.5:aq-mode=2:aq-strength=0.8:bframes=2:b-adapt=2:b-pyramid=0` |
| x264 Production | `keyint=12 … bframes=0 … deblock=-2,-2` (all-intra-ish for editing) |

## x264 reference ladder (for comparison)
| Tier | Speed | 480p | 576p | 720p | 1080p |
|---|---|---|---|---|---|
| Very Fast | veryfast | 22 | 22 | 23 | 24 |
| Fast | fast | 20 | 20 | 21 | 22 |
| HQ | slow | 18 | 18 | 19 | 20 |
| Super HQ | veryslow | 16 | 16 | 17 | 18 |

## Related
- What these keys do inside the encoder: [07-video-encoders](07-video-encoders.md)
- How our class table compares: [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)
