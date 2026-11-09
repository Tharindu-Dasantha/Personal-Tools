---
title: HandBrake audio handling
tags: [handbrake, audio, aac, passthru, auto-passthru, mixdown, bitrate, opus, ac3, resample]
source: libhb/work.c (sanitize_audio), libhb/common.c, libhb/encavcodecaudio.c, libhb/decavcodec.c, libhb/sync.c
related: [00-index, 04-decoding, 05-sync-and-timing, 13-presets, 15-lessons-for-video-compressor]
---

# Audio handling

## Audio pipeline per track
```
reader → audio decoder (decavcodeca) → sync → audio filters (resample / remap / avaudiofilters) → audio encoder → muxer
                   └──────────── passthru: decoder is replaced by a passthrough work object (workpass.c) ───────────┘
```
Each track gets its own FIFOs: `fifo_in`/`fifo_out` are LARGE (32) and `fifo_raw`/`fifo_sync` are SMALL (16).
See [01-architecture-pipeline](01-architecture-pipeline.md).

## Track selection (preset keys)
- `AudioTrackSelectionBehavior`: `first` / `language` / `all`.
- `AudioList`: an ordered list of output encodings applied to each selected source track. For example, the
  Surround presets make **two outputs** per source: `aac 160 stereo` + `copy:ac3`.
- `AudioSecondaryEncoderMode=true`: the second and later entries only apply to the first track.

## Passthru, auto-passthru and fallback (`sanitize_audio`, `work.c:1147`)
1. `hb_autopassthru_apply_settings()` resolves `HB_ACODEC_AUTO_PASS`: if the source codec is in **`AudioCopyMask`**, the track is **copied**; otherwise it is encoded with **`AudioEncoderFallback`**.
   - The H.265 MKV presets use `AudioCopyMask=["copy:aac"]` with fallback `aac`. Device presets allow `copy:ac3` and so on.
2. An explicit passthru whose input codec doesn't match (for example `copy:ac3` on an AAC source) **drops the track**, with a log line.
3. A passthru track keeps the source sample rate and layout. Unneeded filters are removed.
4. For encoded tracks, every parameter is **sanitized** with a log line:
   - Sample rate: `auto` means same as source, then snapped to one the codec supports.
   - Mixdown: if unspecified, the best for the codec. If unsupported, the nearest valid one.
   - Compression level, quality (VBR) or bitrate: clamped to the codec's limits.
   - Dithering is turned off when the codec doesn't support it.

## Default bitrate formula (`hb_audio_bitrate_get_default`, `common.c:1236`)
`nchannels` = full-bandwidth channels of the mixdown (LFE excluded). The result is shifted down for low
sample rates (`sr_shift`: halved below 32 kHz and so on), then snapped to a valid value.

| Codec | Formula | Stereo | 5.1 |
|---|---|---|---|
| AAC (ffmpeg/CoreAudio/FDK LC), MP3, Vorbis, … | `nch × 80` | **160** | 400 |
| HE-AAC | `nch × 32` | 64 | 160 |
| Opus | `coupled × 96 + uncoupled × 64` | 96 | |
| AC-3 | `nch × 128 − 32 × (nch < 5)` | 224 | 640 |
| E-AC-3 | `nch × 384 − 128 × (nch−2) × (nch > 2)` | 768 | 1536 (mono 384) |
| Lossless (FLAC, ALAC, TrueHD, PCM) | no bitrate | | |

Almost every preset uses **AAC 160 kbps stereo**. The Creator presets use 320, and the Android SD presets use 128.

## Encoders (`encavcodecaudio.c`)
| HB codec | libavcodec encoder |
|---|---|
| `ca_aac` / `ca_haac` | Apple CoreAudio (macOS only) |
| `fdk_aac` / `fdk_haac` | `libfdk_aac` (only if built with it, which is non-free) |
| **`av_aac`** | **FFmpeg's native `aac`** (the usual Linux build) |
| `mp3` | `libmp3lame` |
| `opus` | `libopus` |
| `ac3`, `eac3`, `flac`, `alac`, `truehd`, `pcm` | FFmpeg native |

AAC detail: 5.1 and 6.1 inputs are remapped to the **"back"** layouts so the encoder writes a standard
channel-config index instead of a PCE (which some players can't read).

## Mixdown and resampling
- AC-3, DTS and TrueHD can be **downmixed inside the decoder** (see [04-decoding](04-decoding.md)).
- Otherwise `audio_resample.c` (swresample) handles sample rate, sample format (float internally) and the downmix matrix. `AudioNormalizeMixLevel` controls normalisation.
- Mixdowns: mono, left-only, right-only, stereo, **Dolby Surround**, **Dolby Pro Logic II**, 5.1, 6.1, 7.1, 5.2 LFE, and none (passthru).
- Gain (dB) and **DRC** (dynamic range compression, for AC-3 sources) are applied per track.
- 1.12.0 adds `avaudiofilters.c` (libavfilter-based audio filters).

## Sync interaction
- Gaps of 20 ms to 60 s in encoded audio are **filled with silence**. Passthru audio is **dropped or trimmed**, because it can't be synthesised. See [05-sync-and-timing](05-sync-and-timing.md).

## Related
- Our AAC copy rule compared: [15-lessons-for-video-compressor](15-lessons-for-video-compressor.md)
- Preset audio values: [13-presets](13-presets.md)
