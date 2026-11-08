# Spec: video-frames

Module of [CAPABILITIES.md](CAPABILITIES.md). Depends on `app-shell` ([SPEC-app-shell.md](SPEC-app-shell.md)).

## Objective

A `Video → frames` mode, like ezgif's video-to-jpg, that turns a video (or a part of it) into a numbered image sequence. Web
developers use these sequences for scroll-driven canvas animations (the "Apple product page" effect) and similar work.

**User stories**
- I pick `Video → frames`, choose a video, set the options and press Start. I get `frames/<name>/frame_0001.webp`,
  `frame_0002.webp`, …
- **FPS:** `5`, `10`, `12`, `15` (**default**), `24`, `30`, `Every frame` (source rate).
- **Start / End:** two text fields. Empty means the whole video. They accept `12`, `12.5`, `1:05`, `1:05.25` and `0:01:05`. An
  invalid value turns the field red and greys Start.
- **Max width:** the same dropdown as web video, default `1920`, downscale only.
- **Format:** `WebP` (**default**, smaller, works in every current browser including Safari 14+) or `JPG`.
- **Quality:** 1–100, default `80`.
- Before I start, each row shows the frame count I'll get, e.g. `≈ 150 frames`, and it updates when I change FPS or the range.
  When a row finishes it shows `150 frames · 9.8 MB`.

## Decisions
1. **Output is a folder per video**, `frames/<name without extension>/`. It is written as `frames/.<name>.part/` and renamed
   when ffmpeg exits 0. If `frames/<name>/` exists, the row says `Skipped – frames/<name>/ exists`. Two inputs with the same
   stem (`clip.mp4`, `clip.mov`): the second one is skipped by that rule.
2. **Numbering starts at `frame_0001`** with 4 digits. ffmpeg keeps counting past 9999 with more digits.
3. **The same range applies to every video in the batch.** A video shorter than Start is skipped
   (`Skipped – starts after the end`). An End past a video's length just runs to that video's end.
4. **Trim seeks before the input** (`-ss S -to E -i IN`, which is fast and frame-accurate on re-encode). Progress is measured
   against the range length.
5. **HDR → SDR:** the same tone-map chain as web video, so frames from iPhone HDR clips aren't washed out.
6. **JPG quality mapping:** 1–100 → mjpeg `-q:v` 31…2, `qv = round(2 + (100 − q) × 29 / 99)` (80 → 8). WebP passes `-quality q`
   straight through.
7. **Trash is not offered** in this mode (app-shell decision 5).
8. Out of scope (not chosen): ZIP of frames, a scroll-scrub MP4, a sample HTML/JS snippet, a frame manifest.

## Encode settings

```
ffmpeg -nostdin -y [-ss START] [-to END] -i IN -an -sn -dn
  -vf "[fps=F,]scale='min(iw,W)':-2[,TONEMAP]"
  WebP: -c:v libwebp -quality Q           JPG: -c:v mjpeg -q:v QV
  -progress pipe:1 -nostats
  frames/.NAME.part/frame_%04d.EXT
```
This was smoke-tested on 2026-11-08: `-ss 0.5 -to 1.5`, `fps=12`, width 640 and tone-mapped HLG gave 12 WebP files.

## Tech Stack, Commands, Code Style
Same as [SPEC.md](SPEC.md). New code: `src/frames.c`, called from `vc_mode_steps()`.

```c
/* "1:05.25" → 65.25; FALSE for anything else. Empty string → TRUE, *seconds = 0 */
gboolean   vc_parse_time (const char *text, double *seconds);
int        vc_frames_jpeg_qscale (int quality);                       /* 1..100 → 31..2 */
guint      vc_frames_estimate (double duration, double source_fps, const VcOptions *opt); /* 0 = past end */
GPtrArray *vc_frames_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                            char **skip_reason);
```
`VcProbe` gains `double fps` (from `avg_frame_rate`, such as `30000/1001`) for `Every frame` estimates.

## Project Structure (additions)
```
src/frames.c          → time parsing, estimate, output dir, ffmpeg argv
tests/test_frames.c   → unit tests
```

## Testing Strategy
- **Unit (`tests/test_frames.c`):**
  - `vc_parse_time()`: `""`, `12`, `12.5`, `1:05`, `1:05.25`, `0:01:05`, plus rejects `-1`, `1:75`, `abc`, `1::2`, `1:2:3:4`
  - `vc_frames_jpeg_qscale()` at 1, 80 and 100 (31, 8, 2), monotonic
  - `vc_frames_estimate()`: whole video, a range, End past the end, Start past the end (0), `Every frame` with `30000/1001`
  - argv for WebP and JPG, with and without a range, `Every frame` (no `fps=`), Original width, HDR tone-map present or absent
  - output dir naming, the `.part` dir, the existing-dir skip, and a filename with spaces/quotes/unicode
  - `avg_frame_rate` parsing in `vc_parse_probe()` (`30/1`, `30000/1001`, `0/0` → 0)
- **Manual (fixtures, Xvfb):** the criteria below.

## Boundaries
- **Always:** keep the shared rules · write only inside `frames/.NAME.part/` until the rename · remove the whole `.part` dir
  on stop or failure.
- **Ask first:** ZIP output · a scroll-scrub MP4 · per-video ranges · offering trash in this mode.
- **Never:** upscale · write frames anywhere except `frames/` · leave a partial frame folder named like a finished one.

## Success Criteria
1. `720p.mp4` (60 s) at 15 fps, WebP, q80, with no range gives `frames/720p/frame_0001.webp` … `frame_0900.webp` (±1 frame).
   The row showed `≈ 900 frames` beforehand.
2. Start `10`, End `0:12` at 12 fps gives 24 frames (±1), and the first frame matches the source at 10 s.
3. Max width 640 gives 640-wide frames for the 1080p fixture. Portrait 1080×1920 at 640 gives 640×1138.
4. JPG at q80 gives `.jpg` files that open in an image viewer with correct brightness (not crushed or washed out next to a
   screenshot of the source).
5. `hlg-10bit.mov` frames look neither washed out nor dark.
6. Stopping mid-extraction leaves no `frames/.NAME.part/` and no `frames/NAME/`. A re-run then extracts normally, and a
   second re-run skips it.
7. An invalid Start (`abc`) turns the field red and greys Start. Start past a video's length skips only that video.

## Open Questions
None.
