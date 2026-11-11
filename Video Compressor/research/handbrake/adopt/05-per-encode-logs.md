---
title: Adopt — per-encode activity logs
tags: [adopt, reliability, logging, debugging, observability, tuning-data]
source: HandBrake gtk/src/callbacks.c (EncodeLogs), libhb/work.c hb_display_job_info, note 01-architecture-pipeline
related: [00-index, 04-gpu-to-cpu-fallback, 02-x265-texture-tuning, 08-parallel-gpu-encodes, 09-decoder-thread-budget]
---

# 05 · Per-encode log files

## What HandBrake does
- One **activity log per encode**: `~/.config/ghb/EncodeLogs/<output name> YYYY-MM-DD HH-MM-SS.log` in the GTK GUI (`callbacks.c:4171`). There is an option to put it next to the output instead.
- The log starts with the **HandBrake version** and a **full summary of the job** (`hb_display_job_info`: source, filters, encoder settings, audio tracks), and ends with **stats**: average fps, drops and dups from the frame-rate shaper, sync gaps, decode errors.
- These logs are how users report bugs, and how *we* found out what HandBrake really did (the performance work, see `LOG.md`, read them from `~/.var/app/fr.handbrake.ghb/config/ghb/EncodeLogs`).

## What we do now
- `-loglevel error -nostats`, and `main.c` (around line 1186) keeps **only the last stderr line**, for the `Failed` tooltip.
- After the app closes, nothing remains of what happened: no argv, no timing, no sizes. Perf data comes from a separate script (`build/perf/perf.sh`).

## Change
For each encode, write `g_get_user_state_dir()/video-compressor/logs/<name> <date>.log`, which is `~/.local/state/…` (the right XDG place for logs). Contents:
1. App version, date, and the ffmpeg version (`ffmpeg -version`, first line, read once at startup).
2. The probe result (`VcProbe` fields), the class, and the options.
3. The **full argv**, one item per line (copy-pasteable).
4. **All** of ffmpeg's stderr, not just the last line (still `-loglevel error`, so it stays small).
5. The result: exit status, wall time, average fps (frames / time from `-progress`), input → output size, % saved, the decision (kept / already efficient / failed / GPU→CPU fallback), and trash checks.

Keep the last ~200 logs (delete the oldest when writing). Add "Open log" to the row's right-click menu.
Write lines as they arrive (`GOutputStream`), so a crash still leaves a partial log.

## Gain
- A `Failed` row can be diagnosed afterwards, from the full error.
- Real-world speed and size data piles up for free, which helps tune `EST_VIDEO_KBPS`, the class CRFs and the ideas in [02](02-x265-texture-tuning.md), [08](08-parallel-gpu-encodes.md) and [09](09-decoder-thread-budget.md).

## Risk
None to outputs. Logs contain file names and paths; they stay local in the user's state dir.

## How to verify
Run a batch with one good file, one already-efficient file and one stopped file. Check that three logs exist with the fields above, and that the 201st log removes the oldest.

## Effort
S.
