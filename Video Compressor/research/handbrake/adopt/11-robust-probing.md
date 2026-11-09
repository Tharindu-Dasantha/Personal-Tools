---
title: Adopt — robust probing and "sanitize, don't fail"
tags: [adopt, reliability, ffprobe, probesize, sanitize, validation]
source: ../02-scan-and-probe.md (probesize 15 MB, geometry check, most-common info), ../09-audio.md (sanitize_audio), Video Compressor src/compress.c vc_build_ffprobe_argv + vc_parse_probe
related: [00-index, ../02-scan-and-probe, ../09-audio, 03-channel-aware-audio, 06-hdr10-static-metadata, 04-gpu-to-cpu-fallback]
---

# 11 · Robust probing and "sanitize, don't fail"

## What HandBrake does
- **Bigger probe window**: `probesize = 15000000` (15 MB, FFmpeg's default is 5 MB), "to successfully scan some files with large PNGs", meaning cover art or attached pictures (`stream.c:5263`).
- **Doesn't trust one reading**: it decodes several previews and keeps the **most common** geometry and frame rate, dropping previews whose decoded size doesn't match the reported one (see [../02-scan-and-probe](../02-scan-and-probe.md)).
- **Skips attached pictures** when choosing the video stream (`AV_DISPOSITION_ATTACHED_PIC`).
- **Sanitizes instead of failing**: an unsupported sample rate, mixdown or bitrate is clamped to the nearest valid value with a log line, rather than aborting the job (`sanitize_audio`, see [../09-audio](../09-audio.md)).

## What we do now
- `ffprobe` with default probe limits. Files ffprobe can't read show as `Unreadable`.
- `-map 0:v:0` picks the **first** video stream. In some MP4/M4V files with cover art, that is the **attached picture**, not the video.
- Audio channels and sample rate aren't probed (see [03](03-channel-aware-audio.md)).

## Change
1. Add `-probesize 15M` to `vc_build_ffprobe_argv`, and `-probesize 15M` before `-i` on the encode, which has the same limit. Cheap and harmless.
2. Probe `disposition=attached_pic` and pick the first video stream **without** it. Map it by index (`-map 0:<index>`) instead of `0:v:0`. Add a fixture: an MP4 with cover art (`-disposition:v:1 attached_pic`).
3. Probe audio `channels` and `sample_rate`. Clamp anything AAC can't encode at our bitrates (for example 8 kHz mono is fine, 192 kHz PCM → `-ar 48000`), and log it once [05](05-per-encode-logs.md) exists.

## Gain
Fewer `Unreadable` rows and wrong-stream encodes on files from editors, downloaders and podcast apps (cover art). Phone clips are unaffected.

## Risk
Low. A larger probe window makes probing large files slightly slower. Up to 4 probes run in parallel, so it is barely noticeable.

## How to verify
Fixtures: an MP4 with a large PNG cover attached first, and a 192 kHz PCM `.mov`. Both should probe and encode correctly.

## Effort
XS for the probesize, S for attached-pic selection and audio clamping with tests.
