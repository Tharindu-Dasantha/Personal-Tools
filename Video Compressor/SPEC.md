# Spec: Video Compressor

> Module `video-compress` of [CAPABILITIES.md](CAPABILITIES.md). The app is growing into **Media Tools**, with web video,
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
- I can change the **Speed** (default `slow`) to trade file size for encode time.

**Why ffmpeg and not HandBrake:** HandBrake is installed here only as the Flatpak GUI (`fr.handbrake.ghb`), with no `HandBrakeCLI`. System `/usr/bin/ffmpeg` already has `libx265`, the same encoder HandBrake uses for H.265, so the output quality is the same.

## Decisions
1. The app is Linux only (Fedora, GTK 4.22). **No Windows or macOS**, confirmed.
2. The scan is **not recursive**, confirmed: only files directly in the chosen folder. The `compressed/` subfolder is always skipped.
3. Encodes run **one at a time**. x265 already uses all 16 cores, so running several at once gains nothing and would exceed the 7 GB of RAM.
4. Resolution is **never changed**, confirmed. There is no upscaling, no downscaling, and no downscale option.
5. Subtitles are **dropped by default**. The **Keep subtitles** option keeps them (see below). The first video stream and all audio streams are always kept.
6. The x265 preset **defaults to `slow`** and can be changed in the UI.

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

**Fixed for every file:** `-c:v libx265 -c:a aac`. The frame rate is kept as it is.

**Bit depth / HDR:** the source's bit depth is kept.
- *8-bit sources* (`pix_fmt` with no `10`/`12`, e.g. `yuv420p`, `yuvj420p`) are encoded with `-pix_fmt yuv420p`.
- *10-bit or higher sources* (e.g. `yuv420p10le`, `p010le`, `yuv422p10le`; iPhone HDR is 10-bit HLG) are encoded with `-pix_fmt yuv420p10le -profile:v main10`, and the source's colour tags are copied: `-color_primaries`, `-color_trc` and `-colorspace`, with values from ffprobe. This keeps the file HDR, so it isn't washed out. Each tag is copied only if ffprobe reports it (not `unknown`).
- Limit: Dolby Vision metadata is not carried over. Those files keep their HLG/HDR10 base layer, which is what plays on most devices anyway.
- The list row shows a `10-bit HDR` or `10-bit` tag so you can see which files take this path.

**Speed (x265 preset):** a dropdown with `faster`, `fast`, `medium`, `slow` (**default**) and `slower`. The choice applies to the whole batch. Measured on this laptop (2026-11-08, a noisy 16 Mbps 1080p clip and the 720p fixture), against `medium`: `faster` 0.69× the time, `fast` 0.85×, `slow` 2.1×, `slower` 8.2×. At the same CRF the sizes stayed within about 8% of `medium` (slower presets came out slightly *larger*, because they keep more detail), so the speed mostly buys quality, not a smaller file. The earlier claim ("`slow` is 5–10% smaller") did not hold on these clips. Each launch starts at `slow`.

**Container / subtitles:**
- *Keep subtitles off (default):* output is `.mp4` with `-sn -tag:v hvc1 -movflags +faststart`. `hvc1` lets the file play in QuickTime and on iOS.
- *Keep subtitles on:* output is `.mkv` with `-map 0:s? -c:s copy`. MP4-style `mov_text` subtitles can't be copied into MKV, so if a file has them the app uses `-c:s srt` instead (text is preserved). Bitmap subtitles (PGS, DVD) are copied as they are. A file with no subtitles still produces `.mkv` while the option is on, so the output type doesn't change from file to file within a batch.

**"Not smaller" rule:** if the encoded file is not at least 5% smaller than the original, the app deletes it and marks the row `Skipped – already efficient`. This happens with files that are already HEVC/AV1 or very low bitrate.

**Delete originals (optional):** a checkbox that is off by default. When it is on, **Start** first asks for confirmation ("N originals will be moved to Trash after compressing"). An original is removed only when **all** of these are true:
1. ffmpeg exited 0 and the `.part` file was renamed into place
2. `ffprobe` on the output succeeds, and its duration is within 1 s of the source's
3. the output passed the "not smaller" rule (if it didn't, the original is kept anyway)

Removal uses `g_file_trash()`, which moves the file to the Trash so it can be recovered. If trashing fails (for example, on a filesystem with no Trash), the original is **kept** and the row shows `Compressed – original kept (trash failed)`. The app never permanently deletes a file. Rows say `Done – original trashed` so it is clear what happened.

**Controls (added 2026-11-08):**
- The header has icon buttons, each with a tooltip saying what it does: **Start** (▶), **Pause** (⏸), **Stop** (⏹) and **Move originals to Trash** (🗑). Icon-only buttons also get an accessible label.
- The list allows multi-select (Ctrl+click, Shift+click; Esc clears). Each button acts on the **selected** videos, or on **all** videos when none are selected. A button is greyed out when none of those videos can take that action.
- **Start** queues videos that are new, stopped, failed or held, and resumes a paused encode. Queued videos are encoded **one at a time**, in list order.
- **Pause** on the running encode freezes ffmpeg (`SIGSTOP`) and nothing else starts until it is resumed (`SIGCONT`) or stopped, so there is still only one ffmpeg alive. Pause on a queued video holds it: it is skipped until Start is pressed for it.
- **Stop** on the running encode kills ffmpeg and deletes the `.part` file (same as the old Cancel); the queue then continues with videos that weren't stopped. Stop on queued or held videos takes them off the queue. Stop during the output check keeps the original.
- **Move originals to Trash** works on finished videos that have a kept output, including an output from an earlier run (`Skipped – compressed/… exists`). It asks for confirmation, then applies the same three checks as the checkbox above before calling `g_file_trash()`.
- A video that came out `already efficient` is finished: it isn't encoded again in the same session.
- Speed, Keep subtitles, the Trash checkbox and Choose Folder are locked while anything is queued, encoding or being checked.

**Size estimate:** after probing, each row shows a rough expected output, e.g. `→ ≈ 11 MB (−82%)`, for the selected speed (it updates when the speed changes). It is `(class video bitrate × preset size factor + class audio bitrate) × duration`. The video bitrate is capped at 60% of the source's bitrate, and the result is never more than the source. If it predicts less than a 5% saving, the row says `≈ no saving`. The class bitrates are guesses until they are calibrated on real phone clips; the estimate never affects what is encoded.

**Speed guide:** an ⓘ next to the Speed dropdown (and the dropdown itself) has a tooltip table that gives, for each preset, the encode time against `medium` and the estimated total size against the originals in the current folder, both in percent.

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
  CAPABILITIES.md       → module map for Media Tools (index of the SPEC-*.md files)
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
  tasks/                → plan.md, todo.md
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
11. The Speed dropdown changes the `-preset` passed to ffmpeg. The default is `slow`.
12. A 10-bit HLG fixture is encoded as 10-bit HEVC (`ffprobe` shows `yuv420p10le`, `main10`, `arib-std-b67`/`bt2020`), and a real iPhone HDR clip doesn't look washed out compared with the original (judged by me). 8-bit sources stay 8-bit.
13. `ctest` passes. A Debug build with ASan/UBSan shows no errors during a full fixture run.

## Open Questions
None. Resolved: no recursive scan · no downscale option · subtitles dropped by default with an opt-in keep · preset `slow` by default and changeable · Linux only · 10-bit/HDR sources stay 10-bit with their colour tags.
