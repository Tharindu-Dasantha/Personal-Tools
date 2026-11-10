---
title: HandBrake techniques to adopt — index
tags: [handbrake, video-compressor, adopt, roadmap, optimization, index]
source: ../ (HandBrake research notes 00–15) + Video Compressor src/compress.c and src/main.c as of 2026-11-09
related: [../00-index, ../15-lessons-for-video-compressor, 14-already-adopted, 15-rejected]
---

# HandBrake techniques we can adopt

Each note below is one technique HandBrake uses, written against our code. Every note has the same
sections: **What HandBrake does → What we do now → Change → Gain → Risk → How to verify → Effort**.
Background on how HandBrake works is in the parent folder ([../00-index](../00-index.md)).

Only [16](16-encode-speed.md) is implemented so far. Anything that changes output needs the VMAF/size/time check from
[`tasks/plan-performance.md`](../../../tasks/plan-performance.md) before it ships.

## Ranked list

| # | Technique | Kind | Gain | Effort | Risk | Note |
|---|---|---|---|---|---|---|
| 1 | Lanczos scaler for downscales | quality | small, on every resized video | XS (one flag) | low | [01](01-lanczos-scaling.md) |
| 2 | Channel-aware audio bitrate | quality / **bug** | large for 5.1 sources | S | low | [03](03-channel-aware-audio.md) |
| 3 | GPU → CPU fallback on failure | reliability | no "Failed" rows on odd files | S | low | [04](04-gpu-to-cpu-fallback.md) |
| 4 | Per-encode log files | debugging / tuning | faster diagnosis, data for tuning | S | none | [05](05-per-encode-logs.md) |
| 5 | HandBrake's x265 texture tuning | quality and speed | unknown, must measure | XS to try | medium | [02](02-x265-texture-tuning.md) |
| 6 | Run two GPU encodes at once | speed | maybe 1.3–1.8× batch throughput on the GPU path | M | medium | [08](08-parallel-gpu-encodes.md) |
| 7 | Decoder thread budget | speed (x265 path) | a few % less oversubscription | XS to try | low | [09](09-decoder-thread-budget.md) |
| 8 | HDR10 mastering / MaxCLL passthrough | correctness (PQ only) | correct HDR10 tone mapping | M | low | [06](06-hdr10-static-metadata.md) |
| 9 | Quality preview clip | UX | judge quality before a long batch | M | low | [10](10-quality-preview-clip.md) |
| 10 | Robust probing | reliability | fewer "Unreadable" edge cases | XS–S | low | [11](11-robust-probing.md) |
| ✓ | **Adopted:** x265 speed (`ctu=32` small video, `rect=0:rskip=2` at slow) | speed | slow 2× faster on 324p, 20 % on 720p/1080p | done | low | [16](16-encode-speed.md) |
| 11 | 10-bit encode for 8-bit sources | quality / size | possible, must measure | XS to try | **SPEC decision** | [07](07-10bit-for-8bit-sources.md) |
| 12 | Selective deinterlacing | quality (old camcorder files) | large *if* such files exist | S | low | [12](12-selective-deinterlace.md) |
| 13 | Auto-crop black bars | size (letterboxed files) | 5–25 % fewer pixels on those files | M | **high** (crops content if wrong) | [13](13-auto-crop.md) |

Effort: XS < 1 h · S ≈ half a day · M ≈ 1–2 days, including tests.

## Suggested order
1. **Quick, safe wins:** [01 lanczos](01-lanczos-scaling.md), [03 audio](03-channel-aware-audio.md), [04 fallback](04-gpu-to-cpu-fallback.md), [05 logs](05-per-encode-logs.md). Logs first if you plan to measure the rest: they record the data.
2. **Measure on the `build/perf` corpus:** [02 x265 tuning](02-x265-texture-tuning.md), [09 decoder threads](09-decoder-thread-budget.md), [08 parallel GPU](08-parallel-gpu-encodes.md), [07 10-bit](07-10bit-for-8bit-sources.md).
3. **Only when such files show up:** [06 HDR10](06-hdr10-static-metadata.md), [12 deinterlace](12-selective-deinterlace.md), [13 auto-crop](13-auto-crop.md).
4. **Feature work:** [10 preview clip](10-quality-preview-clip.md), [11 robust probing](11-robust-probing.md).

## By category
- **Quality:** [01](01-lanczos-scaling.md), [02](02-x265-texture-tuning.md), [03](03-channel-aware-audio.md), [07](07-10bit-for-8bit-sources.md), [12](12-selective-deinterlace.md)
- **Speed / optimisation:** [02](02-x265-texture-tuning.md), [08](08-parallel-gpu-encodes.md), [09](09-decoder-thread-budget.md), [13](13-auto-crop.md)
- **Reliability:** [04](04-gpu-to-cpu-fallback.md), [05](05-per-encode-logs.md), [11](11-robust-probing.md)
- **Correctness:** [03](03-channel-aware-audio.md), [06](06-hdr10-static-metadata.md)
- **UX:** [10](10-quality-preview-clip.md)

## Also see
- [14-already-adopted](14-already-adopted.md): HandBrake techniques the app already uses (so nobody re-proposes them).
- [15-rejected](15-rejected.md): HandBrake techniques considered and not worth it for this app, with reasons.
