---
title: Adopt — run more than one GPU encode at once
tags: [adopt, speed, optimization, gpu, vaapi, concurrency, queue, worker-process]
source: HandBrake NEWS 1.4.0 (simultaneous jobs, handbrake.worker.exe), win/CS QueueService.cs SimultaneousEncodes, note 01-architecture-pipeline
related: [00-index, note 01-architecture-pipeline, note 14-hardware-acceleration, 04-gpu-to-cpu-fallback, 05-per-encode-logs, 09-decoder-thread-budget]
---

# 08 · Run two GPU encodes at once

## What HandBrake does
- Since **1.4.0**, the Windows and macOS GUIs can run **several queue jobs at once** ("Maximum number of simultaneous encodes", `QueueService.cs`, `SimultaneousEncodes`). The release notes give the reason: "to improve CPU utilisation on high core count systems".
- On Windows each job runs in its own **worker process** (`handbrake.worker.exe`), so a crash in one encode can't take down the GUI or the other jobs.
- Inside one job, libhb is **pipeline-parallel**: reader, decoder, filters, encoder and muxer each run on their own thread, linked by bounded FIFOs (see `01-architecture-pipeline`), so no single stage idles while another works.

## What we do now
- One ffmpeg at a time (SPEC decision 3). That is right for x265: it already uses all 16 threads, and two would need more than the 7 GB of RAM.
- Each encode is already its own process (`GSubprocess`), so we have HandBrake's worker-process isolation for free.
- On the **GPU path**, one encode runs at about 260 fps. The fixed-function encoder, the GPU decoder and the CPU-side demux/mux/audio work take turns rather than overlapping, so some hardware likely sits idle part of the time.

## Change (experiment first)
1. Measure: run 2 (then 3) GPU encodes of different files at once by hand, using the app's argv, and compare
   **total batch time** with running them one after another. Also check RAM and whether the encoder block is saturated (`intel_gpu_top`, Video engine busy %).
2. If the throughput gain is ≥ 1.3×: allow **up to 2 concurrent encodes when the speed is GPU**. Keep 1 for every x265 speed.
3. UI rules that need adjusting: "Compressing n of N · name" shows two names or "2 running"; Pause (SIGSTOP) and Stop must act on every running encode; the time-left estimate adds both speeds.

## Gain
Possibly 1.3–1.8× faster GPU batches on folders of many small phone clips, where per-file start-up (process
launch, probing, GPU init, `faststart` rewrite) is a large share of the time. A single large file gains nothing.

## Risk
Medium: it touches the queue, pause, stop and the progress UI, which are carefully specced. The iGPU shares
memory with the CPU, so watch RAM. Run [05 logs](05-per-encode-logs.md) first, so the per-file timings are already recorded.

## How to verify
Batch-time A/B on a folder of 20 short phone clips and on a folder of 3 long ones. Run the ASan build with concurrency to catch queue bugs.

## Effort
M (queue/UI changes), after an XS measurement that may rule it out.
