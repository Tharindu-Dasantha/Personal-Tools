---
title: HandBrake architecture and pipeline
tags: [handbrake, architecture, threads, fifo, pipeline, libhb]
source: libhb/work.c, libhb/fifo.c, libhb/hb.c
related: [00-index, 03-demux-and-reader, 04-decoding, 05-sync-and-timing, 06-filters, 07-video-encoders, 11-muxing-and-metadata]
---

# Architecture and pipeline

## Layers

| Layer | What it is | Where |
|---|---|---|
| Front ends | GTK GUI (`gtk/`, Linux), WinForms/WPF (`win/`), macOS app (`macosx/`), `HandBrakeCLI` (`test/`) | outside `libhb` |
| JSON API | Front ends build a JSON job; `hb_json_to_job()` expands it into a `hb_job_t` | `libhb/hb_json.c` |
| Presets | JSON presets fill in job settings | `libhb/preset.c`, `preset/*.json`, see [13-presets](13-presets.md) |
| libhb core | Scan, work (encode) and state reporting | `libhb/hb.c`, `scan.c`, `work.c` |
| Codec/format libs | FFmpeg (libavformat/libavcodec/libavfilter/libswscale), x264, x265, SVT-AV1, libass, zimg, libdovi… | `contrib/` |

HandBrake **links FFmpeg as libraries**. It does not run the `ffmpeg` CLI. It bundles its own FFmpeg build
plus x265 builds for 8, 10 and 12 bits (`contrib/x265_8bit`, `x265_10bit`, `x265_12bit`).

## Two top-level threads

- **Scan thread** (`hb_scan_init`, `scan.c`) probes the source into a *title set*. See [02-scan-and-probe](02-scan-and-probe.md).
- **Work thread** (`hb_work_init`, `work.c:55`) takes jobs off the queue one at a time and runs them.

`work_func` (`work.c:107`):
1. Pops a job. For a JSON job it **rescans the title** first, because the job's defaults come from the title.
2. `hb_job_setup_passes()` (`hb.c:1947`) splits the job into passes: an optional *subtitle scan* pass
   (foreign audio search), N *analysis* passes if multi-pass, and a *final* pass. Otherwise it is a single *encode* pass.
3. Runs `do_job()` once per pass. Passes share data through the **interjob** struct, for example the real
   frame rate measured in pass 1 (`correct_framerate`, `work.c:874`).

## do_job: building the pipeline (`work.c:1716`)

In order:
1. Adds the **Reader** work object.
2. Turns off hw decode unless the scan marked it as supported (`title->video_decode_support`).
3. `sanitize_subtitles()` may add the subtitle **render (burn-in)** filter.
4. `sanitize_filter_list_pre()` always inserts the **Adapter** filter (it applies source rotation and pixel
   format conversion), turns on *selective* deinterlace when comb detect is present, and **drops crop/scale
   entirely if the output size equals the source and there is no crop**. See [06-filters](06-filters.md).
5. Decides whether to use a **full hardware pipeline** (`hb_hwaccel_can_use_full_hw_pipeline`). See [14-hardware-acceleration](14-hardware-acceleration.md).
6. Picks the **best pipeline pixel format** (`hb_get_best_pix_fmt`). See [12-hdr-and-color](12-hdr-and-color.md).
7. Calls each filter's `init()`. Filters can change geometry, pix_fmt, colour, frame rate and CFR mode, and the
   job is updated from the result. **A filter that fails to init is dropped with a log line, and the encode goes on without it.**
8. `hb_avfilter_combine()` merges runs of avfilter-based filters into one libavfilter graph.
9. Each filter's `post_init()` then runs with the final job values.
10. Builds the FIFOs and work objects: video decoder, sync, audio decoders/encoders, subtitle decoders/encoders, video encoder, and finally the **Muxer**, always added last.
11. `init()` on every work object. Any failure aborts with `HB_ERROR_INIT`.
12. Starts **one thread per work object and one thread per filter** (`hb_thread_init(..., HB_LOW_PRIORITY)`).
13. Waits on the **last** work object (the muxer), then tears down in a careful order: filters, then work threads, then `close()`, then FIFOs.

## Work objects and the work loop

```c
// hb_work_object_t (simplified)
id, name, init(w, job), work(w, &buf_in, &buf_out), close(w), info(), bsinfo(), flush()
fifo_in, fifo_out, private_data, status, done, die
```

`hb_work_loop()` (`work.c:2422`):
- `buf_in = hb_fifo_get_wait(fifo_in)` blocks with a 200 ms timeout (`FIFO_TIMEOUT`).
- `w->work()` returns `HB_WORK_OK` or `HB_WORK_DONE`.
- **Chapter marks are copied** from input to output buffer (`copy_chapter`), so they survive stages that reorder or drop frames.
- The output is pushed with `hb_fifo_full_wait()` and `hb_fifo_push()`. A full queue blocks the producer, and that is the **backpressure**.
- A work object with no `fifo_in` (the reader) is a *source* and yields when it has no output.
- After `DONE` it **keeps draining its input** until the job ends, so upstream threads never deadlock on a full FIFO.

`filter_loop()` (`work.c:2506`) is the same, except that filters may drop buffers, so it **remembers a pending
chapter mark** and puts it on the next output buffer whose start ≥ the chapter time.

## FIFOs (`fifo.c`)

```c
struct hb_fifo_s { lock, cond_full, cond_empty, capacity, thresh, size, first, last, ... }
```

Sizes used by `do_job` (`work.c:40-47`):

| Name | Capacity / wake | Used for |
|---|---|---|
| `FIFO_MINI` | 4 / 3 | between filters (video frames are big) |
| `FIFO_SMALL` | 16 / 15 | demuxed video in, decoded raw video, sync out |
| `FIFO_LARGE` | 32 / 16 | encoded video out, audio in and out |
| `FIFO_UNBOUNDED` | 65536 | subtitles, which can arrive all at once (SSA), so blocking would deadlock the reader |

The **wake threshold** stops the producer waking after every single pop.

**Buffer pools:** `hb_buffer_init()` takes buffers from size-bucketed pools (`BUFFER_POOL_FIRST`=10 …
`LAST`=25, powers of two), so per-frame allocations are reused. `HB_NO_BUFFER_POOL` and `HB_BUFFER_DEBUG`
turn this off for valgrind.

**`hb_buffer_t`** holds data or AVFrame planes, plus `s.start/stop/duration` (90 kHz), `s.new_chap`,
`s.frametype`, `s.flags`, colour fields and `side_data` (HDR10+, DOVI RPU and so on).

## Threading model, summarised

- A **pipeline-parallel** design: reader, decoder, sync, each filter, encoder and muxer run at the same time.
- Inside a stage there is more parallelism: libavcodec frame+slice threads in the decoder (see [04-decoding](04-decoding.md)), x265's own thread pools, and a custom `taskset` slice threading in the heavy filters (decomb, comb detect, nlmeans).
- All threads run at **low priority**, so the desktop stays responsive.

## Progress and state

`hb_get_state()` reports `pass`, `pass_count`, `progress`, `rate_cur`, `rate_avg` (fps) and `eta_seconds`.
The reader's `UpdateState` drives progress from position against title duration. The log ends with
"average encoding speed for job is X fps".

## Related

- Inputs: [02-scan-and-probe](02-scan-and-probe.md), [03-demux-and-reader](03-demux-and-reader.md)
- Stages: [04-decoding](04-decoding.md) → [05-sync-and-timing](05-sync-and-timing.md) → [06-filters](06-filters.md) → [07-video-encoders](07-video-encoders.md) → [11-muxing-and-metadata](11-muxing-and-metadata.md)
