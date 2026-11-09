---
title: HandBrake muxing and metadata
tags: [handbrake, muxing, mp4, mkv, webm, libavformat, hvc1, hev1, faststart, metadata, chapters, location]
source: libhb/muxcommon.c, libhb/muxavformat.c
related: [00-index, 01-architecture-pipeline, 07-video-encoders, 09-audio, 10-subtitles, 12-hdr-and-color, 15-lessons-for-video-compressor]
---

# Muxing and metadata

## Two layers
1. **`muxcommon.c`**: the `WORK_MUX` work object. It gathers encoded buffers from every track and **interleaves** them.
2. **`muxavformat.c`**: writes the actual file with **libavformat** (`mp4`, `ipod`, `mov`, `matroska`, `webm`).

## Interleaving (`muxcommon.c`)
- Each track has an internal FIFO. Data is written in chunks of `interleave` ticks (90 kHz) **once every track has at least that much buffered**, so audio and video sit close together in the file and play smoothly over slow I/O.
- `MAX_BUFFERING = 50 MB` per track. Past that, the track is flushed anyway, so a sparse subtitle track can't make audio and video pile up in memory.
- Sparse subtitle tracks are handled specially so they don't hold the other tracks back.

## Container settings (`muxavformat.c:~290-340`)

| Container | Options |
|---|---|
| **MP4** | `brand=mp42`, `strict=experimental`, time base **1/90000**, `movflags=+disable_chpl+write_colr`, plus **`faststart`** when `Optimize` ("Web optimized") is on. `ipod` muxer if `Mp4iPodCompatible` |
| MOV | as MP4, plus `negative_cts_offsets` |
| **MKV** | `matroska` muxer, `default_mode=passthrough` (keeps the source's default-track flags) |
| WebM | `webm` muxer, `default_mode=passthrough` |

- `write_colr` writes the **`colr` atom** (primaries, transfer, matrix, range) into MP4 so players and browsers get colour right. See [12-hdr-and-color](12-hdr-and-color.md).
- `disable_chpl` turns off the Nero chapter atom (QuickTime-style chapters are used).

## Codec tags (fourcc)
| Codec | Tag | Rule |
|---|---|---|
| H.264 | `avc1` / `avc3` | `avc3` when `InlineParameterSets` (SPS/PPS repeated in-band) |
| **HEVC** | **`hvc1`** / `hev1` | **`hvc1` by default** (QuickTime/iOS need it). `hev1` only with inline parameter sets in the MP4 family |
| HEVC + Dolby Vision | `dvh1` | when DOVI passthru is on |
| ProRes | `apco/apcs/apcn/apch/ap4h/ap4x` | by profile |
| Text subs in MP4 | `tx3g` | |

The encoders write Annex-B headers as extradata, and the muxer converts them to `hvcC`/`avcC`. See [07-video-encoders](07-video-encoders.md).

## Stream-level data
- Video: `chroma_location`, colour fields, SAR, and DOVI configuration record. Stereo-3D and spherical mapping metadata passthru is new in 1.12.0.
- Audio and subtitle tracks: `language` (ISO 639-2), `title`, `handler_name` (MP4 track names, with `AudioTrackNamePassthru` / `SubtitleTrackNamePassthru`).
- MKV attachments: fonts (`application/vnd.ms-opentype` mimetype) for SSA rendering.
- **Chapters** (`ChapterMarkers=true`): written as container chapters. Each chapter start is an **IDR** frame. See [07-video-encoders](07-video-encoders.md).

## File metadata mapping (`metadata_keys`, `muxavformat.c:67`)
HandBrake reads source metadata into its own keys at scan time (see [02-scan-and-probe](02-scan-and-probe.md)) and writes them back per container. Examples:

| HB key | MP4/MOV key | Notes |
|---|---|---|
| `CreationTime` | `creation_time` | MXF `modification_date` is mapped here |
| `ReleaseDate` | `date` | Android `creation_time` is mapped here |
| `Location` | `location` → **`com.apple.quicktime.location.ISO6709`** | also written as `location-und` (undefined language) for QuickTime |
| Name, Artist, Comment, Genre, Description, … | standard keys | |
| Cover art | `list_coverart` → attached picture | |

libavformat's automatic "Encoded by" tag is managed so that HandBrake's own value is written.

## Related
- Our muxer flags compared (we already use `hvc1` + `faststart` + `use_metadata_tags`): [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)
