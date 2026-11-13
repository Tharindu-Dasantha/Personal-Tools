# Roadmap: Open Source Release and One-Click Install

> Applies to every tool in this repo, now and later. Video Compressor (Media Tools) goes first.
> Goals: any Linux user can install a tool with one click, any developer can build and contribute, and the work holds up
> in a job interview. These tools are not a business. They stay open source.
> Market context for Video Compressor: [Video Compressor/docs/MARKET-RESEARCH.md](Video%20Compressor/docs/MARKET-RESEARCH.md).

**Status (2026-11-13):** Phase 0 is done (`b885e7f`). Phase 2: the store screenshots are done; the rest is manual by Flathub policy. Phase 1: CI workflows written (not pushed yet), sandbox checks
that need no dialog pass; the portal dialogs still need a run on the real desktop. Media Tools has its license, app ID, icon and metainfo, and a Flatpak manifest that builds,
installs and runs every mode inside the sandbox. Committed (`caa7b10`), not tagged yet. Next: tag
`media-tools-v0.1.0`, test the sandbox on the real desktop (Phase 1 list), then choose how updates reach users before
Flathub (Open decision 5).

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
  data/              <appid>.desktop, <appid>.metainfo.xml, icons/hicolor/scalable/apps/<appid>.svg, screenshots/
  packaging/         <appid>.json (Flatpak manifest), generated dependency sources
  docs/              SPEC, LOG, research: design history, kept out of the way of the code
```

## Blockers to fix before the first public release

- [x] **App ID** (2026-11-13): `io.github.Tharindu_Dasantha.MediaTools`, was `io.github.dasantha.VideoCompressor`.
      Flathub checks `io.github.<user>` against the GitHub account `Tharindu-Dasantha` (the hyphen becomes `_`).
      **The ID can't change after release**, because users would stop getting updates. Used in: the desktop file name,
      `data/resources.gresource.xml` (resource prefixes), `CMakeLists.txt` (`APP_ID`), the application ID in `src/main.c`.
      The old native install (`~/.local/share/applications/io.github.dasantha.VideoCompressor.desktop`) is stale.
- [x] **LICENSE** (2026-11-13): GPL-3.0-or-later, at the repo root, installed to `share/licenses/<appid>/`.
- [x] **Icon** (2026-11-13): `data/icons/hicolor/scalable/apps/<appid>.svg`, a video frame squeezed by two arrows in the
      app's accent blue. Still open: a symbolic version (optional for Flathub).
- [x] **AppStream metainfo** (2026-11-13): `data/<appid>.metainfo.xml` with name "Media Tools", summary, description,
      one screenshot, release 0.1.0, OARS rating, branding colours, homepage/bugtracker/VCS links, developer ID
      `io.github.tharindu_dasantha` (appstream rejects uppercase there). Passes `appstreamcli validate`; the only
      warning is that the screenshot URL 404s until tag `media-tools-v0.1.0` is pushed.
- [x] **`graphify-out/` is committed to git** (2026-11-13): ignored in the root `.gitignore` (any depth, so future
      tools too) and removed from the index; the local copy stays for graphify.
- [ ] **Repo name.** `Personal-Tools` reads as private. Consider renaming it (GitHub redirects the old URL).

## Phase 0: make the repo presentable (about a weekend)

- [x] Add `LICENSE`.
- [x] Root `README.md`: what the repo is, a table of tools, install instructions, build instructions.
- [x] `Video Compressor/README.md` with 3 screenshots (2026-11-13, Xvfb, generated test clips; the README says so):
      `data/screenshots/estimates.png`, `results.png`, `web-video-dark.png`. The metainfo uses `main.png` and
      `main-dark.png` (Phase 2).
- [x] `CONTRIBUTING.md`: dependencies (`gtk4-devel`, `cmake`, `ffmpeg` with libx265, `pngquant`, `ImageMagick`),
      the build and test commands from SPEC.md, the ASan debug build, and how to run the fixtures script.
- [x] Issue templates (bug, feature; GitHub issue forms in `.github/ISSUE_TEMPLATE/`) and a PR template.
- [x] Rename the app ID everywhere (see Blockers). The app name is "Media Tools".
- [x] Add the icon and metainfo, and have CMake install them (`share/icons/hicolor/...`, `share/metainfo/`).
- [x] Desktop file: `Exec=video-compressor` (not an absolute path), `Icon=<appid>`, name "Media Tools". It is no longer
      a CMake template (`.desktop.in`), and passes `desktop-file-validate`.
- [x] Clean up `.gitignore` (one root file: `graphify-out/`, `build/`, `build-*/`, `.flatpak-builder/`, `*.flatpak`).
      Moved SPEC, LOG, PERFORMANCE, TEST, HANDOFF, MARKET-RESEARCH, `research/` and `review/` into `docs/`.

**Done when:** a stranger can clone the repo, read the README, build the app and run its tests without asking anything.

## Phase 1: Flatpak builds and downloads (about a week)

- [x] `packaging/<appid>.json`, a Flatpak manifest on GNOME 51 (2026-11-13). Tools pinned to the versions the measurements
      used: ffmpeg 8.1.3, x265 4.1 (8+10-bit), x264 b35605ac, zimg 3.0.6, ImageMagick 7.1.2-34, pngquant 3.0.3 (Rust,
      offline crates from `packaging/gen-cargo-sources.py`). Installed size 58.5 MB, `.flatpak` bundle 16 MB.
      Build: `flatpak run --env=FLATPAK_USER_DIR=$HOME/.local/share/flatpak --command=flatpak-builder org.flatpak.Builder
      --user --install --force-clean --repo=<dir>/repo <dir>/build packaging/<appid>.json` (dirs under `$HOME`: the
      builder's sandbox can't see `/tmp`).
  - Modules: zimg, x264, x265, ffmpeg (`--enable-gpl`, x264, x265, libwebp, zimg, dav1d, VA-API), ImageMagick,
    pngquant, then the app. libwebp, libtiff, lcms2, libjpeg, libpng, dav1d and libva come from the GNOME SDK.
  - x265 4.1 sets two CMake policies to OLD, which CMake 4 refuses; a `sed` in the manifest deletes those lines
    (CMP0025 only matters for Apple's compiler; CMP0054's new behaviour is the safer one).
  - `flatpak-builder-lint`: the manifest passes except `appid-url-not-reachable` (the ID names a repo that doesn't
    exist, see Phase 2) and the screenshot 404 (fixed by the tag).
  - `finish-args`: `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11`, `--device=dri` (VA-API),
    `--filesystem=home` (shortcut: simplest way for folders and `compressed/` output to work; tightened in Phase 3),
    `--filesystem=/run/media` and `/media` (external drives).
- [ ] Test inside the sandbox, since this is where it can behave differently from a normal install:
  - [x] The app starts in the sandbox (2026-11-13, Xvfb): bundled ffmpeg, ImageMagick and pngquant are found (no
        missing-tool banner), and the embedded Inter font and icons load from the renamed resource path.
  - [x] Every mode's real commands run inside the sandbox on the fixtures (2026-11-13, a throwaway headless driver over
        `vc_core`): x265 incl. 10-bit HDR, the GPU encoder, web video incl. HDR tonemap, frames JPG/WebP (60 frames each),
        all image types (JPEG, PNG, alpha, BMP, TIFF, WebP, twins), unicode and quote characters in names; fake files fail
        the probe as they should.
  - [x] Installing from the `.flatpak` bundle file works; the GNOME runtime comes from Flathub automatically.
  - [ ] Choose Folder, Choose Files and drag-and-drop (go through the portals; can't be automated here)
  - [ ] Writing to `compressed/` from the UI, and cleaning up leftover `.part` files (the commands already write
        inside the sandbox, see above)
  - [x] Move to Trash (2026-11-13): `g_file_trash` from inside the sandbox (`flatpak run --command=gio … trash`) puts
        the file in the real Trash. Still worth one click-through in the app.
  - [ ] Show in Files
  - [ ] The GPU (VA-API) test at startup on AMD (Intel done: a hevc_vaapi encode works in the sandbox on this machine)
  - [ ] Run in Background, notifications and the suspend inhibitor
  - [ ] Phone metadata kept (HDR / 10-bit encodes already pass, above)
  - [x] Process priority (2026-11-13): the seccomp filter allows both; inside the sandbox `nice -n 10` gives nice 10
        and `ionice -c3` gives class idle, the same calls `jobs.c` makes for each encode.
- [x] CI (`.github/workflows/`, 2026-11-13; runs once pushed):
  - [x] `ci.yml`, on every PR and push to `main`: Debug build (`-Werror`), `ctest`, and an ASan/UBSan build with its
        own `ctest` (leak checks on), in `fedora:44`. A matrix over tool folders; add new tools there. The same steps
        pass locally in a `fedora:44` podman container.
  - [x] `flatpak.yml`: builds the bundle (`flatpak-github-actions` v6.8, image `gnome-51`) on PRs that touch
        `packaging/`, by hand, and on a tag `media-tools-v*`, where it also creates the GitHub Release and attaches
        `media-tools-x86_64.flatpak`. The manifest's `dir` source now skips the action's `flatpak_app`, `repo` and
        `.flatpak-builder` folders, which sit inside the copied checkout. Not run yet: the first real run is the first
        push.
- [x] CI badges in the README.
- [x] Commit (`65876e3`, `caa7b10`).
- [ ] Tag `media-tools-v0.1.0` (the metainfo screenshot URL points at that tag).
- [ ] Updates before Flathub: a bundle installed from a file never updates. See Open decision 5.

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
- [x] Replace the screenshot with a real one (2026-11-13): `main.png` (light, default) and `main-dark.png`, both in the
      metainfo. A 1000×700 window with six videos listed, captured by GNOME Shell 50 itself (`ScreenshotWindow` in a
      headless `gnome-shell --headless --wayland` session with fresh settings), so the rounded corners and shadow are real
      and transparent. Throwaway patch for the capture only: window size 1000×700 and a folder loaded at startup.
      Uses the generated test clips, so the estimates (−97 %) are far better than real footage would get; retake with
      real videos if a reviewer objects.
- [ ] **Everything below is yours to do by hand** (AI policy above): the Flathub manifest, the PR, and all replies.
- [ ] Before submitting: tag `media-tools-v0.1.0` and push it (screenshot URLs need it), finish the Phase 1 desktop
      tests, and let some history build up (tagged releases, commits over weeks).
- [ ] Submit a PR to `flathub/flathub`. Review can take weeks. Reviewers may question `--filesystem=home`: either explain
      why it's needed (batch processing of whole folders) or tighten it first.
- [ ] After acceptance, updates are PRs to the `flathub/<appid>` repo (Flathub's bot can open them for new tags).
- [ ] "Get it on Flathub" badge in the README.

**Done when:** the app installs from GNOME Software / KDE Discover with one click and updates itself.

## Phase 3+: step-by-step improvements (each its own release)

Ordered roughly by value. Each item is a release on its own.

- [ ] **Tighter sandbox:** use only the file chooser and document portals, and drop `--filesystem=home`.
- [x] **Split `src/main.c`** (2026-11-13): 4,470 lines became 11 files of 120 to 830 lines sharing `src/app.h`
      (only functions used across files are non-static). Done by a script that checks every original line lands
      in exactly one file; the sorted lines match the old file apart from declarations. Debug `-Werror`, ctest and
      the ASan/UBSan build pass, and the ASan app compressed a folder end to end in a headless GNOME Shell.
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

| # | Decision | Status |
|---|---|---|
| 1 | License | **Decided 2026-11-13: GPL-3.0-or-later.** |
| 2 | Final app name and ID | **Decided 2026-11-13: "Media Tools", `io.github.Tharindu_Dasantha.MediaTools`**, kept in this monorepo (a separate `MediaTools` repo was considered and dropped). |
| 3 | Rename the repo | Open. Recommendation: yes, to something public-facing (e.g. `linux-tools`). Renaming changes the metainfo URLs. |
| 4 | Sandbox for v1 | **Decided: `--filesystem=home`** plus `/run/media` and `/media`; portals only in Phase 3. |
| 5 | Updates before Flathub | **Deferred 2026-11-13:** v0.1.0 ships as a bundle only; revisit after the desktop tests. Option: a self-hosted Flatpak repo on GitHub Pages, built by CI on each tag and signed with your own GPG key. Users add it once and get updates in GNOME Software. Without it, the `.flatpak` bundle from Releases installs but never updates. |

## Unverified (check before relying on it)

- Sandbox behaviour on the real desktop: file chooser and drag-and-drop portals, the Trash portal, Show in Files,
  notifications, background running, process priority. Commands and startup were checked only on Xvfb.
- VA-API inside the Flatpak on AMD (only tested on this Intel machine).
- Flathub review times. The rules themselves were read from <https://docs.flathub.org> on 2026-11-13 (Phase 2).
