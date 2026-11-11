# Performance and install size: planned improvements

Written 2026-11-11. Nothing here is built yet. The two goals:
- **Faster:** batches finish sooner and the window stays responsive with big folders.
- **Smaller to install:** the Flatpak, the AppImage and the plain folder build all bundle ffmpeg, ffprobe, ImageMagick and
  pngquant so nothing has to be installed. Those tools, not the app, are almost all of the install size.

History and past measurements are in [LOG.md](LOG.md). Open bugs and security fixes from the review are in
[review/findings.md](review/findings.md) (fixes in [review/fixes.md](review/fixes.md), open questions in
[review/questions.md](review/questions.md)).

## Decision needed first: default speed

The default was changed from Slow to Medium on 2026-11-11 to match the v3 design. The 2026-11-09 corpus test
([LOG.md](LOG.md#medium-vs-slow-and-the-gpu-on-real-clips-r0-r3-r8)) found Medium 1.6–2.7 VMAF below Slow on 4 of 6 real
clips, which failed the project's 1-point limit. Medium is about twice as fast. Keep Medium, or go back to Slow?
SPEC.md Decision 6 still says Slow and must be updated to match whichever is chosen.

## Suggested order

| # | Change | Goal | Gain | Effort | Risk |
|---|---|---|---|---|---|
| 1 | Release build | speed | snappier window; also hardening | XS | none |
| 2 | Several images at once | speed | image batches several times faster | M | medium |
| 3 | Trimmed ffmpeg for the bundles | size | biggest install-size cut | M | low |
| 4 | Review fixes #8, #10, #12 (+ #9) | speed | no freezes on huge folders | S each | low |
| 5 | libvips instead of ImageMagick | size + speed | smaller bundle, faster images | M | medium (outputs change) |
| 6 | Subset the Inter font | size | app binary 2 MB → under 1 MB | XS | none |
| 7 | Two GPU encodes at once | speed | ~1.3–1.8× on the GPU path | M | medium |
| 8 | Decoder thread budget | speed | a few % on the x265 path | XS | low |

Effort: XS < 1 h · S ≈ half a day · M ≈ 1–2 days, including tests.

## 1. Release build

**Now:** the installed app is a Debug build (`-O0`, no hardening), see review finding #7 and question Q4.
**Change:** default `CMAKE_BUILD_TYPE` to `RelWithDebInfo`, add `-fstack-protector-strong` everywhere and
`-D_FORTIFY_SOURCE=3` outside Debug, and strip on install (`cmake --install --strip`). Install from `build/release/`;
`build/` stays the Debug test build. All three packages are built from the release build.
**Verify:** `ctest` green on both builds; `file` shows the installed binary stripped.

## 2. Several images at once

**Now:** one job at a time in every mode (SPEC-app-shell.md Decision 4). magick and pngquant mostly use one core, so
14 of this laptop's 16 threads sit idle during an image batch.
**Change:** for Compress images only, run up to `MIN (8, cores / 2)` jobs at once. Videos stay one at a time, because x265
already uses every core. Needs: `app.job` becomes a small set; Pause and Stop act on every running job; the footer counts
running jobs; the per-row bar already works per item. Update SPEC-app-shell.md Decision 4.
**Verify:** time a 100-image folder before and after (target: ≥ 3× faster); ASan run with Pause, Stop and closing
mid-batch; no `.part` files left; every output identical to a one-at-a-time run (`sha256sum`).

## 3. Trimmed ffmpeg for the bundles

**Now:** prebuilt static ffmpeg builds carry hundreds of codecs, formats and filters the app never uses (roughly
100 MB+ for ffmpeg and ffprobe; to be measured).
**Change:** build one static ffmpeg + ffprobe for the packages with:
- **every common decoder and demuxer kept:** users drop in any video or image;
- **only the encoders used:** libx265, libx264, libwebp, mjpeg, png, aac, and the hevc_vaapi GPU path;
- **only the filters used:** scale, zscale, fps, format, transpose, the vaapi upload and transpose filters, and whatever
  `src/*.c` passes to `-vf`;
- **only the muxers used:** mp4, matroska, image2, plus the `null` muxer the tests use.
`--disable-network`, `--disable-doc`, `--disable-debug`, then strip. The list must come from grepping the argv builders in
`src/compress.c`, `src/web_video.c`, `src/frames.c` and `src/images.c`, so nothing the app uses goes missing.
**Licence:** with x264/x265 the build is GPL, so each package ships under GPL terms with the ffmpeg source offer.
**Verify:** all 5 test suites, then the full fixture run in every mode with the bundled tools first on `PATH`; outputs
match those from the system ffmpeg (same argv, same version). Record the before/after size here.

## 4. Fixes the review already worked out

From [review/findings.md](review/findings.md), with code-level fixes in [review/fixes.md](review/fixes.md):
- **#8:** image output planning scans the folder once per image (`has_twin()`), so time grows with the square of the
  folder size: 4,000 images took 7.6 s, and 20,000 would freeze Start for tens of seconds. Scan the folder once into a set.
- **#10:** each keystroke or setting change redraws every row.
- **#12:** switching folders leaves running ffprobes alive.
- **#9:** the GPU size sample keeps its whole output in RAM; count bytes instead.
- **#11:** only if folders of 10,000+ files are common (question Q5): move the list to `GtkListView`. Large rewrite.

## 5. libvips instead of ImageMagick

**Now:** ImageMagick does auto-orient, resize, metadata strip and the JPEG/WebP encodes. A portable ImageMagick adds tens
of MB to each bundle.
**Change:** use the `vips` command-line tool for the same steps. It is smaller, much faster and uses far less memory.
pngquant stays for PNG.
**Risk:** output sizes and look change. Do it together with T27 (the TinyJPG/TinyPNG calibration, still open), so the
image quality numbers are tuned once, on the new tool. Ask before changing what Compress images writes.
**Verify:** the image unit tests (argv changes), the image fixture run (alpha kept, EXIF orientation applied, GPS
removed, colour profile kept, twins), and the T27 comparison.

## 6. Subset the Inter font

**Now:** four full Inter weights (about 1.6 MB) are embedded in the binary (`data/fonts`).
**Change:** subset each to Latin plus the symbols the UI uses (→ ≈ … ✓ · ⚠ ∞ ×) with `pyftsubset`, keeping the OFL
licence file. Keep the originals' names so `load_fonts()` needs no change.
**Verify:** screenshot every screen; no missing-glyph boxes, including non-Latin file names (those fall back to the system
font, which is fine).

## 7. Two GPU encodes at once

See [research/handbrake/adopt/08-parallel-gpu-encodes.md](research/handbrake/adopt/08-parallel-gpu-encodes.md). The
GPU encoder block is not saturated by one stream. Same runner change as item 2, limited to 2 jobs and to the GPU path.
**Verify:** batch time on the `build/perf` corpus; VMAF unchanged.

## 8. Decoder thread budget

See [research/handbrake/adopt/09-decoder-thread-budget.md](research/handbrake/adopt/09-decoder-thread-budget.md).
Limit ffmpeg's decoder threads so they don't compete with x265's. Measure on the corpus; keep only if it helps.

## Already done (don't redo)

GPU decode on the GPU path (`-hwaccel vaapi`), x265 `ctu=32` for small video and `rect=0:rskip=2` at slow, low CPU and
disk priority for encodes, up to 4 ffprobes at once, AAC copy, skipping files predicted to save nothing, Max size and
Max 30 fps options. Details and numbers in [LOG.md](LOG.md).
