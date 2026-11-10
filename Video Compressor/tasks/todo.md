# Todo: Video Compressor

Plan: [plan.md](plan.md) · Spec: [../SPEC.md](../SPEC.md) · Performance: [todo-performance.md](todo-performance.md)

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

---

# Media Tools (Phases 7–10)

Map: [../CAPABILITIES.md](../CAPABILITIES.md). Each phase follows its module spec. Shared rules for every task: argv only,
`.part` then rename, never overwrite, one job at a time, async I/O, an `app.closing` check in every new callback, ctest green
before every commit. Manual GUI checks use Xvfb as described in [../HANDOFF.md](../HANDOFF.md).

## Phase 7: App shell ([SPEC-app-shell.md](../SPEC-app-shell.md))

### - [x] T16: `modes.c` core
**Description:** Add `src/modes.c/.h` to `vc_core` with `VcMode`, `VcStep`, `VcOptions`, `vc_mode_accepts()`,
`vc_mode_out_dir()`, `vc_mode_can_trash()`, `vc_mode_missing_tool()` (`VC_HAVE_*` bits), `vc_mode_steps()` and
`vc_step_free()`. Only the `VC_MODE_COMPRESS` branch of `vc_mode_steps()` is real: it wraps `vc_output_path()` +
`vc_build_ffmpeg_argv()` into one step, and the existing-output skip moves into it (same text:
`compressed/<name> exists`). The other modes return NULL for now. Add `vc_remove_part()`: it deletes a `.part` file, or
empties a `.part` directory of its regular files and removes it. It refuses any path whose name doesn't end in `.part`
(before the extension) and any directory that contains a subdirectory.

**Acceptance criteria:**
- [x] `test_compress` passes unchanged
- [x] `vc_mode_steps(VC_MODE_COMPRESS, …)` gives exactly what `main.c` builds today

**Verification:**
- [x] Unit (`tests/test_modes.c`): every test listed in the spec's Testing Strategy (accepts, out dir, can-trash,
      missing tool per mode, compress step equals the existing argv/part/output for 8-bit, 10-bit HLG and keep-subs)
- [x] Unit: `vc_remove_part()` on a temp file, a temp `.part` dir with files, a non-`.part` path (refused, untouched)
      and a dir with a subdirectory (refused, untouched)

**Dependencies:** None (Phases 1–6 are committed)
**Files:** `src/modes.c/.h`, `tests/test_modes.c`, `CMakeLists.txt`
**Scope:** M

### - [x] T17: Multi-step job runner
**Description:** `Item` holds the `GPtrArray` of `VcStep` from `vc_mode_steps()`. `start_job()` runs the steps in order,
and the next step starts only after the previous one's three events (stdout EOF, stderr EOF, exit). Exit 0 or the step's
`soft_exit` continues. When the last step ends, each step with an `output` and an existing `part` is moved with
`g_file_move(..., G_FILE_COPY_NONE)`, and intermediate parts are removed. On stop, failure or close, every step's part goes
through `vc_remove_part()`. Steps without `progress` pulse the bar. Pause sends `SIGSTOP` to the current step's process.
The `State` machine, selection rules and the trash path are unchanged. Compress video is the only mode, so it is a
one-step job.

**Acceptance criteria:**
- [x] Compress video behaves exactly as before: progress, pause/resume, stop, skip, "already efficient", trash, close
      mid-encode
- [x] Kept from the performance work ([todo-performance.md](todo-performance.md)): each step's ffmpeg still starts at
      nice 10 / idle I/O through the launcher's child setup (R2), and a kept output still gets the source's mtime
      before the trash check (R12)
- [x] `grep -nE 'unlink|g_file_delete|remove\(' src/` shows deletes only in `vc_remove_part()` and rejected-output cleanup

**Verification:**
- [x] ctest green (Debug and ASan builds)
- [x] Manual (Xvfb, ASan): full fixture run at `faster` with subtitles and auto-trash, stop mid-file, pause/resume,
      close mid-encode (exit 0, no ffmpeg, no `.part`), then restore the trashed originals and compare checksums

**Dependencies:** T16
**Files:** `src/main.c`
**Scope:** M

### - [x] T18: Choose Files and the list source
**Description:** A **Choose Files** button next to Choose Folder opens `gtk_file_dialog_open_multiple()` with the video
filter. The app keeps the list source (one folder, or a list of files) so that a later mode switch can reload it. Each
file's output goes into `<its folder>/compressed/`. The window title becomes `Media Tools`. Folder, repo and binary names
stay the same.

**Acceptance criteria:**
- [x] Choosing two videos from two folders lists only those two, and each output lands in its own folder's `compressed/`
      (app-shell criterion 2)
- [ ] Choose Folder afterwards replaces the list (verified), and vice versa (not yet clicked through)
- [x] Both buttons are greyed while anything is queued, encoding or being checked

**Verification:**
- [x] Manual (Xvfb): the two-folder run, then Choose Folder on the fixtures
- [x] Manual: the window title reads `Media Tools`

**Dependencies:** T17
**Files:** `src/main.c`
**Scope:** S

### Checkpoint 7
- [ ] SPEC.md criteria 1–3 and 5–13 still pass in Compress video mode (Xvfb, ASan). Criterion 4 is still waiting on your clip
- [x] App-shell criterion 2 passes, ctest green, Debug build clean
- [ ] Review with you, then commit before Phase 8

---

## Phase 8: Web video ([SPEC-web-video.md](../SPEC-web-video.md))

### - [ ] T19: `web_video.c` core and fixtures
**Description:** `vc_web_video_steps()` returns one ffmpeg step writing `web/.<name>.part.mp4` → `web/<name>.mp4`
(lowercase extension), or NULL + `web/<name>.mp4 exists`. It builds the argv from the spec's "Encode settings": even-width
scale capped at Max width, the zscale + `tonemap=hable` chain when `vc_probe_is_hdr()` (`bt2020` fallback for missing
primaries/matrix), `-crf 23 -preset slow`, `-an` or AAC 128k stereo, metadata and chapters stripped, `+faststart` and
progress. `vc_mode_steps()` dispatches to it. `make-fixtures.sh` adds `odd-853x479.mp4` and a 10-bit PQ `hdr10-pq.mov`
(using `setparams`, as for HLG).

**Acceptance criteria:**
- [ ] Every test in the spec's unit list passes
- [ ] The fixture script creates both new files, and ffprobe shows 853×479 and `smpte2084`

**Verification:**
- [ ] Unit (`tests/test_web_video.c`): 8-bit 1080p at 1920 / 1280 / Original (each element), Remove audio on/off, HLG
      and PQ tone-map with the right `tin`, no tone-map for 10-bit SDR and 8-bit, `bt2020` fallback, path naming,
      existing-output skip, special-character filename
- [ ] Headless: run the argv on the HLG and PQ fixtures through a throwaway driver, and check `yuv420p`/`bt709`

**Dependencies:** T16
**Files:** `src/web_video.c`, `src/modes.c/.h`, `tests/test_web_video.c`, `tests/make-fixtures.sh`, `CMakeLists.txt`
**Scope:** M

### - [ ] T20: Mode dropdown, options bar, tool check, web mode UI
**Description:**
- A **Mode** dropdown in the header lists only the modes that exist: `Compress video` (default) and `Web video (Safari)`.
- The options bar under the header shows only the current mode's controls. Compress video: Speed + ⓘ, Keep subtitles,
  Move to Trash. Web video: **Max width** (`Original`, `3840`, `2560`, `1920` default, `1280`, `960`, `640`) and
  **Remove audio**.
- Startup check split: missing `ffmpeg`/`ffprobe` is still fatal. `ffmpeg -encoders` and `ffmpeg -filters` run once and set
  the `VC_HAVE_*` bits. A mode with a missing tool shows a banner (`vc_mode_missing_tool()` text) and greys Start.
- Mode, Choose Folder and Choose Files are locked while anything is queued, encoding or being checked.
- Switching mode rebuilds the list from the same source with the new filter, resets row state and shows
  `N files hidden (not videos/images)` when some are filtered out.
- In Web video mode, the Trash button and checkbox are hidden, the size estimate is hidden, there is no "not smaller"
  rule, and a finished row shows `4.1 MB · −63%` or `+12%`.

**Acceptance criteria:**
- [ ] Web-video criteria 1–4 and 7 pass on the fixtures
- [ ] The mode is locked during a job (app-shell criterion 4)
- [ ] Without `libx264` (a fake `ffmpeg` wrapper that filters `-encoders`), Web video shows the banner and Compress video
      still works

**Verification:**
- [ ] Manual (Xvfb): run Web video on the fixture folder, then ffprobe each output (h264, High, yuv420p, avc1, even
      dimensions, aac or none) and do the `moov`-before-`mdat` check
- [ ] Manual: Max width 1280 on 1080p / 720p / portrait; `odd-853x479.mp4` at Original → 852×478
- [ ] Manual: HLG and PQ outputs are `bt709`, plus a screenshot next to the source
- [ ] Manual: stop mid-file, re-run skip, close mid-encode (no `.part`, exit 0)

**Dependencies:** T18, T19
**Files:** `src/main.c`
**Scope:** M

### Checkpoint 8
- [ ] Web-video criteria 1–4 and 7 and app-shell criterion 4 pass, ctest green
- [ ] You check web-video 5 (no GPS tag on a real phone clip's output) and 6 (plays in Safari on iOS before fully downloaded)
- [ ] Review with you, then commit

---

## Phase 9: Video → frames ([SPEC-video-frames.md](../SPEC-video-frames.md))

### - [ ] T21: `frames.c` core
**Description:** `vc_parse_time()`, `vc_frames_jpeg_qscale()`, `vc_frames_estimate()` and `vc_frames_steps()` as in the
spec. The step writes the directory part `frames/.<stem>.part/frame_%04d.<ext>` and has output `frames/<stem>/`. It skips
with `frames/<stem>/ exists`, or with `starts after the end` when Start ≥ duration. Trim goes before `-i`. The step's
progress duration is the range length. `vc_build_ffprobe_argv()` also asks for `avg_frame_rate`, and `vc_parse_probe()`
fills `VcProbe.fps` (`30000/1001`; `0/0` → 0).

**Acceptance criteria:**
- [ ] Every test in the spec's unit list passes, and `test_compress` still passes with the extra probe field

**Verification:**
- [ ] Unit (`tests/test_frames.c`): time parsing accepts/rejects, qscale 1/80/100 → 31/8/2 and monotonic, estimates
      (whole, range, End past end, Start past end = 0, `Every frame` at 30000/1001), WebP and JPG argv with and without
      range, no `fps=` for `Every frame`, Original width, tone-map present or absent, dir naming, special-character name
- [ ] Unit: `avg_frame_rate` parsing in `vc_parse_probe()`

**Dependencies:** T16
**Files:** `src/frames.c`, `src/compress.c/.h`, `src/modes.c/.h`, `tests/test_frames.c`, `tests/test_compress.c`,
`CMakeLists.txt`
**Scope:** M

### - [ ] T22: Frames mode UI
**Description:** Add `Video → frames` to the Mode dropdown. Options bar: **FPS** (`5`, `10`, `12`, `15` default, `24`,
`30`, `Every frame`), **Start** and **End** entries (an invalid value turns the field red and greys Start), **Max width**
(default `1920`), **Format** (`WebP` default, `JPG`) and **Quality** (1–100, default 80). Each row's hint shows
`≈ N frames` and updates when FPS or the range changes. A finished row shows `150 frames · 9.8 MB` (count and total size
of the folder). Trash controls are hidden. The runner moves the finished directory part into place, and stop, failure
or close removes it.

**Acceptance criteria:**
- [ ] Video-frames criteria 1–7 pass

**Verification:**
- [ ] Manual (Xvfb): `720p.mp4` at 15 fps WebP q80 → 900 frames (±1), with `≈ 900 frames` shown beforehand
- [ ] Manual: Start `10`, End `0:12` at 12 fps → 24 frames (±1), and the first frame matches the source at 10 s
- [ ] Manual: Max width 640 → 1080p gives 640 wide, portrait gives 640×1138
- [ ] Manual: JPG q80 and HLG frames look right next to a source screenshot
- [ ] Manual: stop mid-extraction (no `.part` dir, no final dir), re-run extracts, a second re-run skips
- [ ] Manual: Start `abc` turns red and greys Start. A Start past one video's length skips only that video

**Dependencies:** T20, T21
**Files:** `src/main.c`
**Scope:** M

### Checkpoint 9
- [ ] Video-frames criteria 1–7 pass, ctest green, ASan run of the mode clean
- [ ] Review with you, then commit

---

## Phase 10: Compress images ([SPEC-image-compress.md](../SPEC-image-compress.md))

### - [ ] T23: `images.c` core
**Description:** Accepted extensions (`.jpg .jpeg .png .webp .bmp .tif .tiff`), `vc_probe_has_alpha()`,
`vc_image_steps()` and `vc_image_should_trash()` as in the spec. Step plan: JPEG → magick jpg + webp. PNG → pngquant
(`soft_exit = 99`) + webp, with an extra intermediate magick resize step when Max width is set. WebP → webp only.
BMP/TIFF → png (alpha) or jpg (no alpha) + webp. Each output that already exists is dropped from the plan, and the row is
skipped only when all of them exist. Add captured ffprobe output for a JPEG, a PNG with alpha and a `pal8` PNG to the
probe tests, so still images parse (no duration).

**Acceptance criteria:**
- [ ] Every test in the spec's unit list passes
- [ ] `vc_parse_probe()` accepts the captured image probes

**Verification:**
- [ ] Unit (`tests/test_images.c`): extensions (GIF/SVG/HEIC rejected), plan per source type including the 3-step PNG
      resize, argv element by element (`-resize '1280>'` only with a Max width, `--` before the pngquant input), output
      naming `IMG.JPEG` → `IMG.jpg` + `IMG.webp`, per-output and all-exist skip, special-character name
- [ ] Unit: `vc_probe_has_alpha()` for `rgba`, `ya8`, `yuva420p`, `rgb24`, `yuvj420p`, `pal8`
- [ ] Unit: `vc_image_should_trash()` truth table, height ±1 accepted, ±2 rejected

**Dependencies:** T16
**Files:** `src/images.c`, `src/modes.c/.h`, `tests/test_images.c`, `tests/test_compress.c`, `CMakeLists.txt`
**Scope:** M

### - [ ] T24: Image fixtures
**Description:** `make-fixtures.sh` writes `images/` with magick: a noisy photo-like `photo.jpg` (wider than 1280),
`rotated.jpg` with EXIF orientation 6, `screenshot.png`, `alpha.png`, `already-tiny.png`, `scan.tiff`, `logo.bmp`,
`photo.webp`, a 1001×667 `.jpg`, a name with quotes/`$`/unicode, and the decoys `anim.gif` and `fake.png` (text).

**Acceptance criteria:**
- [ ] All files exist, `magick identify` shows orientation 6 on `rotated.jpg` and alpha on `alpha.png`

**Verification:**
- [ ] Run the script into a fresh directory and list the files with `magick identify`

**Dependencies:** None
**Files:** `tests/make-fixtures.sh`
**Scope:** S

### - [ ] T25: Images mode UI
**Description:** Add `Compress images` to the Mode dropdown, with the banner when `magick` or `pngquant` is missing.
Rows show dimensions, format and size. Options bar: **Max width** (default `Original`). Jobs run the multi-step plan with
a pulsing bar. After the last step, each output that isn't at least 5% smaller than the source is deleted on its own, and
the row shows e.g. `jpg 412 kB −58% · webp 288 kB −71%`, `jpg already optimized`, or `Skipped – already optimized` when
nothing was kept. pngquant exit 99 counts as "no output".

**Acceptance criteria:**
- [ ] Image criteria 1–5, 7 and 9 pass
- [ ] Switching from a video mode to Compress images on a mixed folder shows the right hidden count (app-shell criterion 3)
- [ ] Without pngquant on PATH, only Compress images shows the banner and greys Start (app-shell criterion 5)

**Verification:**
- [ ] Manual (Xvfb): run the mode on `images/`, open every output, check `rotated.jpg` is upright with no orientation/GPS
      tag, `alpha.png` outputs keep transparency, and nothing larger than its source is kept
- [ ] Manual: Max width 1280 → the 1001×667 file keeps its size, `photo.jpg` comes out 1280 wide
- [ ] Manual: stop mid-batch (no `.part` or intermediate file), a re-run skips finished images (app-shell criterion 6)

**Dependencies:** T20, T23, T24
**Files:** `src/main.c`
**Scope:** M

### - [ ] T26: Trash for images
**Description:** In Compress images, the Move to Trash checkbox and button come back. The check probes the same-type
output (`.png`/`.jpg` for BMP/TIFF) and calls `vc_image_should_trash()` with the expected width `min(src, W)` and height.
If only the `.webp` survived, the row says `original kept (no jpg copy)`. Removal is `g_file_trash()` only.

**Acceptance criteria:**
- [ ] Image criterion 8 passes: verified originals are in the Trash and restore byte-identical, and an original with only
      a webp kept or a failed step stays put

**Verification:**
- [ ] Manual (Xvfb): run with trash on over a copy of `images/`, `gio list trash://`, restore all, compare checksums
- [ ] Manual: stop during the output check keeps the original

**Dependencies:** T25
**Files:** `src/main.c`
**Scope:** S

### - [ ] T27: Calibrate image quality
**Description:** On 5 of your real photos and 5 real PNG screenshots/graphics, compare output sizes against TinyJPG and
TinyPNG and have you judge the look. If the sizes are more than about 15% off, propose new values for 82 / 65-90 / 80
(ask first) and record the numbers in `SPEC-image-compress.md`.

**Acceptance criteria:**
- [ ] Image criterion 6 passes, judged by you

**Verification:**
- [ ] Results table (file, source, ours, Tiny*, difference) appended to `plan.md`

**Dependencies:** T25. Needs your files and your eyes
**Files:** `tasks/plan.md`, possibly `src/images.c` and the spec (only with your approval)
**Scope:** S

### Checkpoint 10 (Media Tools complete)
- [ ] Image criteria 1–9 pass, and app-shell criteria 3, 5, 6 and 7 pass
- [ ] ASan run of every mode clean, ctest green, Debug build clean
- [ ] `CAPABILITIES.md` State column says Built for every module, HANDOFF.md updated
- [ ] Review with you
