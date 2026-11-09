---
title: HandBrake techniques considered and rejected
tags: [adopt, rejected, video-compressor, yagni]
source: ../ (HandBrake research notes), Video Compressor SPEC.md
related: [00-index, 14-already-adopted, ../15-lessons-for-video-compressor]
---

# Considered and rejected

HandBrake techniques that don't pay off for this app, with the reason, so they aren't re-investigated
without new evidence.

| Technique | HandBrake source | Why not for us |
|---|---|---|
| Keyframe interval `min=fps`, `max=10×fps` | `encx265.c` ([../07](../07-video-encoders.md)) | x265's default `keyint=250` is close (HandBrake gives 300 at 30 fps). Negligible size effect |
| Multi-pass / average bitrate, turbo first pass | `hb_job_setup_passes` ([../08](../08-rate-control-and-quality.md)) | We target quality, not a size. Multi-pass doubles the time. HandBrake turns it off for constant quality too |
| Own A/V sync repair (gap fill, dejitter, SCR) | `sync.c` ([../05](../05-sync-and-timing.md)) | Built for DVD/TS sources with clock jumps. The ffmpeg CLI handles phone and camera files |
| Motion-analysis frame dropping for the fps cap | `vfr.c` `MOTION_ANALYSIS` | HandBrake's own default is `NEAREST`. `-fpsmax` plus ffmpeg's nearest-frame drop is equivalent for 60 → 30 |
| Buffer pools, bounded FIFOs, own threading | `fifo.c`, `work.c` ([../01](../01-architecture-pipeline.md)) | We don't run the pipeline in-process. ffmpeg does all of that inside its own process |
| Linking FFmpeg as libraries instead of the CLI | libhb design | Large rewrite, with GPL and build-system costs, for no output-quality gain. The CLI argv is testable and isolated |
| JSON presets | `preset.c`, `preset_builtin.json` ([../13](../13-presets.md)) | The app's point is "no settings". The class table is one array in `compress.c` |
| Subtitle burn-in, foreign-audio search | `rendersub.c`, `work.c` ([../10](../10-subtitles.md)) | Subtitles are dropped by default or copied (SPEC decision 5) |
| Dolby Vision RPU and HDR10+ passthrough | `rpu.c`, `hdr10plus.c` ([../12](../12-hdr-and-color.md)) | Needs libdovi and per-frame side data the CLI can't pass to x265 easily. The SPEC accepts losing DV (the base layer plays everywhere) |
| Denoise (hqdn3d / NLMeans / BM3D) before encoding | filters ([../06](../06-filters.md)) | Off in every HandBrake preset too. It changes the picture and is slow. Grain kept on purpose ([02](02-x265-texture-tuning.md)) |
| Tone mapping HDR → SDR | `colorspace.c` | The goal is to keep HDR. Tone mapping exists only for the separate video-frames mode |
| Frame-rate snapping (29.97 approximations) | `scan.c` | Only matters for disc and TS sources. Phone VFR clips are handled by `-fpsmax` |
