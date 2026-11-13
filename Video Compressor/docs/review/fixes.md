# Performance and security review: recommended fixes

Numbers match [findings.md](findings.md). Decisions that need you first are in [questions.md](questions.md).
Applied on 2026-11-11 (PERFORMANCE.md items 1 and 4): the release build half of 7, and 8, 9, 10, 12 (12 more
simply than below: `probe_done` force-exits the ffprobe when its read is cancelled). The rest is not applied yet. Per the SPEC rule, run the tests before every commit:
`ctest --test-dir "Video Compressor/build" --output-on-failure`.

## Suggested order

| Order | # | Fix | Size | Needs an answer first |
|---|---|---|---|---|
| 1 | 1 | Sort by display name; make tooltips valid UTF-8 | 2 small edits | No |
| 2 | 2 | Escape `%` in the frames pattern | 3 lines + test | No |
| 3 | 3 | Don't offer Trash when the name clashes with another video | ~15 lines + test | **Yes, Q1** |
| 4 | 5 | Refuse images above a pixel cap; limit ImageMagick's disk use | ~10 lines + test | Q2 (the cap) |
| 5 | 8 | Cache the folder scan in `has_twin()` | ~20 lines + test | No |
| 6 | 10 | Remove the row-by-row `is_busy()`; skip the hidden speed guide | 2 lines | No |
| 7 | 6, 7 | Name the format for `magick`; ffmpeg protocol whitelist; release build | small each | Q4 (build) |
| 8 | 9, 13 | Count sample bytes as they stream; drop stale dialog answers | ~25 + ~6 lines | No |
| – | 4, 11, 12 | Optional, or not recommended now | – | Q3, Q5 |

---

## 1. Crash on non-UTF-8 filenames

**Sort** ([main.c:1066-1070](../../src/main.c#L1066-L1070)): compare display names, which are always valid UTF-8. This makes O(n log n) small allocations, which is fine at these list sizes.

```c
static int
compare_items (gconstpointer a, gconstpointer b)
{
    /* display names: g_utf8_collate crashes on raw paths that aren't UTF-8 */
    g_autofree char *x = g_filename_display_name ((*(Item **) a)->path);
    g_autofree char *y = g_filename_display_name ((*(Item **) b)->path);
    return g_utf8_collate (x, y);
}
```

**Tooltips** ([main.c:374-381](../../src/main.c#L374-L381)): fix it once in `set_status()` instead of at the four call sites that pass a path.

```c
it->tip = tip != NULL ? g_utf8_make_valid (tip, -1) : NULL;
```

**Check:** `main.c` has no unit tests. Test by hand: `touch "$(printf 'caf\xe9.mp4')" b.mp4` in a test folder, then open it. The app should list both files and not crash.

## 2. `%` in the frames pattern

[frames.c:445-446](../../src/frames.c#L445-L446): ffmpeg reads every `%` in the output path, so double the ones that come from the folder name.

```c
g_autofree char *pattern_name = g_strdup_printf ("frame_%%04d.%s", ext);
g_autoptr (GString) dir = g_string_new (part);
g_string_replace (dir, "%", "%%", 0);   /* ffmpeg reads % anywhere in the pattern */
g_autofree char *pattern = g_build_filename (dir->str, pattern_name, NULL);
```

`g_string_replace` needs GLib 2.68 or later (2.88 is installed). `step->part` stays unescaped, which is correct: the runner creates and removes that directory itself.

**Test** (`tests/test_frames.c`): for input `/v/50% off.mp4`, assert that the last argv element is `/v/frames/.50%% off.part/frame_%04d.webp` and that `s->part` is `/v/frames/.50% off.part`.

## 3. Name clash between videos (decide Q1 first)

Two options. **A** is the recommendation because it changes no output names.

**A. Flag the clash and keep the original safe.**
1. Add `gboolean vc_video_has_twin (const char *input)` to `compress.c`. It is TRUE when another video in the same folder has the same stem (`clip.mov` + `clip.mp4`). It is the same scan as `has_twin()` in `images.c`, using `vc_is_video_name()`.
2. In `encode_next()` ([main.c:1797](../../src/main.c#L1797)), when `steps == NULL` in a video mode and the input has a twin, don't point `it->out` at someone else's copy:

```c
if (steps == NULL && app.mode != VC_MODE_IMAGES && vc_video_has_twin (it->path)) {
    it->state = ST_DONE;
    g_clear_pointer (&it->out, g_free);   /* not ours: never offer Trash against it */
    set_status (it, "Name clash", "Another video here has the same name, so its copy has "
                                  "this name too. Rename one of them and run again.");
    continue;
}
```

The video that encodes first still gets trashed normally, because `finish_job()` only trashes against an output made in this run. Only the second one is blocked.

**Test** (`tests/test_compress.c`): in a temp folder with `clip.mov` and `clip.mp4`, `vc_video_has_twin` is TRUE for both. With `clip.mov` and `other.mp4`, it is FALSE.

**B. Give twins their own names, as images do:** `compressed/clip.mov.mp4` and `compressed/clip.mp4.mp4`. Both videos then compress and both can be trashed safely. The downside: names change, so earlier outputs of twins aren't recognised and are encoded again. See Q1.

## 4. Header-only check before trashing (optional, see Q3)

If you want it, decode the last 2 seconds of the copy before trashing. That catches a copy that is cut short or corrupt at the end, at a cost of about 1 s per video:
`ffmpeg -v error -sseof -2 -i <out> -f null -`, and require exit 0 with empty stderr. This would chain after the ffprobe in `check_then_trash()`.

## 5. Image size cap and ImageMagick limits

1. Add `gboolean vc_image_too_large (const VcProbe *probe)` to `images.c`, comparing `(guint64) width * height` against `VC_IMAGE_MAX_PIXELS` (200 MP proposed, see Q2). `pngquant` needs 4 bytes per pixel, so 200 MP is about 800 MB.
2. In `probe_done()` ([main.c:1033](../../src/main.c#L1033)), when in Compress images mode:

```c
if (it->probe != NULL && app.mode == VC_MODE_IMAGES && vc_image_too_large (it->probe)) {
    error = g_error_new (VC_ERROR, 0, "Too large: %d×%d", it->probe->width, it->probe->height);
    g_clear_pointer (&it->probe, vc_probe_free);   /* shows as Unreadable, reason in the tooltip */
}
```

3. Cap ImageMagick's disk cache: in `magick_in()` ([images.c:595](../../src/images.c#L595)), add `"-limit", "disk", "8GiB"` before the input path. Update the expected argv arrays in `tests/test_images.c` (lines 152-215).

**Test:** a probe of 20000×20000 is too large; 6000×4000 is not.

## 6. Name the format for `magick`

In `magick_in()`, put the source type in front of the path so ImageMagick can't switch to another decoder based on the contents:

```c
static const char *const CODER[] = { [SRC_JPEG] = "jpeg:", [SRC_PNG] = "png:", [SRC_WEBP] = "webp:" };
const char *coder = t == SRC_OTHER ? (tiff ? "tiff:" : "bmp:") : CODER[t];
g_autofree char *in = g_strconcat (coder, input, tiff ? "[0]" : "", NULL);
```

`magick_in()` then needs the `SrcType`, which every caller already has. Update the argv expectations in `tests/test_images.c` (for example `"jpeg:/p/IMG.JPEG"` and `"tiff:/p/scan.tif[0]"`). `pngquant` reads only PNG, so it needs no change.

## 7. ffmpeg protocols and the release build

**Protocol whitelist:** add `"-protocol_whitelist", "file"` right before each `-i`:
- `vc_build_ffprobe_argv`, `vc_build_ffmpeg_argv` and `vc_build_sample_argv` (inside the loop, once per input) in `compress.c`
- `build_argv` in `web_video.c` and in `frames.c`

`-progress pipe:1` is an output and isn't affected. Update the argv expectations in the tests.

**Release build (see Q4):** in `CMakeLists.txt`:

```cmake
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-fstack-protector-strong $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>)
```

Then install from a separate folder so `build/` stays the Debug test build:

```
cmake -S "Video Compressor" -B "Video Compressor/build/release" -DCMAKE_BUILD_TYPE=RelWithDebInfo --install-prefix ~/.local
cmake --build "Video Compressor/build/release" -j && cmake --install "Video Compressor/build/release"
```

## 8. Cache the folder scan in `has_twin()`

Keep the API unchanged. Remember the last folder's stems and scan again only when the folder's mtime changes. Creating or deleting a file in a folder changes its mtime, and outputs go into a subfolder, so they don't trigger rescans.

```c
/* stem -> number of accepted images with it, for one folder; rebuilt when its mtime changes */
static GHashTable *twin_stems;
static char *twin_dir;
static struct timespec twin_mtime;

static gboolean
has_twin (const char *dir, const char *name, const char *stem)
{
    (void) name;
    GStatBuf st;
    if (g_stat (dir, &st) != 0)
        return FALSE;
    if (twin_stems == NULL || g_strcmp0 (dir, twin_dir) != 0
        || st.st_mtim.tv_sec != twin_mtime.tv_sec || st.st_mtim.tv_nsec != twin_mtime.tv_nsec) {
        g_clear_pointer (&twin_stems, g_hash_table_unref);
        twin_stems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
        for (const char *e; d != NULL && (e = g_dir_read_name (d)) != NULL;) {
            const char *dot = strrchr (e, '.');
            if (dot == NULL || dot == e || !vc_mode_accepts (VC_MODE_IMAGES, e))
                continue;
            char *s = g_strndup (e, dot - e);
            guint n = GPOINTER_TO_UINT (g_hash_table_lookup (twin_stems, s));
            g_hash_table_replace (twin_stems, s, GUINT_TO_POINTER (n + 1));
        }
        g_free (twin_dir);
        twin_dir = g_strdup (dir);
        twin_mtime = st.st_mtim;
    }
    return GPOINTER_TO_UINT (g_hash_table_lookup (twin_stems, stem)) > 1;
}
```

The `> 1` count matches today's rule: a twin is *another* accepted image with the same stem, and the input itself counts once. The existing `strchr (e + n + 1, '.') == NULL` check is covered, because the stem is everything before the *last* dot.

**Check:**
- `/images/twins` must still pass.
- Add a case: call once, create `photo.png` next to `photo.jpg`, call again, and expect TRUE (the mtime changed).
- Rerun the benchmark from findings #8: 4,000 images should drop from 7.56 s to well under 1 s.

If you take option 3B, the same cache serves videos.

## 9. Count the GPU sample bytes as they stream

Replace `g_subprocess_communicate_async()` in `sample_next()` with a loop that skips stdout in 1 MiB chunks and adds up the bytes. `g_input_stream_skip_async` reads and discards. Wait for the exit status at end of file:

```c
static guint64 sample_bytes;

static void
sample_read (GObject *src, GAsyncResult *res, gpointer user_data)
{
    g_autoptr (GError) error = NULL;
    gssize n = g_input_stream_skip_finish (G_INPUT_STREAM (src), res, &error);
    if (n > 0) {
        sample_bytes += n;
        g_input_stream_skip_async (G_INPUT_STREAM (src), 1 << 20, G_PRIORITY_LOW,
                                   app.sample_cancel, sample_read, NULL);
    } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_subprocess_wait_async (app.sample_proc, app.sample_cancel, sample_done, NULL);
    }
}
```

`sample_done()` then calls `g_subprocess_wait_finish()` and uses `sample_bytes` in place of `g_bytes_get_size (out)`. The early return on cancellation and the timeout stay as they are.

## 10. Row redraws whose cost grows with the square of the row count

1. In `render_actions()` ([main.c:408](../../src/main.c#L408)), change `app.job != NULL || is_busy ()` to `app.job != NULL`. When no job is running, the play button really does start the item at once, so "Compress this video" is the correct tooltip. This makes `render_row()` cost the same however long the list is.
2. Add this to the top of `update_speed_guide()`:

```c
if (app.mode != VC_MODE_COMPRESS)
    return;   /* the guide lives in the compress options; load_source rebuilds it on the way back */
```

## 13. Stale dialog answer after a reload

Give each list load a number and ignore dialog answers from an earlier load:
- In `App`: `guint load_gen;`. In `load_source()`: `app.load_gen++;`.
- In `act_start()`: pass `GUINT_TO_POINTER (app.load_gen)` as the dialog's `user_data`. `confirm()` currently passes NULL, so give it a `user_data` parameter.
- In `start_confirmed()` and `trash_confirmed()`: `if (GPOINTER_TO_UINT (user_data) != app.load_gen) return;`

## Not recommended now

- **#11, `GtkListView`:** a large rewrite of row handling, worth it only if folders of thousands of files are normal (Q5). With fixes 8 and 10, a list of a few thousand rows stays usable.
- **#12, orphan ffprobes:** killing them means keeping each `GSubprocess` connected to the cancellable and disconnecting it in `probe_done()`, which is more code than the problem costs. Revisit if switching folders on network drives feels slow.
