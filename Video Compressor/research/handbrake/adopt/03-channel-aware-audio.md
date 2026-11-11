---
title: Adopt — channel-aware audio bitrate
tags: [adopt, audio, aac, surround, 5.1, mixdown, bitrate, bug]
source: note 09-audio (HandBrake hb_audio_bitrate_get_default, sanitize_audio), Video Compressor src/compress.c copies_audio + vc_build_ffmpeg_argv + vc_build_ffprobe_argv
related: [00-index, note 09-audio, 11-robust-probing, 05-per-encode-logs]
---

# 03 · Channel-aware audio bitrate

## What HandBrake does
- The default bitrate **scales with channel count**: AAC gets `80 kbps × full-bandwidth channels` (LFE not counted), so 160 for stereo and 400 for 5.1 (`common.c:1236`, see `09-audio`).
- Or the preset **downmixes** on purpose: almost every preset outputs `aac 160 stereo`, and the Surround presets add an AC-3 passthru track for the 5.1.
- Every audio parameter is **sanitized** against what the codec supports (sample rate, mixdown, bitrate limits), with a log line.

## What we do now (gap)
- `vc_build_ffprobe_argv` (`compress.c:76`) doesn't ask for `channels` or `channel_layout`.
- The class table gives one stereo-sized bitrate (64–160 kbps), and `-c:a aac -b:a <class>` keeps the **source channel count**.
- So **a 5.1 track is squeezed into 128–160 kbps**, about 25–30 kbps per channel. That is clearly audible (swirly highs).
- `copies_audio()` compares the total AAC bitrate with the stereo class bitrate. A 5.1 AAC track at 384 kbps is therefore **re-encoded down to 128 kbps**: a second lossy pass that lands at a bad bitrate.
- `-b:a` applies the same value to **every** mapped audio stream (`-map 0:a?`).

## Change (pick one; recommended first)
1. **Scale with channels, as HandBrake does:** probe `channels` per audio stream. Per output stream use
   `-b:a:N = class_kbps × max(1, fullband_channels) / 2`, so stereo stays the same and 5.1 gets 2.5× the class
   rate. Make the copy rule compare per channel too.
2. **Downmix like HandBrake's presets:** add `-ac 2` when channels > 2. This is the smallest file, but surround
   is lost. That conflicts with "keep the quality", so it would need a user decision or an option.

Add unit tests: a stereo argv (unchanged), a 5.1 argv (scaled bitrate), and the copy decision for 5.1 AAC.

## Gain
Correct audio quality on surround sources (movies, some camera files). For stereo phone clips nothing changes.

## Risk
Low. 5.1 outputs get bigger, as they should. The size estimate's audio term should use the same formula
(`vc_estimate_size`).

## How to verify
Use a 5.1 fixture (`make-fixtures.sh` can make one with `anullsrc=channel_layout=5.1` or `aevalsrc`). Check the output with `ffprobe` (channels, bit_rate), and listen.

## Effort
S: one probe field, the bitrate maths, the copy rule and tests.
