# Spec: Video Compressor

> Module `video-compress` of Media Tools (history in [LOG.md](LOG.md)). The app is growing into **Media Tools**, with web video,
> video → frames and image compression as further modes (each with its own `SPEC-<id>.md`). This file stays the spec for the
> `Compress video` mode and the source of the shared rules in **Boundaries**. The host window changes (mode dropdown,
> Choose Files, multi-step jobs) are in [SPEC-app-shell.md](SPEC-app-shell.md).

## Objective

A small Linux desktop app, written in C with GTK4. You pick a folder, it finds every video in it, works out what each video is (resolution, codec, size), and compresses each one with H.265 (x265) by calling `ffmpeg`. This is the kind of "keep the quality, cut the size" job HandBrake does, but it runs on a whole folder with no settings to choose.

**User:** me (single user, my own Fedora machine). I want to drop a folder of 240p–1080p+ videos in and get much smaller files out without thinking about encoder settings.

**User stories**
- I click **Choose Folder** and see every video in it, with its detected class (`240p`, `480p`, `720p`, `1080p`, `4K`…), codec, duration and size.
- I click **Start**. The videos are compressed one after another, each with a progress bar. When a file finishes I see its new size and the % saved.
- Originals are never touched. Results go to `<folder>/compressed/`.
- I can **Cancel** at any time. No half-written files are left behind.
- I can tick **Delete originals after compressing** (off by default). With it on, each original is moved to the Trash, but only after its compressed copy has been checked and is confirmed smaller.
- I can tick **Keep subtitles** (off by default). With it on, the output is `.mkv` and keeps the subtitle tracks.
- I can change the **Speed** (default `medium` since the v3 design, 2026-11-11; was `slow`) to trade file size for encode time.

**Why ffmpeg and not HandBrake:** HandBrake is installed here only as the Flatpak GUI (`fr.handbrake.ghb`), with no `HandBrakeCLI`. System `/usr/bin/ffmpeg` already has `libx265`, the same encoder HandBrake uses for H.265, so the output quality is the same.

## Decisions
1. The app is Linux only (Fedora, GTK 4.22). **No Windows or macOS**, confirmed.
2. The scan is **not recursive**, confirmed: only files directly in the chosen folder. The `compressed/` subfolder is always skipped.
3. Video encodes run **one at a time** (Compress images runs several at once, see SPEC-app-shell.md Decision 4). On 720p and up x265 keeps most of the 16 threads busy. On small video it doesn't (a 576×324 clip used ~4 at `slow`), so outputs with a short side ≤ 576 get `ctu=32`, which roughly doubles the parallel work (2026-11-10, research/handbrake/adopt/16-encode-speed.md). Four small encodes at once gave only 1.5× more throughput, so parallel encoding stays out (Ask first).
4. Resolution is **kept by default**. Opt-in **Max size** (`Original size` / `Max 1080p` / `Max 720p`, D6 2026-11-08) downscales by the short side; it never upscales.
5. Subtitles are **dropped by default**. The **Keep subtitles** option keeps them (see below). The first video stream and all audio streams are always kept.
6. The x265 preset **defaults to `medium`** (since 2026-11-11, at the user's request for the v3 design) and can be changed in the UI. Until then it was `slow`: on 2026-11-09 `medium` took 46% of the time but scored 1.6–2.7 VMAF lower on most real clips (see [LOG.md](LOG.md)). Whether to keep `medium` is open in [PERFORMANCE.md](PERFORMANCE.md).
7. The frame rate is **kept by default**. Opt-in **Max 30 fps** (D5, 2026-11-08) caps sources above 30 fps.
8. A **GPU** speed (Intel VA-API `hevc_vaapi`, D3 2026-11-08) is offered when a 1-frame test encode works at startup. Same codec (HEVC). Measured on real clips: at the class QP (`gpu_qp`, 28 for 720p/1080p) it matches `medium`'s quality at about 8% of its time and about 1.1× its size (1.06–1.41×). GPU decode is used when possible; other codecs decode on the CPU. Phone videos with a rotation tag are rotated on the GPU (`transpose_vaapi`), because ffmpeg drops the tag on this path.
9. **Start with nothing selected skips videos predicted to save nothing** (D2, 2026-11-08). Selecting one, or its own play button, still compresses it. Unknown durations are always compressed.
10. Outputs **keep the source's date and phone metadata** (location, creation date, camera; D4 2026-11-08), and the file's modified time.

## Auto-detection and encode settings

The app runs `ffprobe` on each file and reads its width, height, rotation, codec, duration, bitrate and file size.

**Resolution class** is based on the *short side*, `min(width, height)`, so a portrait phone video at 1080×1920 counts as 1080p. The short side also gives the same answer whether or not the rotation metadata is applied.

| Short side (px) | Class | x265 CRF | Audio |
|---|---|---|---|
| ≤ 300 | 240p | 24 | AAC 64k |
| ≤ 400 | 360p | 24 | AAC 96k |
| ≤ 576 | 480p | 25 | AAC 96k |
| ≤ 800 | 720p | 26 | AAC 128k |
| ≤ 1200 | 1080p | 27 | AAC 128k |
| > 1200 | 1440p/4K | 28 | AAC 160k |

Low resolutions get a lower CRF, which means higher quality, because each pixel covers more of the screen and artifacts are easier to see. This table lives in one array in `compress.c` and is the only place to tune quality against size.

**Fixed for every file:** `-c:v libx265` (or `hevc_vaapi` for GPU). Audio is re-encoded with `-c:a aac` at the class bitrate, except when every audio stream is already AAC at no more than the class bitrate + 10%: then it is copied (`-c:a copy`), which avoids a second lossy pass. The frame rate is kept unless **Max 30 fps** is on and the source is above 30.5 fps (`-fpsmax 30`). With **Max size**, the class (CRF, QP, audio) is chosen from the output size.

**Bit depth / HDR:** the source's bit depth is kept.
- *8-bit sources* (`pix_fmt` without a real depth marker `p10`/`p12`/`10le`/`10be`/`12le`/`12be`; e.g. `yuv420p`, `yuvj420p`, `nv12`, `yuv410p`) are encoded with `-pix_fmt yuv420p`.
- *10-bit or higher sources* (e.g. `yuv420p10le`, `p010le`, `yuv422p10le`; iPhone HDR is 10-bit HLG) are encoded with `-pix_fmt yuv420p10le -profile:v main10`, and the source's colour tags are copied: `-color_primaries`, `-color_trc` and `-colorspace`, with values from ffprobe. This keeps the file HDR, so it isn't washed out. Each tag is copied only if ffprobe reports it (not `unknown`).
- PQ (HDR10, `smpte2084`) sources also get `-x265-params …:hdr10=1:repeat-headers=1` so players switch to HDR. HLG needs nothing extra. Mastering-display/MaxCLL values are not passed through.

**x265 speed tuning** (`-x265-params`, measured 2026-11-10): outputs with a short side ≤ 576 add `ctu=32` (more rows to encode in parallel). `slow` and `slower` add HandBrake's `rect=0:rskip=2` (no rectangular partitions, edge-based recursion skip), which cuts their time with no VMAF loss. Other presets already have `rect` off.
- Limit: Dolby Vision metadata is not carried over. Those files keep their HLG/HDR10 base layer, which is what plays on most devices anyway.
- The list row shows a `10-bit HDR` or `10-bit` tag so you can see which files take this path.

**Speed (x265 preset):** a segmented control with `faster`, `fast`, `medium`, `slow` (**default**) and `slower`. The choice applies to the whole batch. Measured on this laptop (2026-11-08, a noisy 16 Mbps 1080p clip and the 720p fixture), against `medium`: `faster` 0.69× the time, `fast` 0.85×, `slow` 2.1×, `slower` 8.2×. At the same CRF the sizes stayed within about 8% of `medium` (slower presets came out slightly *larger*, because they keep more detail), so the speed mostly buys quality, not a smaller file. The earlier claim ("`slow` is 5–10% smaller") did not hold on these clips. Each launch starts at `slow`.

**Container / subtitles:**
- *Keep subtitles off (default):* output is `.mp4` with `-sn -tag:v hvc1 -movflags +faststart+use_metadata_tags`. `hvc1` lets the file play in QuickTime and on iOS; `use_metadata_tags` keeps the phone's `com.apple.quicktime.*` keys (location, camera).
- Every output uses `-map_metadata 0` (ffmpeg 8 drops `creation_time` without it), and gets the original's modified time after the `.part` is renamed.
- *Keep subtitles on:* output is `.mkv` with `-map 0:s? -c:s copy`. MP4-style `mov_text` subtitles can't be copied into MKV, so if a file has them the app uses `-c:s srt` instead (text is preserved). Bitmap subtitles (PGS, DVD) are copied as they are. A file with no subtitles still produces `.mkv` while the option is on, so the output type doesn't change from file to file within a batch.

**"Not smaller" rule:** if the encoded file is not at least 5% smaller than the original, the app deletes it and marks the row `Already efficient`. This happens with files that are already HEVC/AV1 or very low bitrate.

**Delete originals (optional):** a checkbox that is off by default. When it is on, **Start** first asks for confirmation ("N originals will be moved to Trash after compressing"). An original is removed only when **all** of these are true:
1. ffmpeg exited 0 and the `.part` file was renamed into place
2. `ffprobe` on the output succeeds, and its duration is within 1 s of the source's
3. the output passed the "not smaller" rule (if it didn't, the original is kept anyway)

Removal uses `g_file_trash()`, which moves the file to the Trash so it can be recovered. If trashing fails (for example, on a filesystem with no Trash), the original is **kept** and the row says `Original kept (trash failed)`. The app never permanently deletes a file. Rows say `Original in Trash` so it is clear what happened.

**Controls (v2 layout, Figma "v2 – New features", 2026-11-08):**
- **Empty state:** a drop zone ("Drop a folder of videos here") with **Choose Folder…**. Dropping a folder on the window loads it, unless something is running.
- **Header:** the folder name, its path (`~/…`) under it, and **Choose Folder…** (hidden while running and once finished).
- **List:** columns Name (codec · duration · size), Quality (class and `10-bit`/`10-bit HDR` chips) and Size → Estimated. Each row has a checkbox; Ctrl+click and Shift+click on a row also select.
- **Bottom bar, ready:** Speed (segmented, with the ⓘ guide and a `≈ N% smaller · F× Medium time` caption), **Keep subtitles**, **Move originals to Trash** and **Max 30 fps** switches, the **Max size** dropdown, the batch total (`221 MB → ≈ 43 MB`, `N videos ready · estimated for Slow`), and round **Start** / **Pause** / **Stop**. Every icon button has a tooltip (title, shortcut, one line of help) and an accessible label.
- **Bottom bar, running:** `Compressing n of N · name`, an overall bar weighted by size, `About N min left` (from the current file's speed; `Press play to resume` while paused) and what was saved so far.
- **Batch buttons act on all videos.** **Start** (Space) queues every new, stopped, failed or held video; while paused it becomes **Resume** and only resumes the paused encode. **Pause** (Space) freezes the running encode (`SIGSTOP`), so there is still only one ffmpeg alive; nothing else starts until it is resumed (`SIGCONT`) or stopped. **Stop** (Esc) kills ffmpeg, deletes the `.part`, and takes everything else off the queue. Esc clears the selection first if there is one.
- **Per-video buttons** (on hover, always on the running or held row; the same actions on right-click): **Compress next** (moves it to the front of the queue, or compresses only it when idle; resumes it when paused), **Pause/Hold** (a held video is skipped until its play button is pressed), **Stop/Skip** (stop during the output check keeps the original), **Show in Files** and **Move original to Trash** (finished videos with a kept output; no dialog, a toast says it can be restored from the Trash).
- **Selection bar:** when videos are checked, a floating bar runs Start / Pause / Stop / Move originals to Trash on just those. Its Trash asks for confirmation first. Start queues in list order.
- **Finished:** a summary card (`You saved X`, videos compressed and how long it took, already efficient and unreadable counts, Before / After / Smaller), and **Choose Another Folder…** and **Show in Files** (opens `compressed/`).
- Row status: dashed pill = estimate, green `✓ −N%` = real result, `On hold`, `● Waiting`, a progress bar with percent while encoding, `Already efficient`, `Unreadable`, `Failed` / `Stopped` (tooltip has the reason). The meta line adds `Original in Trash`, `Original kept` (tooltip says why) or `Already compressed`.
- Trash still applies the same three checks above before calling `g_file_trash()`, including for outputs from an earlier run.
- A video that came out `already efficient` is finished: it isn't encoded again in the same session.
- Speed, Keep subtitles, the Trash switch, Max 30 fps, Max size and Choose Folder are locked while anything is queued, encoding or being checked.
- Choose Folder stays off until the startup tool check (run asynchronously, so the window paints at once) passes.
- A video that bulk Start skipped says `No saving · skipped` (tooltip: select it to compress anyway); if nothing else is left, the total says `N videos likely won't get smaller · select to compress anyway`.
- ffmpeg runs at nice 10 and idle I/O priority, so the desktop stays responsive. Up to 4 `ffprobe`s run at once while a folder loads.
- **Background:** closing the window while anything is queued, encoding or being checked asks **Run in Background** (default) / **Stop and Quit** / Cancel. In the background the window is hidden and the batch goes on (a paused encode is resumed). Opening the app again, or clicking its notification, shows the window. While a batch runs the app blocks automatic suspend. When a batch ends and the window isn't focused (hidden or behind others), a notification gives the summary title and line (`You saved X` / `N videos compressed in M min`, plus `N failed`). Notifications need the installed `.desktop` file (`cmake --install`), whose name matches the app id.
- Loading a folder deletes leftover `compressed/.<name>.part.<ext>` temp files from a run killed from outside (logout, crash).

**Size estimate:** after probing, each row shows a rough expected output, e.g. `61 MB → ≈ 11 MB` with a dashed `−82%` pill, for the selected speed and max size (it updates when either changes). It is `(preset size factor × min(class video bitrate, source video bitrate × codec cap) + audio bitrate) × duration`. The source video bitrate is the video stream's own (the whole file's when the container doesn't say), the audio bitrate is the copied AAC's or the class's, and the codec cap is 1.0 for HEVC/VP9/AV1 sources and 0.6 for everything else, and the result is never more than the source. If it predicts less than a 5% saving, the pill says `no saving`. That formula is −28% to +127% off for high-bitrate sources (content matters more than resolution), so when the GPU works the app **measures** instead: a sample encode of three 4 s windows (at 20/50/80% of the video; a clip of 12 s or less whole) through the same GPU filters and encoder, video only, counted from a pipe. GPU estimates then land within ±15% of the real size and x265 ones within about −18% to +28% (2026-11-09, 6 real clips). The tooltip says whether a row's estimate was measured. Limits, so big folders stay cheap: one sample at a time at low priority; none while anything is queued, encoding or being checked; biggest videos first and at most 24 per folder and settings (the rest keep the formula); each killed after 15 s; none after 3 failures in a row; changing folder or closing cancels the one in flight; changing Max size or Max 30 fps samples again. Without a working GPU there are no samples (an x265 sample would take 5–10 s per file). The estimate decides only which videos bulk Start skips; it never changes how a video is encoded.

**Speed guide:** the ⓘ next to Speed opens a card that gives, for each preset, the encode time against `medium` (with a bar) and the estimated total size against the originals in the current folder (with a bar). The current speed is highlighted. Each speed button's tooltip gives the same two numbers.

**Size expectation, stated honestly:** how much a file shrinks depends on how wastefully it was encoded in the first place.
- Typical phone or screen recordings in H.264 at 8–15 Mbps usually shrink by 70–85%, so 200 MB becomes roughly 30–60 MB.
- Files that are already efficient shrink much less. The app reports the actual result and never promises a fixed number.

## Tech Stack
- C17, GTK 4 (4.22.5) and GLib/GIO. These are already installed, so there are no new dependencies.
- `GSubprocess` runs `ffprobe` and `ffmpeg` with an **argv array, never a shell string**.
- ffmpeg/ffprobe are used from `$PATH` (`/usr/bin`), with `libx265`.
- Build: CMake ≥ 3.20, gcc.
- Tests: GLib `g_test`, run through CTest.

## Commands
Run from the repo root. The folder name contains a space, so keep the quotes.

```
Configure: cmake -S "Video Compressor" -B "Video Compressor/build" -DCMAKE_BUILD_TYPE=Release
Build:     cmake --build "Video Compressor/build" -j
Test:      ctest --test-dir "Video Compressor/build" --output-on-failure
Fixtures:  "Video Compressor/tests/make-fixtures.sh" "Video Compressor/build/fixtures"
Run:       "Video Compressor/build/video-compressor"
Debug:     cmake -S "Video Compressor" -B "Video Compressor/build-dbg" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
```

## Project Structure
```
Video Compressor/
  LOG.md                → history, decisions and measurements
  PERFORMANCE.md        → planned speed and install-size work
  SPEC.md               → this file (video-compress + shared rules)
  SPEC-*.md             → one spec per further module (app-shell, web-video, video-frames, image-compress)
  CMakeLists.txt        → app target + test target
  src/main.c            → GTK window: folder chooser, file list, Start/Cancel, progress
  src/compress.c/.h     → GTK-free logic: scan folder, parse ffprobe output,
                          pick settings from the class table, build ffmpeg argv,
                          parse ffmpeg -progress lines
  tests/test_compress.c → g_test unit tests for compress.c (no video files needed)
  tests/make-fixtures.sh→ generates 240p/480p/720p/1080p/portrait sample videos with
                          ffmpeg testsrc, for manual/integration checks (not committed)
```
The split exists so that all the decisions (which file is a video, what class it is, which arguments go to ffmpeg) can be tested without a display or real video files.

## Code Style
GLib conventions: `snake_case`, a `vc_` prefix on public functions, `g_autofree`/`g_autoptr` for cleanup, and `GError **` for anything that can fail. No global mutable state except the single app struct in `main.c`.

```c
typedef struct {
    const char *label;   /* "720p" */
    int max_short_side;  /* inclusive upper bound */
    int crf;
    int audio_kbps;
} VcClass;

static const VcClass VC_CLASSES[] = {
    { "240p",  300, 24,  64 }, { "360p",   400, 24,  96 },
    { "480p",  576, 25,  96 }, { "720p",   800, 26, 128 },
    { "1080p",1200, 27, 128 }, { "4K", G_MAXINT, 28, 160 },
};

const VcClass *
vc_class_for (int width, int height)
{
    int short_side = MIN (width, height);
    for (gsize i = 0; i < G_N_ELEMENTS (VC_CLASSES); i++)
        if (short_side <= VC_CLASSES[i].max_short_side)
            return &VC_CLASSES[i];
    g_assert_not_reached ();
}
```
- Format: GNU-ish GTK style (4-space indent, space before `(`), lines ≤ 100 chars.
- Compile with `-Wall -Wextra -Werror` in Debug builds.
- Comments explain *why*, not *what*.

## Testing Strategy
- **Unit (`tests/test_compress.c`, CTest):**
  - class table boundaries: 426×240, 640×360, 854×480, 1280×720, 1920×1080, 1080×1920 (portrait), 3840×2160
  - video extension filter: case-insensitive, rejects `.txt`/`.jpg`, ignores `compressed/` and hidden `.part` files
  - parsing of captured `ffprobe -of default=nw=1` text, including missing or garbage fields, which must return an error and not crash
  - ffmpeg argv built for a filename containing spaces, quotes and unicode, with each element checked
  - progress-line parsing (`out_time_us=` → fraction of duration)
  - output path naming (`clip.MOV` → `compressed/clip.mp4`) and the existing-output skip
  - argv for an 8-bit source (`yuv420p`, no colour tags) and a 10-bit HLG source (`yuv420p10le`, `main10`, `bt2020`/`arib-std-b67`/`bt2020nc`); `unknown` tags are left out
  - argv for each preset choice, and with Keep subtitles off (mp4, `-sn`, `hvc1`) and on (mkv, `-map 0:s?`, `mov_text` → `srt`)
  - `vc_should_trash_original()` decision: trash only when exit 0 + duration match + smaller + option on; every other combination keeps the original
- **Integration (manual, via fixtures):** generate the fixtures, run the app on the fixture folder, and confirm the success criteria below.
- **Quality check (manual):** `ffmpeg -i out.mp4 -i src.mp4 -lavfi ssim -f null -` should report SSIM ≥ 0.95 on the 720p fixture.
- Coverage: every function in `compress.c` is tested. `main.c` (UI wiring) is verified manually.

## Boundaries
- **Always:**
  - never modify an original, and remove one only through the opt-in Delete-originals path, after the output has been verified
  - encode to `compressed/.<name>.part.mp4` and rename only after ffmpeg exits 0
  - pass filenames only through argv
  - check at startup that `ffmpeg` and `ffprobe` exist and that `libx265` is available, and show a clear error dialog if not
  - keep the UI responsive (all subprocess I/O async on the GLib main loop)
  - run the tests before every commit
- **Ask first:**
  - adding any dependency beyond GTK4/GLib (already approved 2026-11-08: ImageMagick `magick` and `pngquant` as runtime
    tools for `image-compress`)
  - changing the CRF/audio table or the codec
  - recursive scanning
  - any deletion beyond the Delete-originals option (e.g. permanent delete, deleting outputs)
  - parallel encoding
- **Never:**
  - `system()`/`popen()` or shell-string commands
  - permanently delete (`unlink`/`g_file_delete`) an original; remove originals only with `g_file_trash()`
  - remove an original whose output failed verification, or one cancelled mid-encode
  - silently overwrite an existing file in `compressed/` (skip it and say so)
  - upscale
  - leave `.part` files behind after a cancel or failure
  - commit generated videos or the `build/` folder

## Success Criteria
1. Choosing the fixture folder lists exactly the video files (mixed `.mp4/.mkv/.mov/.avi/.webm`), ignores non-videos, and shows the correct class for 240p, 480p, 720p, 1080p and portrait 1080×1920.
2. **Start** encodes each file in turn into `compressed/*.mp4` (`*.mkv` with Keep subtitles). The progress bar moves smoothly and the UI stays responsive (the window can be dragged and Cancel works mid-encode).
3. On a 60 s, 8 Mbps H.264 720p fixture (≈ 60 MB), the output is ≤ 25% of the source size with SSIM ≥ 0.95.
4. On a real 200 MB+ phone recording, the output is ≤ 50 MB and looks indistinguishable at normal viewing (judged by me).
5. Cancel mid-encode kills ffmpeg within 1 s, leaves no `.part` file, and leaves the original byte-identical.
6. Filenames with spaces, quotes, `$` and unicode encode correctly.
7. Running it again on the same folder skips files already in `compressed/`.
8. If ffmpeg is missing or lacks libx265, the app shows an error dialog and does not crash.
9. With **Delete originals** on: each successfully compressed original appears in the Trash and can be restored. An original whose output failed verification, came out not smaller, or was cancelled is still in the folder, untouched. With the option off, nothing is ever removed.
10. With **Keep subtitles** on, a fixture with an SRT track and a fixture with a `mov_text` track both produce `.mkv` files that show the subtitles in mpv/VLC. With it off, the output is `.mp4` with no subtitle stream.
11. The Speed control changes the `-preset` passed to ffmpeg. The default is `slow`.
12. A 10-bit HLG fixture is encoded as 10-bit HEVC (`ffprobe` shows `yuv420p10le`, `main10`, `arib-std-b67`/`bt2020`), and a real iPhone HDR clip doesn't look washed out compared with the original (judged by me). 8-bit sources stay 8-bit.
13. `ctest` passes. A Debug build with ASan/UBSan shows no errors during a full fixture run.

## Open Questions
None. Resolved: no recursive scan · no downscale option · subtitles dropped by default with an opt-in keep · preset `slow` by default and changeable · Linux only · 10-bit/HDR sources stay 10-bit with their colour tags.
