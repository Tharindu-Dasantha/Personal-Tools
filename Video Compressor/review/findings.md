# Performance and security review: findings

Reviewed 2026-11-10 at commit `8921588`. Scope: every file in `src/` (about 4,500 lines), read line by line.
The test files were skimmed only. The GUI and ASan were not run for this review.

Fixes for each finding are in [fixes.md](fixes.md), and open decisions are in [questions.md](questions.md). The numbers match across all three files.

**Verified** means the problem was reproduced: the test program and its output are noted under the finding.
**Read** means it was found by reading the code and not reproduced.

| # | Finding | Area | Severity | How found |
|---|---|---|---|---|
| 1 | Crash on a filename that isn't valid UTF-8 | Bug | High | Verified |
| 2 | Video → frames fails for names containing `%` | Bug | Medium | Verified |
| 3 | `clip.mov` and `clip.mp4` share one output, so the wrong original can be trashed | Data safety | Medium | Read |
| 4 | The check before trashing reads headers only | Data safety | Low | Read |
| 5 | No size cap on images before `magick` / `pngquant` | Security | Medium | Read |
| 6 | ImageMagick policy is fully open, and inputs don't name their format | Security | Low | Verified |
| 7 | ffmpeg has no protocol whitelist, and the installed binary is a Debug build (**release build fixed 2026-11-11**; whitelist open) | Security | Low | Read |
| 8 | Image output paths: planning time grows with the square of folder size (**fixed 2026-11-11**: 4,000 images 7.56 s → 0.009 s) | Performance | Medium | Verified |
| 9 | The GPU size sample keeps its whole output in RAM (**fixed 2026-11-11**) | Performance | Low | Read |
| 10 | Each keystroke or setting change redraws every row, at a cost that grows with the square of the row count (**fixed 2026-11-11**) | Performance | Low | Read |
| 11 | Every row's widgets are created up front (`GtkListBox`) | Performance | Low | Read |
| 12 | Switching folders doesn't stop ffprobes already running (**fixed 2026-11-11**: one `force_exit` on cancel) | Performance | Low | Read |
| 13 | A pointer to an old row survives a list reload while the Start dialog is open | Memory | Low | Read |

No memory leaks were found (see "Memory" at the end).

---

## Bugs

### 1. A filename that isn't valid UTF-8 crashes the app on folder load
[src/main.c:1069](../src/main.c#L1069)

`compare_items()` sorts the list with `g_utf8_collate()` on raw paths. When a string isn't valid UTF-8, GLib's normalise step returns NULL and the following `strcoll()` dereferences it.

- **When:** any folder (or file selection) with at least two matching files, one of them named in Latin-1 or another legacy encoding. These names are common in old camera folders, Windows zip extracts and Samba shares.
- **Verified:** a test program calling `g_utf8_collate("/v/caf\xe9.mp4", "/v/b.mp4")` under `en_US.UTF-8` died with SIGSEGV (exit 139).
- **Related:** `set_status(it, NULL, it->out)` ([main.c:1256](../src/main.c#L1256), 1474, 1476, 1488) uses the raw output path as tooltip text. Pango prints "Invalid UTF-8" warnings for such names, and those warnings abort the app under `G_DEBUG=fatal-criticals`.

### 2. Video → frames fails for any video with `%` in its name
[src/frames.c:446](../src/frames.c#L446)

The frames folder is named after the video (`frames/.<stem>.part/`) and becomes part of ffmpeg's output pattern `…/frame_%04d.webp`. ffmpeg reads `%` sequences anywhere in that path. When one of them is invalid (`% o` in `50% off`), it treats the path as a plain filename instead of a pattern.

- **Verified:** with the output path `.50% off.part/frame_%04d.jpg`, ffmpeg 8.1.3 wrote one file literally named `frame_%04d.jpg`, then failed with "Cannot write more than one file with the same name" (exit 234). The row shows **Failed**.
- **Related:** in Compress images mode, ffprobe reads `IMG%03d.jpg` as an image *sequence* pattern, so the file shows as Unreadable or the probe reads a different file. This name is rare.

## Data safety

### 3. Two videos with the same name and different extensions share one output
[src/compress.c:267](../src/compress.c#L267), [src/web_video.c:279](../src/web_video.c#L279), [src/frames.c:386](../src/frames.c#L386), [src/main.c:1797-1806](../src/main.c#L1797-L1806)

`clip.mov` and `clip.mp4` both map to `compressed/clip.mp4`. The same happens with `web/clip.mp4` and `frames/clip/`.

1. The first video encodes normally.
2. The second finds the output already there. `encode_next()` marks it **Already compressed** and sets `it->out` to the *first* video's copy.
3. The second row's Trash button then becomes active. `verify_done()` checks only that the copy is within 1 s in length and at least 5% smaller. Two takes of similar length pass that check.
4. Result: the second original goes to Trash, and no compressed copy of it exists. It can be restored, but the app said it was safe.

Images already avoid this with `has_twin()` (`photo.png` → `photo.png.webp`). Videos have no equivalent.

### 4. The check before trashing reads headers only
[src/main.c:1288-1301](../src/main.c#L1288-L1301)

ffprobe reads the length (video) or the stored size (image) from the container header without decoding any frames. A file truncated after its header would pass. In practice two things cover this: ffmpeg/magick must exit 0, and the `.part` file is renamed atomically. The remaining gap is small.

## Security

What is already sound, checked during this review:
- Every external tool is started with an argument list (`GSubprocess`). There is no shell, so filenames can't inject commands.
- Input paths are absolute, so ffmpeg protocol prefixes (`concat:`, `http:`) and ImageMagick's `|command` input can't be triggered by a filename.
- Outputs are never overwritten: `g_file_move()` runs without the OVERWRITE flag, and `-y` only ever hits the app's own `.part` file.
- `vc_remove_part()` deletes only `.part` names and refuses directories that contain subdirectories. Symlinks are unlinked, never followed.
- Web video strips all metadata, including GPS. Image outputs strip EXIF (`+profile "!icc,*"`, `pngquant --strip`).
- Every async callback is guarded by a cancellable, the `closing` flag, or the busy lock that keeps the list from reloading.

### 5. No size cap on images before `magick` and `pngquant`
[src/images.c:612](../src/images.c#L612)

A PNG of a few kilobytes can declare 50,000 × 50,000 pixels.
- `pngquant` decodes the whole image into memory: 4 bytes × 2.5 billion pixels = about 10 GB.
- ImageMagick's limits on this machine (`magick -list resource`) are memory 7.4 GiB and **disk unlimited**. Above the memory limit it spills to a disk cache and can fill the disk.

The probe already knows the width and height before any step runs, but nothing checks them.

### 6. ImageMagick's policy is fully open, and inputs don't name their format
[src/images.c:598](../src/images.c#L598)

`/etc/ImageMagick-7/policy.xml` has every rule commented out, so all coders are allowed, and Ghostscript is installed at `/usr/bin/gs`. `magick` picks the decoder from the file's contents, not its extension.

- **Verified:** SVG content saved as `photo2.png` was read as `SVG`. PostScript saved as `photo3.jpg` was read as `PS`, which goes through Ghostscript, a decoder with a long history of security bugs.
- **Mitigated today:** ffprobe reports `width=0` for both files, so `vc_parse_probe()` rejects them as "Bad video size" and they never reach `magick`. A polyglot file that ffprobe accepts while ImageMagick detects something else would get through. That is unlikely but not ruled out. This finding is defence in depth, not an open hole.

### 7. Smaller hardening gaps
- **ffmpeg protocols:** inputs are untrusted media, and ffmpeg runs with no `-protocol_whitelist`. A crafted file (an HLS playlist saved as `.mp4`, for example) can make ffmpeg open other local files or network URLs. ffmpeg 8 restricts the known cases. A whitelist would close the rest.
- **Build:** `~/.local/bin/video-compressor` was installed from `build/` (`CMAKE_BUILD_TYPE=Debug`): `-O0`, no `_FORTIFY_SOURCE`, no `-fstack-protector-strong`. [CMakeLists.txt](../CMakeLists.txt) sets no flags for other build types either.

## Performance and algorithms

### 8. Image output paths: planning time grows with the square of folder size
[src/images.c:518](../src/images.c#L518) (`has_twin`), called from `out_path()`

Every output path reads the whole folder to look for a twin. One image costs about 7 folder scans: `vc_image_steps()` makes 2 to 5 and `image_result()` makes 2. With Move to Trash on, `act_start()` ([main.c:1170](../src/main.c#L1170)) scans once per item, all at once, on the UI thread, before the confirm dialog appears.

- **Verified (`-O2`, `vc_image_steps` + `vc_image_outputs` per image):** 1,000 images took **0.39 s** and 4,000 took **7.56 s**. That is 4× the files for 19× the time.
- **Effect:** with 20,000 photos and Trash on, Start freezes the window for tens of seconds.

### 9. The GPU size sample keeps its whole output in RAM
[src/main.c:1762](../src/main.c#L1762)

`g_subprocess_communicate_async()` collects all of the sample's raw HEVC (up to 12 s of video) into one `GBytes`, only to read its length. That can be tens of MB per sample at 4K. It is freed right away, but each sample still allocates it all.

### 10. Each keystroke or setting change redraws every row, at a cost that grows with the square of the row count
[src/main.c:2351](../src/main.c#L2351) (`on_estimate_input`), wired to the frames Start/End `changed` signal ([main.c:3086](../src/main.c#L3086)), the speed buttons, Max size, Max 30 fps and the frames FPS dropdown.

- It calls `render_row()` for every item. `render_row()` → `render_actions()` → `is_busy()` loops over every item again.
- Each row also calls `current_options()`, which reads about 10 widgets and parses both time fields.
- `update_speed_guide()` destroys and rebuilds about 60 widgets each time, even in modes where the guide is hidden.

This is unnoticeable at 100 rows and laggy at a few thousand.

### 11. Every row's widgets are created up front
[src/main.c:2265](../src/main.c#L2265)

`GtkListBox` doesn't recycle rows. `make_row()` builds about 25 widgets per item at load, so a 5,000-image folder means about 125,000 widgets: slow to load and heavy on memory. `GtkListView` creates widgets only for the rows on screen.

### 12. Switching folders doesn't stop ffprobes already running
[src/main.c:1029](../src/main.c#L1029)

`g_cancellable_cancel(app.probe_cancel)` cancels the *read* of ffprobe's output, but the child process keeps running. Each folder switch can leave up to 4 ffprobes running until they finish, which takes a while on slow or network drives.

## Memory

There is no garbage collector: this is C with GLib reference counting. Every allocation path was traced, and no leaks were found:
- Each `GSubprocess` is owned by its async callback through `g_autoptr`.
- `GDataInputStream`s are released at end of file.
- Right-click popovers are unparented and released on `closed`.
- `gtk_list_box_remove_all()` destroys rows, which also removes them from the size groups.
- `item_free()` frees every heap field, and `it->extra` is always a static string.
- What is still allocated at exit (the static `ffmpeg` path, `app.items`) lives for the whole app and is reclaimed by the OS.

### 13. A pointer to an old row survives a list reload while the Start dialog is open
[src/main.c:1182](../src/main.c#L1182)

`act_start()` stores `app.ask_one` and then opens the "Move to Trash?" confirm dialog. If the list reloads before you answer (a folder dropped onto the window, if the compositor allows a drop while the dialog is up), the pointer refers to a freed item. `do_start()` only compares it by address, so nothing crashes. The worst case is the action applying to a new item that happens to reuse that memory.
