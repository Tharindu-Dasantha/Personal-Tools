# Todo: Video Compressor

Plan: [plan.md](plan.md) · Spec: [../SPEC.md](../SPEC.md)

Shared commands (run from the repo root):
- Build: `cmake -S "Video Compressor" -B "Video Compressor/build" -DCMAKE_BUILD_TYPE=Debug && cmake --build "Video Compressor/build" -j`
- Test: `ctest --test-dir "Video Compressor/build" --output-on-failure`
- Fixtures: `"Video Compressor/tests/make-fixtures.sh" "Video Compressor/build/fixtures"`
- Run: `"Video Compressor/build/video-compressor"`

---

## Phase 1: Foundation

### - [x] T1: Skeleton, startup check, fixture script
**Description:** Build files that produce `video-compressor` and `test_compress`. The app opens an empty GTK4 window titled "Video Compressor". At startup it confirms that `ffmpeg` and `ffprobe` are on PATH and that `ffmpeg -encoders` lists `libx265`. If not, it shows an error dialog and does not crash. The fixture script generates sample videos with `testsrc` + a sine tone:
- 240p, 480p and 720p (60 s at 8 Mbps)
- 1080p, and portrait 1080×1920
- a name with spaces/quotes/unicode
- a 10-bit HLG clip (`-vf setparams=color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc,format=yuv420p10le`). Use `setparams`, not the `-color_*` output flags: ffmpeg 8 takes frame tags over those flags, so a `testsrc` clip made with the flags comes out `unknown`. Checked 2026-11-08.
- a `.txt` decoy

**Acceptance criteria:**
- [x] Debug build is clean with `-Wall -Wextra -Werror`, and the app window opens
- [x] `PATH=/nonexistent ./video-compressor` shows the error dialog and exits cleanly
- [x] The fixture script creates all the files listed above in the given directory

**Verification:**
- [x] Build succeeds, and `ctest` runs (one placeholder test)
- [x] Manual: launch normally, then with a broken PATH

**Dependencies:** None
**Files:** `CMakeLists.txt`, `src/main.c`, `src/compress.c`, `src/compress.h`, `tests/test_compress.c`, `tests/make-fixtures.sh` (the file count is high, but most are stubs)
**Scope:** M

### Checkpoint 1
- [x] Build clean, ctest green, window opens, missing-ffmpeg dialog works

---

## Phase 2: See your videos

### - [x] T2: Choose a folder → list the videos
**Description:** A **Choose Folder** button opens `GtkFileDialog` (folder mode). `vc_is_video_name()` filters entries: it is case-insensitive over mp4, mkv, mov, avi, wmv, flv, webm, m4v, mpg, mpeg, ts, m2ts and 3gp, and skips hidden files and the `compressed/` directory. Each match becomes a row in a `GtkListBox` showing the filename and a human-readable size.

**Acceptance criteria:**
- [x] The fixture folder shows only the video files, and the `.txt` and `compressed/` are absent
- [x] Choosing another folder replaces the list

**Verification:**
- [x] Unit: `vc_is_video_name` accepts `.MP4`/`.mkv` and rejects `.txt`, `.jpg`, `.name.part.mp4` and `compressed`
- [x] Manual: pick the fixture folder

**Dependencies:** T1
**Files:** `src/main.c`, `src/compress.c/.h`, `tests/test_compress.c`
**Scope:** S

### - [x] T3: ffprobe → class, codec, duration in each row
**Description:** For each listed file, run `ffprobe -v error -select_streams v:0 -show_entries stream=width,height,codec_name,pix_fmt,color_primaries,color_transfer,color_space:format=duration,bit_rate -of default=nw=1` asynchronously. `vc_parse_probe()` turns that text into a `VcProbe` struct, returning a `GError` on missing or garbage fields. `vc_class_for()` (the SPEC table, based on the short side) chooses the class. The row shows, for example, `720p · h264 · 1:00 · 60 MB`. Files that can't be probed show `Unreadable` and are excluded from encoding. `vc_probe_is_10bit()` checks whether `pix_fmt` contains `10` or `12`. Those rows get a `10-bit` tag, or `10-bit HDR` when `color_transfer` is `arib-std-b67` or `smpte2084`.

**Acceptance criteria:**
- [x] All fixtures show the correct class, and portrait 1080×1920 shows `1080p`
- [x] A corrupt or renamed non-video file shows `Unreadable` and does not crash
- [x] The UI stays responsive while probing (async)

**Verification:**
- [x] Unit: class boundaries for 426×240, 640×360, 854×480, 1280×720, 1920×1080, 1080×1920 and 3840×2160
- [x] Unit: `vc_parse_probe` on captured good output, output with missing duration, and garbage
- [x] Manual: fixture folder plus a `fake.mp4` that is actually text

**Dependencies:** T2
**Files:** `src/main.c`, `src/compress.c/.h`, `tests/test_compress.c`
**Scope:** M

### Checkpoint 2
- [x] Success Criterion 1 passes on fixtures. ctest green.

---

## Phase 3: Compress (MVP)

### - [x] T4: Encode queue → `.part` → rename → result
**Description:**
- `vc_output_path()` maps `dir/clip.MOV` to `dir/compressed/clip.mp4` and the temp file `dir/compressed/.clip.part.mp4`.
- `vc_build_ffmpeg_argv()` builds the full argv from the probe result and class: `-map 0:v:0 -map 0:a?`, x265 at the class CRF, `-preset slow`, bit depth (8-bit gives `yuv420p`; 10-bit gives `yuv420p10le -profile:v main10` plus the known colour tags), `-sn`, `hvc1`, AAC at the class bitrate, and `+faststart`.
- **Start** creates `compressed/` and runs the files one at a time with `GSubprocess`.
- On exit 0 the temp file is renamed and the row shows `new size · −78%`. On failure the `.part` is deleted and the row shows `Failed` (with the last stderr line as a tooltip). Either way the queue moves on.

**Acceptance criteria:**
- [x] Start on the fixtures produces `compressed/*.mp4` for every video, and all of them play
- [x] The filename with spaces, quotes and unicode encodes correctly
- [x] Originals are byte-identical afterwards (`sha256sum` before and after)
- [x] The 10-bit HLG fixture's output shows `yuv420p10le`, `Main 10` and `arib-std-b67` in ffprobe. 8-bit fixtures stay `yuv420p`.

**Verification:**
- [x] Unit: 8-bit vs 10-bit HLG argv (pix_fmt, profile, colour tags, `unknown` tags omitted)
- [x] Unit: argv elements for a name containing a space, `'`, `$` and unicode; CRF and audio bitrate per class; output/temp path naming
- [x] Manual: full fixture run, then `ffprobe` an output (hevc, hvc1)

**Dependencies:** T3
**Files:** `src/main.c`, `src/compress.c/.h`, `tests/test_compress.c`
**Scope:** M

### - [x] T5: Live progress
**Description:** Add `-progress pipe:1 -nostats` and read stdout line by line asynchronously. `vc_parse_progress_line()` turns `out_time_us=` into a fraction of the probed duration. The row's `GtkProgressBar` updates, and a header label shows `3 / 12`. If the duration is unknown, the bar pulses.

**Acceptance criteria:**
- [x] The progress bar advances smoothly to 100% on each fixture
- [x] The window stays draggable and responsive during an encode (drag itself untestable on Xvfb with no WM; the list scrolls and repaints and Cancel responds mid-encode)

**Verification:**
- [x] Unit: `out_time_us=30000000` with a 60 s duration gives 0.5. Junk lines and `progress=end` are handled.
- [x] Manual: watch the 1080p fixture encode

**Dependencies:** T4
**Files:** `src/main.c`, `src/compress.c/.h`, `tests/test_compress.c`
**Scope:** S

### - [x] T6: Cancel
**Description:** A **Cancel** button (enabled while encoding) calls `g_subprocess_force_exit()`. In the `wait_async` callback the `.part` file is deleted and the queue stops. The current row shows `Cancelled` and the remaining rows stay `Pending`. **Start** can be pressed again.

**Acceptance criteria:**
- [x] Cancel mid-encode stops ffmpeg within 1 s (`pgrep ffmpeg` is empty)
- [x] No `.part` file remains, and the original is byte-identical

**Verification:**
- [x] Manual: cancel during the 1080p fixture, then check `ls -a compressed/` and `sha256sum`

**Dependencies:** T4
**Files:** `src/main.c`
**Scope:** S

### - [x] T7: Skip existing outputs and the "not smaller" rule
**Description:** Before encoding, if `compressed/<name>.mp4` exists, the row shows `Skipped – already done`. After encoding, if the output is not at least 5% smaller than the source, it is deleted and the row shows `Skipped – already efficient`. The decision lives in `vc_output_is_worth_keeping(src_bytes, out_bytes)`.

**Acceptance criteria:**
- [x] A second run on the fixture folder skips every file without encoding
- [x] An already-tiny low-bitrate fixture produces no output and shows `already efficient`

**Verification:**
- [x] Unit: `vc_output_is_worth_keeping` at 94%, 95%, 96% and 120% of the source size
- [x] Manual: run twice, plus a low-bitrate fixture (add it to `make-fixtures.sh`)

**Dependencies:** T4
**Files:** `src/main.c`, `src/compress.c/.h`, `tests/test_compress.c`, `tests/make-fixtures.sh`
**Scope:** M

### - [x] T8: Speed dropdown + Keep subtitles option
**Description:**
- A `GtkDropDown` labelled "Speed" offers `faster`, `fast`, `medium`, `slow` (selected by default) and `slower`. The choice is passed into `vc_build_ffmpeg_argv()` as the `-preset`.
- A `GtkCheckButton` "Keep subtitles" (off by default) switches the output to `.mkv` with `-map 0:s? -c:s copy` and no `hvc1`/`faststart`. If the file has a `mov_text` track, it uses `-c:s srt`.
- The ffprobe call from T3 also lists subtitle stream codecs (`stream=codec_type,codec_name`), and `vc_parse_probe()` records them.
- `vc_output_path()` takes the extension, so skip-existing (T7) checks the right file.
- Both controls are disabled while a batch is running.
- `make-fixtures.sh` adds a clip with an SRT subtitle track (`.mkv`) and a clip with a `mov_text` subtitle track (`.mp4`).

**Acceptance criteria:**
- [x] With the default settings, `ffprobe` on an output shows hevc in `.mp4` with no subtitle stream. `ps`/the log shows `-preset slow`.
- [x] With Keep subtitles on, both subtitle fixtures produce `.mkv` files whose subtitles show in mpv (checked in VLC; mpv isn't installed. The `subs-srt` source track has `default=0`, which is copied as-is, so that track must be picked in the player)
- [x] Choosing `faster` passes `-preset faster`, and the encode is visibly quicker

**Verification:**
- [x] Unit: argv for each preset; subs off (mp4, `-sn`, `hvc1`); subs on (mkv, `-map 0:s?`, `-c:s copy`); `mov_text` gives `-c:s srt`; probe parse picks up subtitle codecs
- [x] Manual: encode both subtitle fixtures with the option on and with it off

**Dependencies:** T4 (T7 for the skip-existing extension check)
**Files:** `src/main.c`, `src/compress.c/.h`, `tests/test_compress.c`, `tests/make-fixtures.sh`
**Scope:** M

### Checkpoint 3 (MVP)
- [x] Success Criteria 2, 3, 5, 6, 7, 10, 11 and 12 pass on fixtures
- [x] ctest green, Debug build clean
- [x] **Review with you before starting Phase 4** (user said to go ahead with Phase 4, 2026-11-08)

---

## Phase 4: Delete originals

### - [x] T9: Output verification + trash decision (logic only)
**Description:** After a successful rename, ffprobe the output. `vc_should_trash_original(opt_on, exit_ok, src_dur, out_dur, kept)` returns TRUE only when the option is on, ffmpeg exited 0, the durations are within 1 s, and the output was kept by the T7 rule. This is pure logic. Nothing is trashed yet.

**Acceptance criteria:**
- [x] Every combination except all-true returns FALSE
- [x] The output probe runs asynchronously, and a probe failure counts as "do not trash"

**Verification:**
- [x] Unit: a truth table covering each condition failing alone, duration difference 0.9 s vs 1.1 s, and the option off
- [x] Build + ctest green

**Dependencies:** T7
**Files:** `src/compress.c/.h`, `src/main.c`, `tests/test_compress.c`
**Scope:** S

### - [x] T10: Delete-originals checkbox → confirm → Trash
**Description:**
- Add a `GtkCheckButton` labelled "Move originals to Trash after compressing", off by default.
- When it is on, **Start** shows a `GtkAlertDialog`: "N originals will be moved to Trash after compressing. Continue?"
- For each file where `vc_should_trash_original()` returns TRUE, call `g_file_trash()` asynchronously. The row shows `Done – original trashed`.
- If trashing fails, the row shows `Compressed – original kept (trash failed)`.
- There is never a permanent delete.

**Acceptance criteria:**
- [x] With the option on, the trashed originals appear in the Files Trash and can be restored
- [x] Cancelled, failed and "already efficient" files keep their originals
- [x] With the option off, nothing is removed (fixture checksums unchanged)

**Verification:**
- [x] Manual: run on a copy of the fixtures with the option on, then check `gio list trash://` and restore one file (all 9 restored, checksums match)
- [x] Manual: option on with a cancel mid-file, and check that the original is still present
- [x] `grep -nE 'unlink|g_file_delete|remove\(' src/` shows only `.part` and rejected-output cleanup

**Dependencies:** T9
**Files:** `src/main.c`
**Scope:** S

### Checkpoint 4
- [x] Success Criterion 9 passes (GUI on Xvfb, 2026-11-08)
- [x] Review with you (Phase 4 committed as `e0a1656`; user said to go ahead with Phase 5)

---

## Phase 5: Harden

### - [ ] T11: Sanitizers, real-world check, calibration
**Description:**
- Full fixture run on an ASan/UBSan Debug build.
- Run a real 200 MB+ phone video at `slow` and `faster`, and record size, encode fps and SSIM for each.
- Check the SSIM of the 720p fixture.
- If the sizes or quality miss the targets, propose CRF table changes to you (this is an ask-first item) rather than changing them silently.

**Acceptance criteria:**
- [x] ASan/UBSan reports nothing across a full run that includes cancel (2026-11-08, after the Phase 6 changes: the full fixture run with subs and auto-trash, stop, pause, the Trash button, a stop during the output check, and closing while paused. All sessions exited 0 with no reports. An earlier UBSan-trap run found and fixed a SIGSEGV on close mid-encode)
- [x] The 720p fixture output is ≤ 25% of its source with SSIM ≥ 0.95 (Criterion 3): 2.7%, SSIM 0.9992
- [ ] The real 200 MB+ video comes out ≤ 50 MB and looks indistinguishable to you (Criterion 4) (needs a phone clip from you; none on this machine)

**Verification:**
- [x] `ffmpeg -i out.mp4 -i src.mp4 -lavfi ssim -f null -` (fixtures)
- [x] Results table (file, class, before, after, %, SSIM, fps) appended to `plan.md` (the real-clip row is still empty)

**Dependencies:** T10
**Files:** `tasks/plan.md`, possibly `src/compress.c` (table, only with your approval)
**Scope:** S

### Checkpoint 5 (Complete)
- [ ] All 13 success criteria in SPEC.md met. **Open:** 4 (real phone clip) and 12's iPhone HDR half (your eyes). 13 now passes
- [ ] Ready for your review

---

## Phase 6: Controls and estimates (added 2026-11-08 at your request)

### - [x] T12: Size estimate per row
**Description:** `vc_estimate_size()` gives `(class video kbps × preset size factor + audio) × duration`, with the video capped at 60% of the source bitrate and the total never above the source. The row shows `→ ≈ 11 MB (−82%)` or `≈ no saving`, and it updates when the speed changes.
- [x] Unit: medium/slow values, the lean-source cap, never above the source, unknown duration gives 0, portrait
- [x] Manual: the estimates appear after probing and change when the speed is switched to `faster`. `lowbitrate.mp4` predicted `≈ no saving` and came out already efficient

### - [x] T13: Speed guide
**Description:** an ⓘ (`help-about-symbolic`) next to Speed, and the dropdown, have a tooltip table: time against medium and size against the folder's originals, in percent, per preset. The factors (`vc_preset_factors()`) were measured; see plan.md, "Preset factors".
- [x] Unit: medium is 1/1, an unknown preset gives 1/1, time rises with each slower preset
- [x] Manual: the tooltip shows the 5-row table on hover

### - [x] T14: Icon buttons: Start / Pause / Stop / Move originals to Trash
**Description:** symbolic icons with tooltips and accessible labels. Pause = `SIGSTOP` on the running ffmpeg (nothing else starts while it is paused), or holds queued videos. Stop = kill + delete the `.part`, or take videos off the queue. The Trash button verifies and trashes finished originals after confirmation.
- [x] Pause: ffmpeg in state `T`, `.part` size frozen; Start resumes it (`S`)
- [x] Stop on one item: shows Stopped, its `.part` is removed, the next queued item starts
- [x] A held item is not started when the running one finishes
- [x] Trash button: confirm says "Move 2 originals to Trash?", exactly those 2 go to the Trash, a selected unfinished row is untouched, and both restore byte-identical
- [x] Trash on a `Skipped – … exists` row, then Stop during the output check: `– original kept`, the original is not trashed
- [x] Close while paused: exit 0, no ffmpeg, no `.part`

### - [x] T15: Multi-select
**Description:** `GTK_SELECTION_MULTIPLE` with single-click activation off (with it on, a plain click *adds* to the selection). The buttons act on the selection, or on all videos when none is selected. Esc clears the selection.
- [x] Click, Ctrl+click: only the selected two are queued. A plain click then selects just one row
- [x] Esc, then Start / Pause with no selection apply to every eligible row

### Checkpoint 6
- [x] ctest green (Debug and ASan builds), Debug build clean
- [ ] Review with you
