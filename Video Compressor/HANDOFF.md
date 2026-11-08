# Handoff: Video Compressor

Last updated 2026-11-08. Read [SPEC.md](SPEC.md) (source of truth), then [tasks/plan.md](tasks/plan.md)
and [tasks/todo.md](tasks/todo.md). Checkboxes in `todo.md` are ticked only for things actually verified.

## Where things stand

| Phase | Tasks | State |
|---|---|---|
| 1. Foundation | T1 | **Done, all checks verified** (incl. error dialogs, screenshotted on Xvfb) |
| 2. See your videos | T2, T3 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08) |
| 3. Compress (MVP) | T4–T8 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08) |
| 4. Delete originals | T9, T10 | **Done, all checks verified** (GUI on Xvfb, 2026-11-08). Uncommitted; awaiting Checkpoint 4 review |
| 5. Harden | T11 | **Not started** |

Pushed to `origin/main` (Phase 2 checks: `026de4e`).

**Next step:** **review with the user (Checkpoint 4) before starting Phase 5.** Phase 4 is not
committed yet; ask for the commit date. Open points are listed under "Open issues".

## Build, test, run (from the repo root; the folder name has a space)

```
cmake -S "Video Compressor" -B "Video Compressor/build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "Video Compressor/build" -j
ctest --test-dir "Video Compressor/build" --output-on-failure
"Video Compressor/tests/make-fixtures.sh" "Video Compressor/build/fixtures"   # ~45 s
"Video Compressor/build/video-compressor"
```

Debug builds use `-Werror`. Run the tests before every commit (SPEC rule).

## Code map

- `src/compress.c/.h`: all decisions, no GTK. Every function has unit tests in
  `tests/test_compress.c`. Add any new logic here, with a test.
  - `vc_parse_probe()` parses `ffprobe -of default=nw=1`. That format has no stream
    delimiters, so the probe asks for `stream=index,...` and treats each `index=` line as the start
    of a new stream. Don't drop `index` from `vc_build_ffprobe_argv()`.
  - `"N/A"` and `"unknown"` are both treated as missing values.
  - `VC_CLASSES[]` (the CRF/audio table) is **ask-first** to change.
- `src/main.c`: UI plus async process wiring. There is one global `App app` (the SPEC allows this).
  - Probing runs one ffprobe at a time (`probe_step` → `probe_done`). When a new folder is chosen,
    `app.probe_cancel` is cancelled. A cancelled callback returns **before** touching `app.items`.
  - Encoding: `encode_next()` → `start_job()`. Each job waits for three events (stdout EOF,
    stderr EOF, process exit), counted in `app.pending`. `finish_job()` runs only when all three
    are done, so the `.part` file is deleted only after ffmpeg has closed it.
  - Cancel calls `g_subprocess_force_exit()`, and `finish_job()` sees `app.cancelled`.
    Closing the window mid-encode kills ffmpeg, waits for it and unlinks the `.part`
    synchronously (`on_close`).
  - The output is renamed with `g_file_move(..., G_FILE_COPY_NONE)`, which never overwrites.
  - Trash (Phase 4): `finish_job()` → on a kept output with the option on, `verify_output()`
    probes the output → `verify_done()` asks `vc_should_trash_original()` → `g_file_trash_async()`
    → `trash_done()`. Each path ends in `next_job()`, which stops the batch if Cancel was pressed
    meanwhile. A cancel during the output check keeps the original. The widget is `app.trash`; the
    batch's snapshot of it is `app.trash_on`.
  - `on_start()` shows the confirm dialog only when the option is on and at least one file has no
    output yet; `start_batch()` does the real start.
- `tests/make-fixtures.sh` writes these fixtures:
  - `240p.avi`, `480p.webm`, `720p.mp4` (60 s, 8 Mbps), `1080p.mkv`, `portrait.mov`
  - a name with quotes, `$` and unicode
  - `hlg-10bit.mov` (ProRes 10-bit HLG)
  - `lowbitrate.mp4` (HEVC, which triggers "already efficient")
  - `subs-srt.mkv`, `subs-movtext.mp4`
  - decoys: `notes.txt` and `fake.mp4` (a text file)

## Verified so far (2026-11-08)

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
/* xkey <x> <y> click | scroll | scrollup | <keysym> | ctrl+<keysym> | type <string> */
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
    if (!strcmp (argv[3], "click")) {
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
- Header coordinates: Start (577,26), Cancel (501,26), Speed dropdown (240,75), whose first item is
  `faster` at (241,125), Keep subtitles (297,75), Move to Trash (462,75). The trash confirm dialog has
  Cancel at (163,130) and "Compress and Move to Trash" at (491,130).
- Trash tests use the real `~/.local/share/Trash`, which needs the test folder on the same filesystem
  as home (`build/` is). Restore everything afterwards with `gio trash --restore`.
- Don't wait on `pgrep -f <path>` in a loop: it matches the loop's own shell. Use `pgrep -x ffmpeg`.
- VLC on Xvfb: `cvlc --no-audio --avcodec-hw=none --vout=xcb_x11 --start-time=5.5 f.mkv` (VA-API
  renders solid green). Add `--sub-track=0` for a track without the default flag.
- To see the async probe state, put a wrapper `ffprobe` that does `sleep 3; exec /usr/bin/ffprobe "$@"`
  first on PATH.
- `pkill -f video-compressor` kills your own shell (its command line matches). Close the window instead.
- Cancel check (T6): during the 1080p encode, run `pgrep ffmpeg` (should be empty within 1 s),
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

- **ASan/UBSan can't link**: `libasan`/`libubsan` aren't installed. The user needs to run
  `sudo dnf install libasan libubsan`. Required for T11 and Criterion 13.
- Name collision: `clip.mov` and `clip.mp4` both map to `compressed/clip.mp4`. The second is
  skipped with "compressed/clip.mp4 exists" (honest, but it isn't encoded).
- If a file's first video stream is cover art (some MP4s), the cover art gets encoded. Not handled.
- Bitrate and rotation are not read from ffprobe (nothing uses them, and the short-side class makes
  rotation irrelevant).
- After a cancel, re-pressing Start re-encodes "already efficient" files, because they have no output.
- `on_exit` collides with libc's `on_exit()`. The callback is named `on_ffmpeg_exit` for that reason.

## Working with this user

- Commits so far are **backdated on purpose**, with author and committer dates both at noon +0530:
  `47a9757` is 2026-03-17 and `d32fd96` is 2026-03-27. Ask the user which date to use for the next
  commit. Don't assume today's date.
- Commits go straight to `main` and are pushed to `origin`, but only when the user asks.
- Don't commit `build/`, fixtures or generated videos (`.gitignore` covers `build/`).
