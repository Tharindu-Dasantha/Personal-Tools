# Capability Map: Media Tools

Approved 2026-11-08. The single GTK4 app in this folder, first built as "Video Compressor", grows into **Media Tools**:
one window with a **mode** dropdown. Each module below is one mode (or the shell that hosts them) and has its own spec.
This map is the index of what exists. Module ids are fixed; don't rename them.

| Module id | Responsibility | Depends on | Spec | State |
|---|---|---|---|---|
| `video-compress` | Folder of videos → H.265 copies in `compressed/`, opt-in trash of originals | — | [SPEC.md](SPEC.md) | Built (Phases 1–6) |
| `app-shell` | Mode dropdown, Choose Files, per-mode options bar, generic multi-step job runner, per-mode tool check | `video-compress` | [SPEC-app-shell.md](SPEC-app-shell.md) | Built (Phase 7) |
| `web-video` | Safari-ready H.264 MP4 in `web/`, max width, remove audio, HDR → SDR | `app-shell` | [SPEC-web-video.md](SPEC-web-video.md) | Built (Phase 8) |
| `video-frames` | Video → numbered JPG/WebP frames in `frames/<name>/` (fps, trim, width, quality) | `app-shell` | [SPEC-video-frames.md](SPEC-video-frames.md) | Built (Phase 9) |
| `image-compress` | Images → optimized same-type copy + `.webp` in `compressed/` (TinyPNG-style), opt-in trash | `app-shell` | [SPEC-image-compress.md](SPEC-image-compress.md) | Built (Phase 10); quality calibration (T27) open |

Build order: `app-shell` → `web-video` → `video-frames` → `image-compress`

- `app-shell` first turns today's app into the host and must leave `video-compress` working exactly as before.
- `web-video` is next because it uses the same ffmpeg + progress path, which makes it the cheapest proof that modes work.
- `image-compress` comes last. It is the only module with new tools (ImageMagick, pngquant) and no ffmpeg progress.

Rules shared by every module (one job at a time, argv only, `.part` then rename, never overwrite, trash only and never
permanently delete) live in [SPEC.md](SPEC.md) "Boundaries" and are restated per mode in
[SPEC-app-shell.md](SPEC-app-shell.md).

Decisions made with the user (2026-11-08): one app with a mode switch · input from a folder **or** chosen files · images get a
same-type copy plus `.webp` · pngquant may be installed · web video is H.264 MP4 only · web modes have a Max width dropdown
(downscale only) · web video has a Remove audio checkbox · frames are a JPG/WebP sequence with start/end trim (no ZIP, no
scroll-scrub MP4).
