---
title: Adopt — leave CPU headroom by capping decoder threads
tags: [adopt, speed, optimization, threads, decoder, x265, oversubscription]
source: ../04-decoding.md (HandBrake hb_avcodec_open: cpu_count/2+1, FF_THREAD_FRAME|SLICE), Video Compressor src/compress.c add_input_options
related: [00-index, ../04-decoding, 08-parallel-gpu-encodes, 05-per-encode-logs]
---

# 09 · Decoder thread budget

## What HandBrake does
The video decoder gets **`cpu_count / 2 + 1` threads** (`hb.c:93`, see [../04-decoding](../04-decoding.md)), with
frame + slice threading, while the encoder (x265) runs its own thread pools on all cores. The decoder is
deliberately given less, so it doesn't compete with the encoder, which is the real bottleneck. Threading is
turned off for sources whose resolution changes mid-stream.

## What we do now
On the x265 path, ffmpeg's decoder uses its own **auto** thread count, and x265 creates pools for all 16
threads, so both claim the whole CPU (oversubscription: more context switches and cache thrash).

## Change (experiment first)
Add `-threads 9` (that is, 16/2 + 1, computed from `g_get_num_processors()`) as an **input** option (before `-i`)
on the x265 path only. Not on the GPU path, where decode runs on the GPU. A one-line change in `add_input_options`.

## Gain
Probably small: a few % on the x265 path, mostly for high-bitrate H.264 or HEVC sources where decode isn't
trivial. It may also smooth desktop responsiveness a little, on top of nice 10.

## Risk
Low. Too few decoder threads could starve x265 on `faster`/`fast`, where encoding is quicker, so measure those too.

## How to verify
Time 3 clips at `faster` and `slow`, with and without the flag (repeat runs, since times are ±5 % on this
laptop). Adopt only if no speed gets slower.

## Effort
XS.
