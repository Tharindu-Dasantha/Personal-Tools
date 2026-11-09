---
title: HandBrake scan and probe
tags: [handbrake, scan, probe, autocrop, interlace-detection, framerate-detection, previews, rotation]
source: libhb/scan.c, libhb/stream.c
related: [00-index, 03-demux-and-reader, 04-decoding, 06-filters, 12-hdr-and-color, 15-lessons-for-video-compressor]
---

# Scan and probe

The scan turns a source into **titles** (`hb_title_t`): geometry, PAR/DAR, frame rate, colour, rotation,
audio and subtitle tracks, chapters, metadata, auto-crop values, an interlace flag and preview images.

## Opening the file (`stream.c`)

`hb_stream_open()` (`stream.c:831`):
1. Tries HandBrake's **own MPEG-2 PS/TS demuxer** first (`hb_stream_get_type`). It is used for `.ts/.m2ts/.vob/.mpg`, which have timestamp discontinuities and no index.
2. Otherwise it uses **libavformat** through `ffmpeg_open()` (`stream.c:5253`):
   - `probesize = 15000000` (15 MB, against FFmpeg's 5 MB default), "to successfully scan some files with large PNGs".
   - Reads the video stream's **colour info before** `avformat_find_stream_info()`, because that call can overwrite it.
   - Comment: "FFMpeg has issues with seeking … the safest thing … open 2 AVFormatContext. One for probing info and the other for reading."
3. `ffmpeg_title_scan()` (`stream.c:6034`) builds the title: tracks, chapters, attachments (fonts), cover art and metadata.

### Rotation
Rotation comes from the **display matrix side data** (`AV_PKT_DATA_DISPLAYMATRIX` → `av_display_rotation_get`)
and is stored as `title->rotation` (0/90/180/270). The **Adapter** filter later physically rotates the frames
(see [06-filters](06-filters.md)), so the output carries no rotation flag that players could ignore.

### Metadata
`ffmpeg_decmetadata()` maps container tags to HandBrake keys through the `metadata_keys` table
(`muxavformat.c:67`). Two special cases:
- **Android**: if `com.android.version` or `firmware` is present and there is no release date, `creation_time` becomes `ReleaseDate`.
- **MXF**: `modification_date` becomes `CreationTime`.

On output these keys are written back, including **Location** as `com.apple.quicktime.location.ISO6709`. See [11-muxing-and-metadata](11-muxing-and-metadata.md).

## Preview decoding: `DecodePreviews()` (`scan.c:760`)

- Decodes **N preview frames** spread evenly through the title. The default is 10, the minimum is 3
  (`scan.c:111-116`). Seek points are at `i / (N+1)` of the duration.
- After each seek it **spins until a keyframe** (up to 50 video frames) so it never decodes garbage (`stream.c` `need_keyframe`).
- For each preview it:
  1. Gets `vid_info` (geometry, PAR, rate, colour) from the decoder's `info()` call. It **drops previews whose geometry doesn't match the buffer**.
  2. `remember_info()` tallies the info, and the **most common** info wins. `has_resolution_change()` flags sources whose size changes mid-stream (decoder threading is then turned off, see [04-decoding](04-decoding.md)).
  3. Converts the frame to YUV420 through a small filter (rotation applied).
  4. Runs **comb detection**: `hb_detect_comb(buf, 10, 30, 9, 10, 30, 9)`.
  5. Optionally saves a JPEG preview (`hb_save_preview`).
  6. Runs **black-border detection** (below).

### Auto-crop algorithm (`scan.c:449-570`, `1384-1460`)

- `row_all_dark()` / `column_all_dark()`: a row is "dark" if its **average luma < 32** (with values clamped up
  to 16) **and every pixel is within ±16 of that average**, so it must be a smooth border, not a dark scene.
- The search starts at `border = height/100`, which skips thin VBI or diagnostic lines on the matte edge, up to **¼ of the frame** on each side.
- A preview's crop is **recorded only if every side is < ¼ of the frame**. Black title cards, credits and fades would otherwise fool it.
- Final choice over at least 3 recorded previews:
  - **Median** crop by default ("smart").
  - If many previews (`crop_threshold_frames`: 4 by default, 6 at ≥30 previews, 8 at >40) crop noticeably less
    than the median (more than `crop_threshold_pixels`, 9 px by default), it switches to **loose crop** (the
    minimum), on the assumption that the aspect ratio is mixed.
  - Values are rounded to EVEN.
- Both `title->crop` (smart) and `title->loose_crop` are stored. The preset's `PictureCropMode` picks one
  (0 = automatic, 2 = none in the hardware/pro presets).

### Interlace detection
If **≥ half of the previews** are combed (`scan.c:1552`), the title is flagged as interlaced. In the presets,
deinterlacing is **selective** anyway (comb detect + decomb, applied per frame). See [06-filters](06-filters.md).

### Frame-rate detection (`scan.c:1225-1262`)
- Internal rate is `27 MHz / den` (for example `den = 1126125` → 23.976).
- Rates within ±100 ticks of a standard rate are **snapped** to it.
- NTSC heuristics: lots of **pulldown** or mostly progressive frames at 29.97 → 23.976; mostly
  **repeat-flagged doubled frames** → the rate is halved.
- `title->video_bitrate` is recorded as well.

## Hardware decode capability at scan
The scan records which hw decoders can handle the title (`title->video_decode_support`). At encode time
`do_job` turns hw decode off unless it was marked as supported. See [14-hardware-acceleration](14-hardware-acceleration.md).

## Related
- Next stage: [03-demux-and-reader](03-demux-and-reader.md)
- Colour fields gathered here: [12-hdr-and-color](12-hdr-and-color.md)
- Our app's equivalent (`ffprobe`): [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)
