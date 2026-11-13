# Contributing

Thanks for helping. This covers building, testing and sending changes. Each tool lives in its own folder with its own
`CMakeLists.txt`; the commands below are for **Media Tools** (`Video Compressor/`). Run them from the repo root, and
keep the quotes, because the folder name has a space in it.

## 1. Install the dependencies

To build: a C17 compiler, CMake ≥ 3.20, pkg-config and GTK 4 (with GLib).
To run: `ffmpeg` and `ffprobe` with libx265, ImageMagick 7 (the `magick` command) and `pngquant`. The app starts without
them and shows a banner naming whatever is missing.

**Fedora.** Fedora's own `ffmpeg-free` has no x265, so take ffmpeg from [RPM Fusion](https://rpmfusion.org/Configuration):

```sh
sudo dnf install gcc cmake pkgconf gtk4-devel ImageMagick pngquant
sudo dnf swap ffmpeg-free ffmpeg --allowerasing    # after enabling RPM Fusion free
```

**Debian / Ubuntu.** The package names are below. ImageMagick 7 (`magick`) is only packaged in recent releases; on older
ones Compress images stays disabled.

```sh
sudo apt install build-essential cmake pkg-config libgtk-4-dev ffmpeg imagemagick pngquant
```

**Arch.**

```sh
sudo pacman -S base-devel cmake gtk4 ffmpeg imagemagick pngquant
```

## 2. Build, test, run

```sh
cmake -S "Video Compressor" -B "Video Compressor/build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "Video Compressor/build" -j
ctest --test-dir "Video Compressor/build" --output-on-failure
"Video Compressor/build/video-compressor"
```

`ctest` should finish with `100% tests passed, 0 tests failed out of 5`. The unit tests need no display and no video
files: all the decisions (which file is a video, which settings it gets, the exact ffmpeg arguments) live in GTK-free
code in `src/*.c` (the `vc_core` library) and are tested there. `src/app.h` and the UI files (`main.c` startup, `widgets.c` and `inspector.c` build the window, `row.c` and `status.c` draw it, `source.c` lists files, `jobs.c` runs encodes, `handlers.c` reacts to clicks, `style.c` holds the CSS) are the GTK window.

Debug builds use `-Werror`, so a new warning fails the build. Run the tests before every commit.

### Sample files

`tests/make-fixtures.sh` generates test videos and images (several resolutions, HDR, subtitles, a file name with quotes
and emoji, fake files that must be rejected). It takes about 45 seconds and needs ffmpeg with libx264, libx265, libvpx
and libopus, plus ImageMagick and `exiftool` (`perl-Image-ExifTool` on Fedora, `libimage-exiftool-perl` on Debian).

```sh
"Video Compressor/tests/make-fixtures.sh" "Video Compressor/build/fixtures"
```

Then open `build/fixtures` in the app with Choose Folder.

### Sanitizer build

Run this before sending anything that touches memory, strings or the job runner. It needs `libasan` and `libubsan`.

```sh
cmake -S "Video Compressor" -B "Video Compressor/build/asan" -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build "Video Compressor/build/asan" -j
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir "Video Compressor/build/asan" --output-on-failure
ASAN_OPTIONS=detect_leaks=0 "Video Compressor/build/asan/video-compressor"
```

Any `AddressSanitizer` or `runtime error` line in the output is a bug.

### Install locally

```sh
cmake -S "Video Compressor" -B "Video Compressor/build/release" --install-prefix ~/.local
cmake --build "Video Compressor/build/release" -j && cmake --install "Video Compressor/build/release" --strip
```

### Flatpak build

The manifest is `Video Compressor/packaging/io.github.Tharindu_Dasantha.MediaTools.json`. GNOME Builder opens it with
one click. From the command line, with [`org.flatpak.Builder`](https://flathub.org/apps/org.flatpak.Builder) installed
(keep the build directories under `$HOME`, because the builder's sandbox can't see `/tmp`):

```sh
cd "Video Compressor"
flatpak run --env=FLATPAK_USER_DIR=$HOME/.local/share/flatpak --command=flatpak-builder org.flatpak.Builder \
    --user --install --force-clean --repo=$HOME/mt-flatpak/repo $HOME/mt-flatpak/build \
    packaging/io.github.Tharindu_Dasantha.MediaTools.json
flatpak run io.github.Tharindu_Dasantha.MediaTools
```

The first build compiles ffmpeg, x265 and ImageMagick and takes a while; later builds are cached.

## 3. Code style

- C17 with GLib conventions: `snake_case`, a `vc_` prefix on public functions, `g_autofree` / `g_autoptr` for cleanup,
  and `GError **` for anything that can fail.
- GTK-style formatting: 4-space indent, a space before `(`, lines up to 100 characters.
- External tools always run through `GSubprocess` with an **argv array, never a shell string**.
- Never overwrite or change an original file. Output goes to a `.part` file that is renamed when finished.
- Comments explain *why*, not *what*.
- New logic goes into `vc_core` with a unit test in `tests/`, not into the UI files.

## 4. Sending a change

1. For anything bigger than a small fix, open an issue first so we can agree on the approach.
2. Branch from `main` and keep each pull request to one change.
3. Make sure the Debug build has no warnings and `ctest` passes. For risky changes, run the sanitizer build too.
4. Fill in the pull request template. A screenshot helps for UI changes.

## Where things are

```
<Tool>/
  src/          app code (app.h and its UI files are the GTK window; compress, modes, web_video, frames, images are testable logic)
  tests/        unit tests and make-fixtures.sh
  data/         desktop file, AppStream metainfo, icons, screenshots, embedded fonts
  packaging/    Flatpak manifest
  docs/         specs, design log and measurements, research notes
```

`docs/SPEC.md` is the source of truth for how Media Tools behaves, and `docs/LOG.md` explains why decisions were made,
with the measurements behind them.

## License

By contributing you agree that your work is released under the [GPL-3.0-or-later](LICENSE).
