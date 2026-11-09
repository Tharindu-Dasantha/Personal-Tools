---
title: Adopt — fall back from GPU to CPU instead of failing
tags: [adopt, reliability, gpu, vaapi, fallback, degrade-gracefully, x265]
source: ../14-hardware-acceleration.md, ../01-architecture-pipeline.md (filter init failure), Video Compressor src/main.c encode-finished handler
related: [00-index, ../14-hardware-acceleration, ../01-architecture-pipeline, 05-per-encode-logs, 08-parallel-gpu-encodes]
---

# 04 · GPU → CPU fallback on failure

## What HandBrake does
HandBrake **degrades instead of failing** at several levels:
- A hw device that fails to initialise logs "Failure to initialise hwaccel" and the job **continues with software decode** (`work.c` `do_job`).
- A filter whose `init()` fails is **removed and the encode goes on** (see [../01-architecture-pipeline](../01-architecture-pipeline.md)).
- A hwaccel that can't handle the filters, rotation, colour range or encoder makes HandBrake **download frames and continue on the CPU** (see [../14-hardware-acceleration](../14-hardware-acceleration.md)).
- Hardware encoders are **test-opened** before they're offered (`hb_vaapi_encoder_available`).

## What we do now
- The startup 1-frame test encode decides whether GPU is offered. That matches HandBrake.
- GPU **decode** already falls back on its own: `-hwaccel vaapi` lets ffmpeg decode unsupported codecs on the CPU.
- But if the **GPU encode itself fails** for one file (an odd size or pixel format, a driver error), `main.c` (around line 1121) marks the row **`Failed`** with the last stderr line. The user has to switch to a CPU speed and re-run it by hand.

## Change
In the encode-finished handler, when the run used `VC_PRESET_GPU`, ffmpeg exited non-zero, and the user didn't stop it:
1. Delete the `.part`.
2. Re-queue the same item **once** with the x265 `medium` argv. `medium` is the closest CPU equivalent to the GPU's quality, per SPEC decision 8.
3. Show a meta note `GPU failed, encoded on CPU` with the GPU error in the tooltip.
4. After three GPU failures in a row, stop trying the GPU for the rest of the batch. That is the same pattern the sample encoder already uses.

Keep `vc_build_ffmpeg_argv` pure: the retry is only a different `VcOptions.preset`. Test the decision ("should retry on CPU?") as a small pure function in `compress.c`.

## Gain
A batch never stops at a `Failed` row just because one file doesn't suit the GPU.

## Risk
That one file takes much longer (x265 `medium` is about 12× the GPU's time). The time-left estimate jumps for it. That is acceptable, and the note explains it.

## How to verify
Force a GPU failure with a fixture the low-power encoder rejects (for example a tiny 64×64 clip, or an
unsupported pixel format; check which one fails on this Iris Xe first), and confirm the row ends with the CPU result and the note.

## Effort
S.
