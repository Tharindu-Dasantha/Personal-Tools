---
title: HandBrake rate control and quality
tags: [handbrake, rate-control, crf, rf, constant-quality, abr, multipass, two-pass, turbo, quality-ranges]
source: libhb/hb.c, libhb/encx265.c, libhb/common.c, handbrake.fr docs
related: [00-index, 07-video-encoders, 13-presets, 15-lessons-for-video-compressor]
---

# Rate control and quality

## Two modes

| Mode | Preset key | Encoder setting | Use |
|---|---|---|---|
| **Constant Quality (RF)** | `VideoQualityType = 2`, `VideoQualitySlider = RF` | x265/x264 CRF, VAAPI CQP `qp`, NVENC `cq`, QSV ICQ | Default for almost every preset. File size follows complexity |
| **Average bitrate** | `VideoQualityType = 1`, `VideoAvgBitrate = kbps` | x265/x264 ABR, multi-pass | When the **size is fixed**, for example the "Social 25 MB / 10 MB" presets |

libhb has **no size predictor for constant quality**: nothing in `libhb/` estimates output size from an RF.
In bitrate mode, size is simply bitrate × duration. The **live preview** encode (a short clip from a preview point, see
[03-demux-and-reader](03-demux-and-reader.md)) is how users judge a setting before they commit.

## Multi-pass (`hb_job_setup_passes`, `hb.c:1947`)
- Constant quality turns multi-pass **off** unless the encoder supports it with CQ (`hb_video_multipass_is_supported(vcodec, 1)`).
- Bitrate mode with `VideoMultiPass=true` runs **N analysis passes + 1 final pass** (N from `hb_video_encoder_get_count_of_analysis_passes`).
- **Turbo first pass** (`VideoTurboMultiPass`): x265 gets `slow-firstpass=0` on the analysis pass. It is much faster and barely changes pass-2 quality.
- Stats files go to the temp dir (`x265.log`, x264's equivalent).
- The interjob struct carries the measured frame rate and frame count from pass 1 to pass 2.

## Official recommended RF ranges (handbrake.fr docs)

**x264 / x265:**

| Resolution | RF |
|---|---|
| 480p / 576p (SD) | 18 – 22 |
| 720p | 19 – 23 |
| 1080p | 20 – 24 |
| 2160p (4K) | 22 – 28 |

**SVT-AV1:** 480p/576p 22–32, 720p 25–35, 1080p 25–35, 2160p 25–40.

The docs also say:
- **Lower RF means higher quality.** A few RF steps make a visible difference, so change it a little at a time.
- Animation can use 1–2 RF more (it compresses well).
- "Higher than recommended quality settings can lead to extremely large files."

### Why RF rises with resolution
Higher resolutions are watched at the same physical screen size, so each pixel is smaller and artefacts are
less visible. HandBrake's own presets follow this exactly:

| Preset family | 480p | 576p | 720p | 1080p | 2160p |
|---|---|---|---|---|---|
| Fast (x264 `fast`) | 20 | 20 | 21 | 22 | 24 (x265 10-bit, `faster`) |
| HQ (x264 `slow`) | 18 | 18 | 19 | 20 | 22 (x265 10-bit, `medium`) |
| Super HQ (x264 `veryslow`) | 16 | 16 | 17 | 18 | 20 (x265 10-bit, `slow`) |
| **H.265 MKV (x265 10-bit `slow`)** | 20 | 20 | 21 | **22** | 24 |

Our app uses the same per-resolution idea with higher CRFs, because it aims at size (see
[15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)).

## Encoder speed preset vs RF
- The **speed preset** (`ultrafast` … `placebo`) trades encode time for efficiency: at the **same RF**, slower presets give **smaller files at about the same quality**.
- **RF** sets the quality target.
- The presets pair the quality tier with the speed: Fast uses `fast`/`faster`, HQ uses `slow`/`medium`, Super HQ uses `veryslow`/`slow`, and the H.265 MKV and Device 4K presets use `slow`.

## Related
- Encoder mapping of RF: [07-video-encoders](07-video-encoders.md)
- Full preset values: [13-presets](13-presets.md)

Source: <https://handbrake.fr/docs/en/1.6.0/workflow/adjust-quality.html>
