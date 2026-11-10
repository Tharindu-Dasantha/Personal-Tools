---
title: Adopted — x265 encode speed (ctu=32 for small video, rect=0:rskip=2 at slow)
tags: [adopt, adopted, speed, x265, ctu, wpp, rect, rskip, threads, benchmark]
source: benchmarks on this laptop (i5-1240P, 16 threads) 2026-11-10; HandBrake log of the same file; ../13-presets.md (HandBrake x265 tuning string)
related: [00-index, 02-x265-texture-tuning, 08-parallel-gpu-encodes, 09-decoder-thread-budget, 14-already-adopted, ../07-video-encoders, ../13-presets]
---

# 16 · x265 encode speed (adopted 2026-11-10)

## Why
On a 58 min 576×324 clip, HandBrake took 6.4 min and the app 12+ min (at `medium`). HandBrake's log shows it
used **x264 `fast` RF 20** (H.264, much cheaper than HEVC) and re-encoded the audio at 160 kbps. Its file came
out **~30 % bigger than the source**, while ours was 23 % smaller. So the jobs weren't comparable.

The real problem was ours: **x265 kept only ~4 of 16 threads busy on small video.** x265 encodes rows of
64 px blocks (CTUs) in parallel (WPP), and a 324 px frame has only 6 rows.

## Measurements (2 min cut of that clip, 2405 frames, VMAF against the cut)

| Pipeline | Time | Threads busy | Size | VMAF |
|---|---|---|---|---|
| `slow` (old default) | 64.2 s | 3.7 | 5.65 MB | 98.44 |
| `slow` + `ctu=32` | 46.8 s | 7.0 | 5.77 MB | 98.45 |
| **`slow` + `ctu=32:rect=0:rskip=2`** (new) | **33.1 s** | | 5.81 MB | 98.42 |
| `medium` (old) | 25.2 s | 4.3 | 5.40 MB | 97.33 |
| **`medium` + `ctu=32`** (new) | **18.6 s** | 7.2 | 5.52 MB | 97.36 |
| GPU (QP 26) | 1.8 s | 0.7 | 5.15 MB | 95.64 |
| HandBrake's job (x264 fast RF 20) | 8.4 s | 7.1 | 9.12 MB | 97.76 |

`rect=0:rskip=2` at `slow` on real clips (video only, `ctu` unchanged):

| Clip | Time | Size | VMAF |
|---|---|---|---|
| 1080p H.264 17 Mbps, 21 s | 88.9 → 71.0 s (−20 %) | +1.3 % | 94.50 → 94.30 |
| 720p H.264 9 Mbps, 33 s | 76.0 → 59.6 s (−22 %) | −0.1 % | 96.40 → 96.17 |

## What changed
`add_video_output` in `src/compress.c`:
- output short side ≤ 576 → `ctu=32`
- `slow` / `slower` → `rect=0:rskip=2` (HandBrake's tuning; `medium` and faster already have `rect` off)
- `slow` time factor 2.1 → 1.7 (speed guide and time-left estimate)

## Tried and dropped
- `pmode`/`pme`: slower (medium 95 → 51 fps).
- `frame-threads=8`: no gain.
- 4 small encodes at once: 1.5× throughput at 12.4 threads busy. Parallel encoding is "Ask first" in the SPEC, and `ctu=32` already recovers most of it.

## Not measured
`ctu=32` at 720p+ (not applied there; those already fill the CPU). `slower` with the new tuning.
