# Handoff: Video Compressor

Last updated 2026-11-08 (Phase 6). Read [SPEC.md](SPEC.md) (source of truth), then [tasks/plan.md](tasks/plan.md)
and [tasks/todo.md](tasks/todo.md). Checkboxes in `todo.md` are ticked only for things actually verified.

## Where things stand

| Phase | Tasks | State |
|---|---|---|
| 1. Foundation | T1 | **Done, all checks verified** (incl. error dialogs, screenshotted on Xvfb) |
| 2. See your videos | T2, T3 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08) |
| 3. Compress (MVP) | T4–T8 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08) |
| 4. Delete originals | T9, T10 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08). Committed `e0a1656` |
| 5. Harden | T11 | **Done except Criterion 4 and 12's iPhone half (the user's eyes).** ASan/UBSan clean. Close-mid-encode crash fixed. Uncommitted |
| 6. Controls + estimates | T12–T15 | **Done, verified on Xvfb with ASan.** Icon buttons, pause, per-item/multi-select actions, size estimate, speed guide. Uncommitted; awaiting review |
| 6b. v2 UI (Figma) | – | **Built 2026-11-08, screenshotted on Xvfb** (empty, ready, guide, selection, running, paused, finished, dark). Not yet re-run under ASan. Uncommitted; awaiting review |

Pushed to `origin/main` (Phase 2 checks: `026de4e`).

**New work (specced 2026-11-08):** the app grows into **Media Tools**, with four modules mapped in
[CAPABILITIES.md](CAPABILITIES.md): `app-shell` → `web-video` → `video-frames` → `image-compress`, each with a `SPEC-<id>.md`.
State: the specs are written and **awaiting the user's approval**. After that, write the plan and tasks (extend
`tasks/plan.md`/`todo.md` with Phase 7+). Commit Phases 5–6 before `app-shell` starts refactoring `main.c`.
`image-compress` needs `sudo dnf install pngquant` (approved).

**Next step:** review Phase 6 with the user, then commit (ask for the date). Still only the user can do:
1. Supply a real 200 MB+ phone clip and judge it at `slow` and `faster` (Criterion 4). Fill the empty row in `plan.md`, and use it
   to recalibrate `EST_VIDEO_KBPS` (the size estimate) in `compress.c`.
2. Judge a real iPhone HDR clip for washed-out colour (Criterion 12).

T11 results and the measured preset factors are in `tasks/plan.md`.

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
    and the speed guide. They never change what is encoded. The factors were measured (plan.md, "Preset factors"); the kbps values are guesses.
- `src/main.c`: UI plus async process wiring. There is one global `App app` (the SPEC allows this).
  - Probing runs one ffprobe at a time (`probe_step` → `probe_done`). When a new folder is chosen,
    `app.probe_cancel` is cancelled. A cancelled callback returns **before** touching `app.items`.
  - **Per-item state** (`State` enum): NEW → QUEUED → ACTIVE → DONE/TRASHED, plus HELD (paused while queued) and CHECKING
    (verify, then trash). `app.job` is the one encoding item; `app.paused` means it is SIGSTOPped. `encode_next()` starts
    the QUEUED item with the highest `prio` ("Compress next"), else the first in list order, and does nothing while `app.job` is set
    (even when paused), so there is only ever one ffmpeg.
    Actions are `act_start/pause/stop(Item *one, gboolean sel)`: one row, the checked rows (`sel`), or all, filtered by
    `can_start/pause/stop/trash()`. DONE with `out == NULL` means already efficient: finished, never re-encoded.
  - **UI (v2 design, 2026-11-08):** `render_row(it)` draws a row purely from its state (+ `note`/`tip`/`extra`); call it after any
    state change (`set_status()` does). `update_controls()` picks the page (empty/list), the bottom bar (ready/running/finished),
    the selection bar and the summary. `update_progress()` drives the running bar and ETA. Styling is the `CSS[]` string,
    using GTK Default theme colours so dark mode works. `in_batch` marks what the running bar and summary count.
  - Encoding: `encode_next()` → `start_job()`. Each job waits for three events (stdout EOF,
    stderr EOF, process exit), counted in `app.pending`. `finish_job()` runs only when all three
    are done, so the `.part` file is deleted only after ffmpeg has closed it.
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

## Verified so far (2026-11-08)

Phase 6, on Xvfb, `build/asan` (ASan + UBSan), run copies under `build/asan/run*`: all T14/T15 checks in `todo.md`, plus
the full run at `faster` with subtitles and auto-trash (9 trashed and restored byte-identical, subtitle outputs `subrip`). Every
session exited 0 with no sanitizer report. Use `ASAN_OPTIONS=detect_leaks=0` (GTK noise).

Phase 5, on Xvfb, `build/san` (see Build): the full fixture run at `faster` with subtitles and trash on, a cancel,
a restart, all 9 originals restored byte-identical, a second Start and a close gave exit 0 with nothing on stderr.
Closing mid-encode **was** a SIGSEGV (exit 139); now it exits 0 with no `.part`. Closing during the output check
(slow `ffprobe` wrapper) exits 0, the original is untouched and not trashed, and the output stays. The plain Debug
build also exits 0 on close mid-encode. Always check the app's exit code: wrap it as
`/bin/sh -c '"$0"; echo $? > rc' "$APP"` inside `dbus-run-session`.

Phase 2 GUI, on Xvfb: the fixture folder lists exactly the 11 videos (`notes.txt` and `compressed/` are absent).
The classes match ffprobe dimensions, portrait shows `1080p`, `fake.mp4` shows `Unreadable` and HLG shows
`10-bit HDR`. Choosing another folder replaces the list, including mid-probe (no stale rows). While
probes run, rows show `Reading…`, Start is disabled and the Speed dropdown still opens.

Phase 4 GUI, on Xvfb, on copies of the fixtures:
- The option is on and the dialog says "Up to 10 originals…". Its Cancel starts nothing. Confirming at `faster`
  takes 60 s: 9 rows show `Done – original trashed`, `lowbitrate.mp4` shows `already efficient` and stays,
  and `fake.mp4` stays. `gio list trash://` lists all 9. `gio trash --restore` brings them back, with
  matching checksums.
- Cancel during 1080p: the original and the pending file are still there, and the Trash is empty.
- Read-only folder: `Compressed – original kept (trash failed)`.
- An ffprobe wrapper that fails on `compressed/` paths gives `177.3 kB · −98% – original kept`.
- With the option off there is no dialog, the checksums are unchanged and the Trash is empty.

Phase 3 GUI, on Xvfb (default `slow`): the full fixture run took 112 s. All 9 outputs decode without
errors, `ps` showed `-preset slow`, and the bars advance with the header showing `n / 10`. Cancel kills
ffmpeg in 66 ms and leaves no `.part`. Start works again after a cancel, and closing the window
mid-encode kills ffmpeg and removes the `.part`. A second Start skips everything. Keep subtitles plus
`faster` gives `.mkv` files with `subrip` (`mov_text` becomes `-c:s srt`), and both show in VLC.

Phase 3, headless (earlier):

The app's exact argv was run on all fixtures through a throwaway driver linked against `vc_core`:

- All outputs are produced. 8-bit outputs are `hevc`/`hvc1`/`yuv420p`; the HLG output is `Main 10`,
  `yuv420p10le`, `arib-std-b67`/`bt2020`/`bt2020nc`.
- 720p: 60.9 MB → 1.64 MB, SSIM 0.9992. `testsrc` compresses far better than real footage, so
  Criterion 4 (a real 200 MB phone clip) is still the real test.
- `lowbitrate.mp4` came out at 183% of its source, so it was rejected. `fake.mp4` shows as Unreadable.
  A re-run skips everything. Original checksums are unchanged.
- With Keep subtitles on, both subtitle fixtures give `.mkv` files with a `subrip` track (`mov_text`
  is converted to `srt`). With it off, outputs have no subtitle stream.
- On 1080p, `faster` takes 10.3 s and `slow` takes 26.9 s.

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
- Header coordinates (800 px window, Phase 6): Start (519,26), Pause (554,26), Stop (589,26), Trash button (631,26),
  close (774,26). Speed dropdown (257,75), whose first item is `faster` at (258,125); ⓘ (309,75); Keep subtitles (337,75);
  Move to Trash checkbox (503,75). The start confirm dialog has Cancel at (163,130) and "Compress and Move to Trash" at (491,130);
  the Trash-button dialog has Cancel at (133,103) and "Move to Trash" at (398,103). Rows: first at y=130, then every 63 px.
- Tooltips: any key press hides them, so hover with `xkey X Y move` (move away first, then onto the widget).
- Multi-select: `xkey X Y ctrlclick`. Esc (with the pointer over the window) clears the selection.
- Trash tests use the real `~/.local/share/Trash`, which needs the test folder on the same filesystem
  as home (`build/` is). Restore everything afterwards with `gio trash --restore`.
- Don't wait on `pgrep -f <path>` in a loop: it matches the loop's own shell. Use `pgrep -x ffmpeg` (or `pgrep -x sleep` for the slow-ffprobe wrapper).
- VLC on Xvfb: `cvlc --no-audio --avcodec-hw=none --vout=xcb_x11 --start-time=5.5 f.mkv` (VA-API
  renders solid green). Add `--sub-track=0` for a track without the default flag.
- To see the async probe state, put a wrapper `ffprobe` that does `sleep 3; exec /usr/bin/ffprobe "$@"`
  first on PATH.
- `pkill -f video-compressor` kills your own shell (its command line matches). Close the window instead.
- Stop check (T6, was Cancel): during the 1080p encode, press Stop (589,26), run `pgrep ffmpeg` (should be empty within 1 s),
  `ls -A compressed/` (no `.part`) and `sha256sum` the original.

## Open issues and limits

- An empty folder (or one with no videos) shows a blank list, not the "No videos" placeholder.
  `gtk_list_box_remove_all()` in `load_folder` appears to drop the placeholder. Minor; not fixed.
- `subs-srt.mkv` has `default=0` on its subtitle track, and the output copies that, so players don't
  show it until it's picked. That matches the source. Forcing `-disposition:s:0 default` is an option
  to raise at review.
- The row text is `Skipped – compressed/<name> exists`, where the plan said `Skipped – already done`.
- The confirm dialog says "**Up to** N originals…" (the SPEC has no "Up to"). N counts readable files
  with no output yet, but "already efficient" or failed ones won't be trashed, so N is a maximum.
- `Done – original trashed` replaces the size/percent text in the row (as the SPEC wording says). The new
  size is no longer visible there. Could be `20 MB · −78% · original trashed` if wanted.
- If the output check fails, the row shows `<size> · −N% – original kept`, with the reason in the tooltip.
  The SPEC doesn't name this text.

- The size estimate overshoots badly on `testsrc` fixtures (240p: ≈ 3.1 MB vs 770 kB actual), because synthetic video is unusually
  easy to compress. It is meant for real footage and is uncalibrated until a phone clip is measured.
- The "N left" counter doesn't include held (paused-while-queued) items.
- Name collision: `clip.mov` and `clip.mp4` both map to `compressed/clip.mp4`. The second is
  skipped with "compressed/clip.mp4 exists" (honest, but it isn't encoded).
- If a file's first video stream is cover art (some MP4s), the cover art gets encoded. Not handled.
- Bitrate and rotation are not read from ffprobe (nothing uses them, and the short-side class makes
  rotation irrelevant).
- `on_exit` collides with libc's `on_exit()`. The callback is named `on_ffmpeg_exit` for that reason.

## Working with this user

- Commits so far are **backdated on purpose**, with author and committer dates both at noon +0530:
  `47a9757` is 2026-03-17 and `d32fd96` is 2026-03-27. Ask the user which date to use for the next
  commit. Don't assume today's date.
- Commits go straight to `main` and are pushed to `origin`, but only when the user asks.
- Don't commit `build/`, fixtures or generated videos (`.gitignore` covers `build/`).
