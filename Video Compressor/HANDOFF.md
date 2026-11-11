# Handoff: Video Compressor

Last updated 2026-11-11 (v3 UI). Read [SPEC.md](SPEC.md) (source of truth) and the `SPEC-*.md` module specs. History,
decisions and measurements are in [LOG.md](LOG.md); planned speed and install-size work is in [PERFORMANCE.md](PERFORMANCE.md).
[TEST.md](TEST.md) lists every test the user still runs by hand, with the expected result of each.

## Where things stand

| Phase | Tasks | State |
|---|---|---|
| 1. Foundation | T1 | **Done, all checks verified** (incl. error dialogs, screenshotted on Xvfb) |
| 2. See your videos | T2, T3 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08) |
| 3. Compress (MVP) | T4–T8 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08) |
| 4. Delete originals | T9, T10 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08). Committed `e0a1656` |
| 5. Harden | T11 | **Done except Criterion 4 and 12's iPhone half (the user's eyes).** ASan/UBSan clean. Close-mid-encode crash fixed. Committed |
| 6. Controls + estimates | T12–T15 | **Done, verified on Xvfb with ASan.** Icon buttons, pause, per-item/multi-select actions, size estimate, speed guide. Committed; review with the user still open |
| 6b. v2 UI (Figma) | – | **Built 2026-11-08**, screenshotted on Xvfb. The Phase 7 ASan run (2026-11-10) went through it. Committed |
| Performance | R1–R12 | GPU encoder, size samples, x265 tuning, nice/idle I/O, mtime copy. Results in [LOG.md](LOG.md#measurements); next steps in [PERFORMANCE.md](PERFORMANCE.md). Committed |
| 7. App shell | T16–T18 | **Done, verified on Xvfb with ASan (2026-11-10)**: `modes.c`, multi-step runner, Choose Files, title `Media Tools`. Committed `7541405`. Checkpoint 7 still open (see below) |
| 8. Web video | T19, T20 | **Done, verified on Xvfb with ASan (2026-11-10)**: `web_video.c`, Mode dropdown, per-mode options, split tool check, banner. Committed `7de014b`. Checkpoint 8 needs the user (web-video 5, 6, review) |
| 9. Video → frames | T21, T22 | **Done, verified on Xvfb with ASan (2026-11-10)**: `frames.c`, frames options bar, directory parts in the runner. Committed `d785d0a`. Checkpoint 9 needs the review |
| 11. v3 UI (Figma "v3 – Media Tools") | – | **Built 2026-11-11, verified on Xvfb with ASan**: mode sidebar (accent per mode, lock while busy, missing-tool dot, Tools card), header toolbar (Choose Files/Folder, Start/Pause/Stop, Show in Files when finished), per-mode inspector, footer status, two-line row results, Twin chip, banner with copyable fix, custom close dialog, dark mode. Follow-up the same day: default speed `medium`, GPU moved from the speed buttons to a **Use GPU** switch, footer time estimate (`vc_estimate_seconds`, speed learned per machine in `~/.config/media-tools/speed.ini`), Inter font embedded (copied to `~/.cache/media-tools/fonts` on first run). Committed `b1f4378` and `84398e5` |
| 10. Compress images | T23–T27 | **T23–T26 done, verified on Xvfb with ASan (2026-11-10)**: `images.c`, image fixtures, Compress images mode, trash for images. **Not committed yet.** T27 (calibration against TinyJPG/TinyPNG) needs the user's photos and eyes |

Everything up to `caed08a` is pushed to `origin/main`.

**Media Tools (Phases 7–10):** the app grows into four modules:
`app-shell` → `web-video` → `video-frames` → `image-compress`, each with a `SPEC-<id>.md` (tasks T16–T27, see [LOG.md](LOG.md)).
All four are built. pngquant 4.0.3 and ImageMagick 7.1.2 are installed; the image fixtures also need `exiftool`.

**Next step:** the user reviews Phase 10 (and runs [TEST.md](TEST.md) 5c and 2.3 = T27) and says which commit date to use for
it. Phases 7–9 also still wait on the review and the items below.
1. Checkpoint 7 still needs: a one-by-one re-run of SPEC.md criteria 1–3 and 5–13 (the read-only-folder and failed-output-check
   cases weren't repeated), Choose Folder → Choose Files clicked through (only Files → Folder was), and a review with the user.
2. Checkpoint 8 needs the user: web-video criterion 5 (no GPS tag on a real phone clip's output) and 6 (plays in Safari on iOS
   before it has fully downloaded).

Still only the user can do (steps and expected results in [TEST.md](TEST.md), sections 1–2):
1. Supply a real 200 MB+ phone clip and judge it at `slow` and `faster` (Criterion 4). Fill the empty row of the T11 table in `LOG.md`, and use it
   to recalibrate `EST_VIDEO_KBPS` (the size estimate) in `compress.c`.
2. Judge a real iPhone HDR clip for washed-out colour (Criterion 12).
3. Web video: no GPS tag in a real phone clip's output (web-video 5), and the output plays in Safari on iOS before it has
   downloaded (web-video 6). Serve it with `npx http-server`: Python's `http.server` has no byte ranges, which Safari needs.
4. Images: compare 5 real photos and 5 real PNGs against TinyJPG/TinyPNG and judge the look (image criterion 6, T27,
   TEST.md 2.3). Changing 82 / 65-90 / 80 is ask-first.

**Ask the user:** Compress video fails on odd-sized video (see "Open issues"). Fixing it changes what Compress video encodes.

T11 results and the measured preset factors are in [LOG.md](LOG.md#measurements).

## Build, test, run (from the repo root; the folder name has a space)

```
cmake -S "Video Compressor" -B "Video Compressor/build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "Video Compressor/build" -j
ctest --test-dir "Video Compressor/build" --output-on-failure
"Video Compressor/tests/make-fixtures.sh" "Video Compressor/build/fixtures"   # ~45 s
"Video Compressor/build/video-compressor"
```

Debug builds use `-Werror`. Run the tests before every commit (SPEC rule).

Sanitizer builds go under `build/` so they stay gitignored:
```
# ASan + UBSan (needs libasan libubsan):
cmake -S "Video Compressor" -B "Video Compressor/build/asan" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
# What ran without them: UBSan in trap mode, fortify, plus glibc heap checks at runtime:
cmake -S "Video Compressor" -B "Video Compressor/build/san" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-O1 -D_FORTIFY_SOURCE=3 -fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-sanitize-recover=all -fno-omit-frame-pointer"
#   run the app with MALLOC_CHECK_=3 MALLOC_PERTURB_=165 G_DEBUG=fatal-criticals; a trap is exit 132, a heap abort 134
```

## Code map

- `src/compress.c/.h`: all decisions, no GTK. Every function has unit tests in
  `tests/test_compress.c`. Add any new logic here, with a test.
  - `vc_parse_probe()` parses `ffprobe -of default=nw=1`. That format has no stream
    delimiters, so the probe asks for `stream=index,...` and treats each `index=` line as the start
    of a new stream. Don't drop `index` from `vc_build_ffprobe_argv()`.
  - `"N/A"` and `"unknown"` are both treated as missing values.
  - `VC_CLASSES[]` (the CRF/audio table) is **ask-first** to change.
  - `EST_VIDEO_KBPS[]` (parallel to `VC_CLASSES`, static-asserted) and `PRESET_FACTORS[]` only feed the size estimate
    and the speed guide. They never change what is encoded. The factors were measured (LOG.md, "Preset factors"); the kbps values are guesses.
- `src/web_video.c` (Phase 8): `vc_web_video_output()` (`web/<stem>.mp4`, lowercase) and `vc_web_video_steps()`, one libx264 step
  per SPEC-web-video.md. The zscale matrix fallback is `bt2020nc`: zscale rejects `bt2020` as a matrix name (it's a primaries name).
- `src/frames.c` (Phase 9): `vc_parse_time()`, `vc_frames_jpeg_qscale()`, `vc_frames_estimate()`, `vc_frames_output()`
  (`frames/<stem>`, part `frames/.<stem>.part`) and `vc_frames_steps()`. The step sets `part_is_dir` (the runner empties and
  creates that folder before ffmpeg, which won't) and `duration` (the range length, so progress counts the range).
  `vc_tonemap_chain()` in `web_video.c` is shared by both modes. `VcProbe.fps` (from `avg_frame_rate`) already existed.
- Images in `main.c`: `finish_job()` applies the 5% rule to each image output on its own, then `image_result()` builds the
  row text (`it->detail`, e.g. `jpg 412 kB −58% · webp 288 kB −71%`, `png already optimized`, `jpg exists`) from what is on
  disk, and sets `it->out` to the same-type copy, else the webp (`it->webp_only`, which blocks `can_trash()`). Images have
  their own Trash switch (`app.img_trash`); `trash_on()` picks the mode's. `verify_done()` checks the same-type copy's
  width/height against `vc_image_expected_size()` at `it->max_width` (the Max width the item ran at), upright or transposed
  (ffprobe ignores EXIF orientation). `spawn_step()` now removes a leftover part before **every** step (pngquant refuses to
  overwrite). `noun()` gives "video"/"image" for the counts and dialogs.
- `src/images.c` (Phase 10): `vc_probe_has_alpha()`, `vc_image_outputs()` (same-type path + `.webp` path; BMP/TIFF → png
  with alpha, else jpg), `vc_image_steps()`, `vc_image_expected_size()`, `vc_image_should_trash()`. Source type comes from the
  extension. Parts are `compressed/.<stem>.part.<ext>`, the intermediate `compressed/.<stem>.mid.part.png` (output NULL).
  BMP/TIFF → png always goes through magick first (pngquant reads PNG only). TIFF input is `IN[0]` (first page only).
  Twins: when another accepted image in the same folder (read from disk, not the list) has the same stem, every output and
  part uses the whole source name (`photo.png.png`, `photo.png.webp`, `.photo.png.part.webp`), so twins never share one.
  The extension list itself is `IMAGE_EXTS` in `modes.c`.
- `src/modes.c/.h` (Phase 7): `VcMode`, `VcStep`, per-mode filter/out dir/trash/tool check, and `vc_mode_steps()`, which
  builds an item's steps (only Compress video has a builder so far; it also does the existing-output skip).
  `vc_remove_part()` is the only delete in `src/`. Options stay in `compress.h`'s `VcOptions`.
- `src/main.c`: UI plus async process wiring. There is one global `App app` (the SPEC allows this).
  - Probing runs one ffprobe at a time (`probe_step` → `probe_done`). When a new folder is chosen,
    `app.probe_cancel` is cancelled. A cancelled callback returns **before** touching `app.items`.
  - **Per-item state** (`State` enum): NEW → QUEUED → ACTIVE → DONE/TRASHED, plus HELD (paused while queued) and CHECKING
    (verify, then trash). `app.job` is the one encoding item; `app.paused` means it is SIGSTOPped. `encode_next()` starts
    the QUEUED item with the highest `prio` ("Compress next"), else the first in list order, and does nothing while `app.job` is set
    (even when paused), so there is only ever one ffmpeg.
    Actions are `act_start/pause/stop(Item *one, gboolean sel)`: one row, the checked rows (`sel`), or all, filtered by
    `can_start/pause/stop/trash()`. DONE with `out == NULL` means already efficient: finished, never re-encoded.
  - **UI (v3 design, 2026-11-11):** sidebar (`build_sidebar`, modes in `MODE_INFO`) | header (`build_header`) over
    content (`build_banner`, empty/list pages, `build_footer`) | inspector (`build_inspector`, one stack page per mode).
    `render_row(it)` draws a row purely from its state (+ `note`/`tip`/`extra`/`res[]`); call it after any state change
    (`set_status()` does). Its status column has two lines (`set_line`). `update_controls()` picks the page, the header
    buttons, sidebar locks/dots/counts, the banner, the footer, the selection bar and the summary; `update_progress()`
    drives the footer while busy. Styling is the `CSS[]` string: colours are CSS variables on `window.mt`, the mode's
    class (`web`, `frames`, `images`) sets `--accent`, and dark comes from `prefers-color-scheme` or the `.dark` class
    (`sync_dark`, for themes made dark via prefer-dark-theme). Icons Adwaita lacks (globe, bolt, wrench, …) are SVGs in
    `data/icons`, compiled into a GResource; GtkApplication finds them by name. `in_batch` marks what the footer and
    summary count.
  - Encoding: `encode_next()` → `vc_mode_steps()` → `start_job()` → `spawn_step()` per step. Each step waits for three
    events (stdout EOF, stderr EOF, process exit), counted in `app.pending`; `step_ended()` then starts the next step or
    calls `finish_job()`, which moves outputs into place and runs every step's part through `vc_remove_part()`.
  - List source: `load_source(folder, files)`, one folder or the Choose Files list (`app.folder` / `app.files`).
  - Stop sets `it->stop` and calls `g_subprocess_force_exit()` (SIGKILL ends a paused ffmpeg too). `finish_job()` sees `it->stop`.
    Closing the window mid-encode kills ffmpeg, waits for it and unlinks the `.part`
    synchronously (`on_close`). `on_close` also sets `app.closing`. GTK destroys the widgets, but async
    callbacks still fire in the last loop iterations, so every callback that touches a row checks
    `app.closing` first. Without it, closing mid-encode segfaulted. Any new async callback needs the same check.
  - The output is renamed with `g_file_move(..., G_FILE_COPY_NONE)`, which never overwrites.
  - Trash: `check_then_trash(it)` (from `finish_job()` when the checkbox is on, or from the Trash button) →
    `verify_done()` re-stats the output and asks `vc_should_trash_original()` → `g_file_trash_async()` → `trash_done()`.
    The `Item *` is the callback's user_data. This runs alongside the next encode. Stop during the check keeps the original.
  - `act_start()` shows the confirm dialog only when the Trash switch is on and a targeted file has no output yet;
    `do_start()` does the real start (and SIGCONTs a paused job). The bottom Start only resumes when paused.
- `tests/make-fixtures.sh` writes these fixtures:
  - `240p.avi`, `480p.webm`, `720p.mp4` (60 s, 8 Mbps), `1080p.mkv`, `portrait.mov`
  - a name with quotes, `$` and unicode
  - `hlg-10bit.mov` (ProRes 10-bit HLG)
  - `lowbitrate.mp4` (HEVC, which triggers "already efficient")
  - `subs-srt.mkv`, `subs-movtext.mp4`
  - decoys: `notes.txt` and `fake.mp4` (a text file)

## Testing the GUI (no human needed)

Wayland has no automation here, and there is no `xdotool`, AT-SPI or python-gi. What works:
Xvfb + ImageMagick `import` for screenshots + a small XTest key/click sender (`xkey.c` below,
built with `gcc xkey.c -o xkey $(pkg-config --cflags --libs x11 xtst)`).

```
export DISPLAY=:99 GDK_BACKEND=x11 GTK_A11Y=none
Xvfb :99 -screen 0 1280x800x24 -nolisten tcp &
/usr/bin/dbus-run-session -- /usr/bin/env PATH=/usr/bin:/bin GDK_DEBUG=no-portals "$APP" &   # PATH *inside* dbus-run-session
sleep 3; import -window root shot.png                                   # then view shot.png
./xkey 200 150 Escape          # move pointer (focus follows it, there is no WM) and press a key
./xkey 70 26 click             # click at x,y; Choose Folder is around (70,26)
./xkey 500 150 ctrl+l; ./xkey 500 150 type "$DIR"; ./xkey 500 150 Return   # pick a folder in the dialog
./xkey 380 400 scroll          # wheel down over the list (scrollup = wheel up); close the window with a click at (734,26)
```

```c
/* xkey <x> <y> move | click | ctrlclick | scroll | scrollup | <keysym> | ctrl+<keysym> | type <string> */
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <stdlib.h>
#include <string.h>
static void tap (Display *d, KeySym ks, int ctrl) {
    KeyCode k = XKeysymToKeycode (d, ks);
    int shift = XkbKeycodeToKeysym (d, k, 0, 0) != ks;
    KeyCode sh = XKeysymToKeycode (d, XK_Shift_L), ct = XKeysymToKeycode (d, XK_Control_L);
    if (ctrl) XTestFakeKeyEvent (d, ct, True, 0);
    if (shift) XTestFakeKeyEvent (d, sh, True, 0);
    XTestFakeKeyEvent (d, k, True, 0); XTestFakeKeyEvent (d, k, False, 0);
    if (shift) XTestFakeKeyEvent (d, sh, False, 0);
    if (ctrl) XTestFakeKeyEvent (d, ct, False, 0);
    XSync (d, False);
}
int main (int argc, char **argv) {
    Display *d = XOpenDisplay (NULL);
    if (!d || argc < 4) return 1;
    XTestFakeMotionEvent (d, -1, atoi (argv[1]), atoi (argv[2]), 0);
    if (!strcmp (argv[3], "move")) {
    } else if (!strcmp (argv[3], "ctrlclick")) {
        KeyCode ct = XKeysymToKeycode (d, XK_Control_L);
        XTestFakeKeyEvent (d, ct, True, 0);
        XTestFakeButtonEvent (d, 1, True, 0); XTestFakeButtonEvent (d, 1, False, 0);
        XTestFakeKeyEvent (d, ct, False, 0);
    } else if (!strcmp (argv[3], "click")) {
        XTestFakeButtonEvent (d, 1, True, 0); XTestFakeButtonEvent (d, 1, False, 0);
    } else if (!strcmp (argv[3], "scroll")) {
        for (int i = 0; i < 10; i++) { XTestFakeButtonEvent (d, 5, True, 0); XTestFakeButtonEvent (d, 5, False, 0); }
    } else if (!strcmp (argv[3], "scrollup")) {
        for (int i = 0; i < 10; i++) { XTestFakeButtonEvent (d, 4, True, 0); XTestFakeButtonEvent (d, 4, False, 0); }
    } else if (!strcmp (argv[3], "type") && argc > 4) {
        for (const char *c = argv[4]; *c; c++) tap (d, (KeySym) (unsigned char) *c, 0);
    } else if (!strncmp (argv[3], "ctrl+", 5)) {
        tap (d, XStringToKeysym (argv[3] + 5), 1);
    } else {
        tap (d, XStringToKeysym (argv[3]), 0);
    }
    XFlush (d); XCloseDisplay (d); return 0;
}
```

Gotchas:
- Without a WM, windows and dialogs open at (0,0) and overlap. That's expected; on GNOME they're centred.
- If PATH goes before `dbus-run-session`, it hides `dbus-daemon` and the app never starts.
- Under `GDK_BACKEND=x11` the portal has no FileChooser backend and clicking Choose Folder does nothing.
  `GDK_DEBUG=no-portals` forces GTK's own chooser; Ctrl+L, type the path, Return selects the folder.
- The chooser's location entry auto-completes to the common prefix of the folder's files (`w3` became
  `w3/subs-`), and selecting a folder name that doesn't exist **creates it**. Make sure the folder
  has files without a shared prefix.
- Choose Files across folders: GTK's own chooser can't browse two folders at once, and its fallback search finds nothing in
  `build/`. Use **Recent** instead: write a `recently-used.xbel` listing the files into a scratch dir, launch with
  `XDG_DATA_HOME=<that dir>` (inside `dbus-run-session`, next to `GDK_DEBUG`), open Choose Files, click Recent (64,22), click the
  first row, `ctrlclick` the second, Open (826,302). Don't run Trash tests in that session: Trash moves with `XDG_DATA_HOME`.
- Phase 8: the window opened 1260×720 (not maximised) and the bottom bar is the same height in both modes. Mode dropdown (80,26),
  its items Compress video (80,82) and Web video (80,117); web Max width (85,677), whose items run from Original (86,424) down in
  35 px steps (1280 is (82,564)); Remove audio (199,677); Start (1109,677), Stop (1215,677); close (1235,27). The banner pushes the
  empty page down 39 px (Choose Folder at (558,460)).
- Phase 7 coordinates (1280×800 screen, maximised window): empty page Choose Folder (558,436), Choose Files (706,436); header
  close (1235,27); bottom bar Faster (122,664), Slower (403,664), Keep subtitles (494,642), Move to Trash (494,680), Start
  (1109,664), Pause (1164,664), Stop (1215,664); start confirm "Compress and Move to Trash" (400,175); close dialog "Stop and
  Quit" (269,103); finished bar "Choose Another Folder…" (1001,685). Rows start at y=130, every 57 px; the row stop button is at x=1173.
- `faster` finishes each fixture in a few seconds, faster than one shell round trip. To catch a file mid-encode, read
  `pgrep -x ffmpeg` and click in the same command, or use `slower` on `720p.mp4`.
- If a click closes the window while a dialog is still open, the app keeps running. Check `ps` before the next launch, or two
  windows overlap at (0,0).
- Header coordinates (800 px window, Phase 6): Start (519,26), Pause (554,26), Stop (589,26), Trash button (631,26),
  close (774,26). Speed dropdown (257,75), whose first item is `faster` at (258,125); ⓘ (309,75); Keep subtitles (337,75);
  Move to Trash checkbox (503,75). The start confirm dialog has Cancel at (163,130) and "Compress and Move to Trash" at (491,130);
  the Trash-button dialog has Cancel at (133,103) and "Move to Trash" at (398,103). Rows: first at y=130, then every 63 px.
- Tooltips: any key press hides them, so hover with `xkey X Y move` (move away first, then onto the widget).
- Multi-select: `xkey X Y ctrlclick`. Esc (with the pointer over the window) clears the selection.
- Trash tests use the real `~/.local/share/Trash`, which needs the test folder on the same filesystem
  as home (`build/` is). Afterwards restore **only the test files**, never the whole Trash (the user keeps things there):
  `gio trash --list | awk -F'\t' -v d="$PWD/build/asan/p10/" 'index($2, d) == 1 {print $1}' | xargs -r -d '\n' -n1 gio trash --restore`.
- Phase 10: Mode dropdown item `Compress images` (80,187). Images bottom bar: Max width (85,677), whose items run from Original
  (86,424) in 35 px steps (1280 is (82,564)); Move to Trash (208,677); Start (1109,677), Stop (1215,677); start confirm
  "Compress and Move to Trash" (400,175). A finished row's buttons are Show in Files then Trash; a click meant for a row's
  Stop that lands after the state changed opens Nautilus (close it at (861,27)).
- A shell wrapper that `sleep`s keeps the step's pipes open after SIGKILL (its `sleep` child holds them), so Stop shows
  `Stopping…` until the sleep ends. Real magick/pngquant/ffmpeg have no such child.
- Screenshot round trips take several seconds: a wait in a wrapper must be long (30 s) to act on a state in a later command.
- Don't wait on `pgrep -f <path>` in a loop: it matches the loop's own shell. Use `pgrep -x ffmpeg` (or `pgrep -x sleep` for the slow-ffprobe wrapper).
- VLC on Xvfb: `cvlc --no-audio --avcodec-hw=none --vout=xcb_x11 --start-time=5.5 f.mkv` (VA-API
  renders solid green). Add `--sub-track=0` for a track without the default flag.
- To see the async probe state, put a wrapper `ffprobe` that does `sleep 3; exec /usr/bin/ffprobe "$@"`
  first on PATH.
- `pkill -f video-compressor` kills your own shell (its command line matches). Close the window instead.
- Stop check (T6, was Cancel): during the 1080p encode, press Stop (589,26), run `pgrep ffmpeg` (should be empty within 1 s),
  `ls -A compressed/` (no `.part`) and `sha256sum` the original.

## Open issues and limits

- **Compress video fails on a video with an odd width or height** (found 2026-11-10, not fixed). x265 rejects 853×479 in 4:2:0
  ("Picture width must be an integer multiple of the specified chroma subsampling"), so `odd-853x479.mp4` ends `Failed`, original
  untouched. The bug predates Phase 8; the new fixture is the first to expose it. The fix would be scaling to even dimensions
  (as web mode's `trunc(iw/2)*2`) when the source is odd. It changes what Compress video encodes, so ask first.
- Fixture count since Phase 8: 13 video-named files (12 readable + `fake.mp4`) plus `notes.txt`. Web video gives 12 outputs.
  Compress video gives 10 (minus `lowbitrate.mp4`, rejected, and `odd-853x479.mp4`, failed).

- An empty folder (or one with no videos) shows a blank list, not the "No videos" placeholder.
  `gtk_list_box_remove_all()` in `load_source` appears to drop the placeholder. Minor; not fixed.
- `subs-srt.mkv` has `default=0` on its subtitle track, and the output copies that, so players don't
  show it until it's picked. That matches the source. Forcing `-disposition:s:0 default` is an option
  to raise at review.
- The confirm dialog says "**Up to** N originals…" (the SPEC has no "Up to"). N counts readable files
  with no output yet, but "already efficient" or failed ones won't be trashed, so N is a maximum.

- The size estimate overshoots badly on `testsrc` fixtures (240p: ≈ 3.1 MB vs 770 kB actual), because synthetic video is unusually
  easy to compress. It is meant for real footage and is uncalibrated until a phone clip is measured.
- Since Phase 7 the tooltip on an `Already compressed` row is `compressed/<name> exists`, not the full output path.
- Images: an output dropped by the 5% rule is retried on every new run (only outputs on disk are skipped), so
  `already-tiny.png` is re-encoded and skipped again each time. Same as `lowbitrate.mp4` in Compress video.
- Images twins (`photo.jpg` + `photo.png`) get `photo.jpg.webp` / `photo.png.webp`. If a twin appears or disappears later, the
  names change and the next run makes new copies under the new names (the old ones stay). Compress video still has the
  `clip.mov`/`clip.mp4` collision: fixing it changes what that mode writes, which is ask-first.
- Images: the row meta shows a JPEG's stored size (ffprobe ignores EXIF orientation), e.g. `1600×1200` for a portrait photo.
- A copy of 320 kB in 1.4 kB showed `−100%`; `percent_smaller()` now caps at 99% for any non-empty output (all modes).
- Drag and drop still takes folders only; Choose Files is the only way to list single files.
- Name collision: `clip.mov` and `clip.mp4` both map to `compressed/clip.mp4`. The second is
  skipped with "compressed/clip.mp4 exists" (honest, but it isn't encoded).
- If a file's first video stream is cover art (some MP4s), the cover art gets encoded. Not handled.
- Bitrate and rotation are not read from ffprobe (nothing uses them, and the short-side class makes
  rotation irrelevant).
- `on_exit` collides with libc's `on_exit()`. The callback is named `on_ffmpeg_exit` for that reason.

- Frames: the summary's Before/After block compares video bytes with frame bytes (captioned `vs the videos`). `N videos ready` also counts a row that will be skipped as
  `Starts after the end`. HLG/PQ JPG frames come out `yuvj444p` (ffmpeg's pick after the float tone-map); valid JPEG, a bit bigger.
- `N files hidden` can't differ between the two video modes (same filter); it was not re-checked on a mode switch.
- Wording is mode-aware since Phase 10 (`noun()`, `verb()`, `doing()`): tooltips, row menu (`Compress`/`Convert`/`Extract`),
  selection bar, suspend-inhibit reason, background notification. The close dialog is neutral ("Keep working in the
  background?"). The summary caption is `Smaller`, `Bigger` (a web total that grew) or `vs the videos` (frames). Still fixed
  text: the list placeholder "Nothing here for this mode." (never shown, see above) and the compress-only tooltips.

## Working with this user

- Commits from 2026-11-08 on carry their real dates. Earlier commits were **backdated on purpose**, with author and committer dates both at noon +0530:
  `47a9757` is 2026-03-17 and `d32fd96` is 2026-03-27. Ask the user which date to use for the next
  commit. Don't assume today's date.
- Commits go straight to `main` and are pushed to `origin`, but only when the user asks.
- Don't commit `build/`, fixtures or generated videos (`.gitignore` covers `build/`).
