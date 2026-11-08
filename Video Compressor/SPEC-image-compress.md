# Spec: image-compress

Module of [CAPABILITIES.md](CAPABILITIES.md). Depends on `app-shell` ([SPEC-app-shell.md](SPEC-app-shell.md)).

## Objective

A `Compress images` mode that works like `Compress video`, but for images, with TinyPNG-style results. Choose a folder or
some images, press Start, and each image gets a smaller copy in its own format **plus** a `.webp`, ready for the web. It covers
two of the requested features: image compression, and turning images into web-ready formats.

**User stories**
- I pick `Compress images`, choose a folder (or Choose Files), and see every image with its dimensions, format and size.
- Start gives me, in `compressed/`, `photo.jpg` (optimized JPEG) and `photo.webp` for each JPEG, and `logo.png` (quantized
  PNG, like TinyPNG) and `logo.webp` for each PNG.
- Each row shows both results, e.g. `jpg 412 kB −58% · webp 288 kB −71%`.
- **Max width** (`Original` (**default**), `3840`, `2560`, `1920`, `1280`, `960`, `640`) scales larger images down, never up.
- **Move originals to Trash** (checkbox and button) works as in `Compress video`, with image-specific checks.

## Decisions
1. **Accepted inputs:** `.jpg .jpeg .png .webp .bmp .tif .tiff` (case-insensitive). Not accepted: GIF (often animated), SVG
   (already vector), HEIC/AVIF (the installed ImageMagick has no libheif). Files are read with ffprobe for width, height,
   codec and `pix_fmt` (which says whether there is alpha).
2. **Outputs per source:**

   | Source | Same-type output | Plus |
   |---|---|---|
   | JPEG | `name.jpg` via ImageMagick | `name.webp` |
   | PNG | `name.png` via pngquant | `name.webp` (alpha kept) |
   | WebP | `name.webp` only | — |
   | BMP / TIFF | `name.png` (has alpha) or `name.jpg` (no alpha) | `name.webp` |

3. **Settings:**
   - JPEG: `magick IN -auto-orient +profile '!icc,*' [-resize 'W>'] -sampling-factor 4:2:0 -interlace JPEG -quality 82 OUT`
   - PNG: `pngquant --quality=65-90 --speed 3 --strip --output OUT -- IN`. With a resize, a first step runs
     `magick IN -auto-orient -resize 'W>' MID.png` and pngquant reads `MID`, an intermediate step that is deleted afterwards.
   - WebP: `magick IN -auto-orient +profile '!icc,*' [-resize 'W>'] -quality 80 -define webp:method=6 OUT`
   - All three commands were smoke-tested here on 2026-11-08. The quality numbers (82, 65-90, 80) are starting points to
     calibrate against TinyPNG/TinyJPG on real photos (success criterion 6).
4. **Metadata:** EXIF orientation is applied to the pixels, then all metadata except the colour profile is removed. That drops
   GPS and camera data but keeps colours right for Display-P3 phone photos.
5. **The "not smaller" rule applies to each output on its own.** An output that isn't at least 5% smaller than the source is
   deleted, and its part of the row says `jpg already optimized`. If every output is dropped, the row says
   `Skipped – already optimized`. A pngquant exit of `99` (it can't reach quality 65 without visible damage) counts as "no
   output", not an error (`soft_exit = 99`).
6. **Trash rule:** the original is trashed only when **all** of these are true:
   1. every step exited 0 (or soft-exited)
   2. the **same-type** output (for BMP/TIFF, the `.png`/`.jpg`) was kept
   3. ffprobe on that output succeeds, its width is the expected one (`min(src, W)`), and its height is within 1 px of the
      expected one

   If only the `.webp` survived, the original is kept and the row says `original kept (no jpg copy)`. Removal goes through
   `g_file_trash()` only, as in SPEC.md.
7. **Progress:** image steps take well under a second each, so the row bar pulses while active. There is no percentage.
8. **Existing outputs:** each output is skipped if it already exists. The row is skipped only when **all** of them exist.
9. **No size estimate.**

## Tech Stack
Same as [SPEC.md](SPEC.md), plus two runtime tools called through argv like ffmpeg:
- ImageMagick 7 `magick`, already installed (JPEG, PNG, WebP; no HEIC/AVIF).
- `pngquant` 2.18 from the Fedora repo, approved by the user 2026-11-08. Install with `sudo dnf install pngquant`.

## Commands
Same as [SPEC.md](SPEC.md). Fixtures are extended (below).

## Project Structure (additions)
```
src/images.c           → accepted types, output plan, magick/pngquant argv, trash decision
tests/test_images.c    → unit tests
tests/make-fixtures.sh → + images/: photo.jpg (noisy, photo-like), rotated.jpg (EXIF orientation 6),
                         screenshot.png, alpha.png, already-tiny.png, scan.tiff, logo.bmp, photo.webp,
                         odd 1001x667.jpg, a name with quotes/$/unicode, decoys anim.gif and fake.png (text)
```

## Code Style
Same as [SPEC.md](SPEC.md).
```c
GPtrArray *vc_image_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                           char **skip_reason);
gboolean   vc_probe_has_alpha (const VcProbe *probe);        /* pix_fmt rgba, ya8, yuva420p, … */
gboolean   vc_image_should_trash (gboolean opt_on, gboolean steps_ok, gboolean same_type_kept,
                                  int exp_w, int exp_h, int out_w, int out_h);
```

## Testing Strategy
- **Unit (`tests/test_images.c`):**
  - accepted extensions, with GIF/SVG/HEIC rejected
  - step plan per source type: JPEG → 2 steps, PNG → 2 steps, PNG with resize → 3 steps (one intermediate), WebP → 1, BMP or
    TIFF with and without alpha → png/jpg + webp
  - argv checked element by element, including `-resize '1280>'` only when a Max width is set, and `--` before the pngquant
    input
  - output naming (`IMG.JPEG` → `compressed/IMG.jpg` + `compressed/IMG.webp`), per-output existing skip, all-exist skip, a
    name with spaces/quotes/unicode
  - `vc_probe_has_alpha()` for `rgba`, `ya8`, `yuva420p`, `rgb24`, `yuvj420p`, `pal8`
  - `vc_image_should_trash()`: true only for the all-good case, false for each failing condition, height ±1 accepted, ±2 not
- **Manual (fixtures, Xvfb):** the criteria below.
- **Manual (the user):** compare against TinyPNG/TinyJPG on 5 real photos and 5 real PNG screenshots/graphics.

## Boundaries
- **Always:** keep the shared rules · trash only through the rule above · keep the colour profile.
- **Ask first:** new tools beyond magick/pngquant (mozjpeg, oxipng, cwebp, libheif) · AVIF or HEIC support · changing the
  quality numbers after calibration · lossless WebP.
- **Never:** overwrite a file in `compressed/` · trash an original whose same-type copy wasn't kept and verified · keep GPS
  metadata · upscale.

## Success Criteria
1. The fixture folder lists exactly the accepted images. `anim.gif` doesn't appear, and `fake.png` shows `Unreadable`.
2. Every JPEG/PNG/BMP/TIFF fixture gives its same-type (or converted) output plus `.webp`, all of which open in an image
   viewer. `photo.webp` gives one output.
3. `rotated.jpg` comes out upright, and ffprobe/`magick identify` shows no EXIF orientation or GPS on any output.
4. `alpha.png` → `.png` and `.webp` both keep transparency.
5. `already-tiny.png` gives `Skipped – already optimized` (or `png already optimized` with a kept webp), and nothing larger
   than its source is kept.
6. On 5 real photos, the `.jpg` outputs are within about 15% of TinyJPG's size at a look the user accepts. On 5 real PNGs,
   the `.png` outputs are within about 15% of TinyPNG's. If not, retune 82 / 65-90 and record the numbers here.
7. With Max width 1280, the 1001×667 fixture keeps its size and `photo.jpg` (wider) comes out 1280 wide.
8. With trash on: originals with a verified same-type copy are in the Trash and can be restored. An original with only a
   webp kept, or a failed step, stays put.
9. Stopping mid-batch leaves no `.part` or intermediate file, and a re-run skips finished images.

## Open Questions
None.
