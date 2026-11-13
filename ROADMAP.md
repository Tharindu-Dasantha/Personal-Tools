# Roadmap: Open Source Release and One-Click Install

> Applies to every tool in this repo, now and later. Video Compressor (Media Tools) goes first.
> Goals: any Linux user can install a tool with one click, any developer can build and contribute, and the work holds up
> in a job interview. These tools are not a business. They stay open source.
> Market context for Video Compressor: [Video Compressor/MARKET-RESEARCH.md](Video%20Compressor/MARKET-RESEARCH.md).

## Decision: Flatpak on Flathub

Flathub apps show up in the built-in app store on most desktop distros (GNOME Software, KDE Discover), with one
**Install** button and automatic updates. The Flatpak manifest also gives contributors a one-click build in GNOME Builder.

| Option | Verdict |
|---|---|
| **Flatpak + Flathub** | ✅ Primary. Works on every distro, updates automatically, and can bundle ffmpeg with x265, pngquant and ImageMagick, so users don't need RPM Fusion first. HandBrake ships x265 on Flathub the same way. |
| AppImage | ❌ Bundling GTK4 is painful, there are no auto-updates, and it often breaks across glibc versions. |
| Snap | ❌ Really only at home on Ubuntu. Uncommon for GTK apps. |
| Fedora COPR / AUR / .deb | Later and optional. Community packagers usually create these once a tool gets noticed. |

Caveat: Fedora and most GNOME distros have Flathub set up already. Ubuntu doesn't ship Flatpak, so users there run a
one-time setup first. Link the setup guide at <https://flathub.org/setup> in each README.

**The code is already sandbox-friendly.** Video Compressor uses GTK and GLib calls that work through the Flatpak portals:
`GtkFileLauncher` (Show in Files), `GNotification`, `gtk_application_inhibit` and `g_file_trash`. The only hard-coded
host path is `/dev/dri/renderD128` (`src/compress.c`), which needs `--device=dri`.

## Repo layout for current and future tools

- **Each tool is its own app**, with its own app ID, Flatpak manifest and Flathub listing. No launcher suite.
- One monorepo, with releases tagged per tool: `media-tools-v0.1.0`, `<tool>-vX.Y.Z`.

```
README.md            index of all tools, install badges, Flathub setup link
CONTRIBUTING.md      build, test, code style, how to send a PR
LICENSE
.github/
  workflows/         one CI that builds and tests every tool
  ISSUE_TEMPLATE/    bug report, feature request
<Tool>/
  README.md          what it does, screenshots, install, build
  CMakeLists.txt
  src/  tests/
  data/              <appid>.desktop.in, <appid>.metainfo.xml, icons/hicolor/scalable/apps/<appid>.svg
  packaging/         <appid>.json (Flatpak manifest)
  docs/              SPEC, LOG, research: design history, kept out of the way of the code
```

## Blockers to fix before the first public release

- [x] **App ID.** Now `io.github.Tharindu_Dasantha.MediaTools` (2026-11-13). It was `io.github.dasantha.VideoCompressor`, but the GitHub user is `Tharindu-Dasantha`. Flathub
      checks `io.github.<user>` against your GitHub account, so it must be `io.github.Tharindu_Dasantha.<AppName>`
      (the hyphen becomes `_`). **The ID can't change after release**, because users would stop getting updates.
      Used in: `data/*.desktop.in`, `data/resources.gresource.xml` (resource prefixes), `CMakeLists.txt`, the
      application ID in `src/main.c`.
- [x] **LICENSE.** GPL-3.0-or-later, at the repo root (2026-11-13). There was none, so the code isn't legally open source yet, and Flathub won't accept it.
- [x] **Icon.** Done: `data/icons/hicolor/scalable/apps/<appid>.svg` (no symbolic version yet). The desktop file used the generic `video-x-generic`. Flathub requires your own icon (scalable SVG, plus a
      symbolic version).
- [x] **AppStream metainfo** (`<appid>.metainfo.xml`): summary, description, screenshots, release notes, content rating
      (OARS), license, homepage. Flathub requires it, and app stores display it.
- [ ] **`graphify-out/` is committed to git.** Add it to `.gitignore` and remove it from git.
- [ ] **Repo name.** `Personal-Tools` reads as private. Consider renaming it (GitHub redirects the old URL).

## Phase 0: make the repo presentable (about a weekend)

- [x] Add `LICENSE`.
- [ ] Root `README.md`: what the repo is, a table of tools, install instructions, build instructions.
- [ ] `Video Compressor/README.md` with 2–3 screenshots.
- [ ] `CONTRIBUTING.md`: dependencies (`gtk4-devel`, `cmake`, `ffmpeg` with libx265, `pngquant`, `ImageMagick`),
      the build and test commands from SPEC.md, the ASan debug build, and how to run the fixtures script.
- [ ] Issue templates (bug, feature) and a PR template.
- [x] Rename the app ID everywhere (see Blockers). The app name is "Media Tools".
- [x] Add the icon and metainfo, and have CMake install them (`share/icons/hicolor/...`, `share/metainfo/`).
- [x] Desktop file: `Exec=video-compressor` (not an absolute path), `Icon=<appid>`.
- [ ] Clean up `.gitignore`. Move SPEC, LOG, PERFORMANCE, TEST, HANDOFF, `research/` and `review/` into `docs/`.

**Done when:** a stranger can clone the repo, read the README, build the app and run its tests without asking anything.

## Phase 1: Flatpak builds and downloads (about a week)

- [x] `packaging/<appid>.json`, a Flatpak manifest on GNOME 51 (2026-11-13). Tools pinned to the versions the measurements
      used: ffmpeg 8.1.3, x265 4.1 (8+10-bit), x264 b35605ac, zimg 3.0.6, ImageMagick 7.1.2-34, pngquant 3.0.3 (Rust,
      offline crates from `packaging/gen-cargo-sources.py`). Installed size 58.5 MB, `.flatpak` bundle 16 MB.
      Build: `flatpak run --env=FLATPAK_USER_DIR=$HOME/.local/share/flatpak --command=flatpak-builder org.flatpak.Builder
      --user --install --force-clean --repo=<dir>/repo <dir>/build packaging/<appid>.json` (dirs under `$HOME`: the
      builder's sandbox can't see `/tmp`).
  - Modules: x265, then ffmpeg (`--enable-libx265 --enable-gpl`, plus VA-API), pngquant, ImageMagick, then the app.
    Use HandBrake's Flathub manifest as the reference for x265 and ffmpeg.
  - `finish-args`: `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11`, `--device=dri` (VA-API),
    `--filesystem=home` (shortcut: simplest way for folders and `compressed/` output to work; tightened in Phase 3),
    `--filesystem=/run/media` and `/media` (external drives).
- [ ] Test inside the sandbox, since this is where it can behave differently from a normal install:
  - [ ] Choose Folder, Choose Files and drag-and-drop
  - [x] Every mode's real commands run inside the sandbox on the fixtures (2026-11-13, headless driver over `vc_core`):
        x265 incl. 10-bit HDR, web video incl. HDR tonemap, frames JPG/WebP, all image types, unicode names
  - [ ] Writing to `compressed/`, and cleaning up leftover `.part` files
  - [ ] Move to Trash (should go through the Trash portal)
  - [ ] Show in Files
  - [ ] The GPU (VA-API) test at startup, on Intel and AMD (a hevc_vaapi encode works in the sandbox on this Intel machine)
  - [ ] Run in Background, notifications and the suspend inhibitor
  - [ ] HDR / 10-bit fixtures, phone metadata kept
  - [ ] Process priority (`setpriority` and `ioprio_set` in `main.c`) still works
- [ ] CI (`.github/workflows/`):
  - [ ] On every PR: CMake build, `ctest`, and an ASan/UBSan build (Fedora container).
  - [ ] On a tag `<tool>-v*`: build the Flatpak with `flatpak/flatpak-github-actions` and attach the `.flatpak`
        bundle to the GitHub Release.
- [ ] CI badges in the README.

**Done when:** a user downloads the `.flatpak` from GitHub Releases, double-clicks it, and the app installs and works with
nothing else installed.

## Phase 2: Flathub, the true one-click install

- [x] Read the current Flathub submission requirements (2026-11-13, <https://docs.flathub.org/docs/for-app-authors/requirements>):
  - **Generative AI policy:** Flathub manifests must not contain AI-generated or AI-assisted content, even if disclosed.
    AI tools must not open the submission PR or write its commits, description or replies. AI-generated app code must
    be disclosed (which parts, roughly how much), and reviewers decide case by case. Breaking this can mean a permanent ban.
    So: **you write the Flathub manifest and the PR yourself.** `packaging/` (written with Claude) is for the
    self-hosted build only; use it as a reference, not as the Flathub submission.
  - **Development history:** apps that have existed only a very short time are generally not accepted. Show tagged
    releases, sustained commits and real use first.
  - **App ID vs. repo:** for `io.github.*` the last part should match the repo (`.../Personal-Tools`). A monorepo ID
    like `MediaTools` needs a reviewer exception, which the rules allow. Ask in the PR.
  - Screenshot URLs must point at a tag or commit, not a branch. The metainfo and desktop file need complete English.
- [ ] Submit a PR to `flathub/flathub`. Review can take weeks. Reviewers may question `--filesystem=home`: either explain
      why it's needed (batch processing of whole folders) or tighten it first.
- [ ] After acceptance, updates are PRs to the `flathub/<appid>` repo (Flathub's bot can open them for new tags).
- [ ] "Get it on Flathub" badge in the README.

**Done when:** the app installs from GNOME Software / KDE Discover with one click and updates itself.

## Phase 3+: step-by-step improvements (each its own release)

Ordered roughly by value. Each item is a release on its own.

- [ ] **Tighter sandbox:** use only the file chooser and document portals, and drop `--filesystem=home`.
- [ ] **Split `src/main.c`** (about 4,470 lines) into per-mode and per-widget files. Contributors won't touch a file that size.
- [ ] **Libadwaita look** and the GNOME Human Interface Guidelines.
- [ ] **Translations** (gettext and a `po/` folder), so contributors can add languages.
- [ ] **Apply to GNOME Circle** once the two items above are done. It's the strongest credibility signal on Linux.
- [ ] **AV1 option** (SVT-AV1): smaller files than H.265.
- [ ] **NVIDIA encoding** (NVENC). Today only VA-API (Intel, possibly AMD) is used.
- [ ] **Target size mode** ("fit under 10 MB", like Constrict).
- [ ] **AVIF output** for images.
- [ ] **Smaller package:** replace ImageMagick with lighter tools where they're good enough.
- [ ] Optional: Fedora COPR, so Fedora users can `dnf install`.

## Template for new tools

Each new tool in this repo follows the same path:

1. Pick the app ID first (`io.github.Tharindu_Dasantha.<ToolName>`). It's permanent.
2. Start with: `README.md`, `data/` (desktop, metainfo, icon), `packaging/` (Flatpak manifest), tests.
3. Add it to the root CI and the root README table.
4. Tag `<tool>-v0.1.0`, then submit it to Flathub as its own app.

## Interview angle

What counts isn't the feature list. It's being able to show:
- a Flathub listing with real installs
- green CI badges, unit tests and sanitizer builds
- decisions backed by measurements (the VMAF numbers in LOG.md)
- a clean contribution path (README, CONTRIBUTING, issue templates, small PRs merged from strangers)

## Open decisions

| # | Decision | Recommendation |
|---|---|---|
| 1 | License | **GPL-3.0-or-later**: the GNOME-ecosystem default, and forks stay open. MIT if you want maximum reuse, including in closed-source software. |
| 2 | Final app name and ID | e.g. `io.github.Tharindu_Dasantha.MediaTools` |
| 3 | Rename the repo | Yes, to something public-facing (e.g. `linux-tools`) |
| 4 | Sandbox for v1 | `--filesystem=home` now, portals only in Phase 3 |

## Unverified (check before relying on it)

- Sandbox behaviour (Trash portal, VA-API inside Flatpak, background running): no Flatpak has been built yet.
- Flathub's exact rules, the app ID verification format, and review times come from memory. Check the current docs at
  <https://docs.flathub.org>.
