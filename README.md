# Personal Tools

Small, offline desktop apps for Linux, written in C with GTK 4. Each tool is its own app with its own install.
They are free and open source under the [GPL-3.0-or-later](LICENSE).

| Tool | What it does | Folder |
|---|---|---|
| **Media Tools** | Make videos and images smaller: H.265 video compression, web-ready MP4, video to frames, image compression | [Video Compressor/](Video%20Compressor/) |

![Media Tools after compressing a folder of videos](Video%20Compressor/data/screenshots/results.png)

## Install

The apps ship as [Flatpak](https://flatpak.org) bundles, which run on every major distro and bring their own ffmpeg,
so you don't need anything else installed.

1. Set up Flatpak and Flathub once, if your distro hasn't already (Fedora has; Ubuntu hasn't):
   <https://flathub.org/setup>
2. Download the `.flatpak` file for a tool from [Releases](https://github.com/Tharindu-Dasantha/Personal-Tools/releases).
3. Double-click it, or run `flatpak install --user <file>.flatpak`.

A Flathub listing (install from GNOME Software or KDE Discover, with automatic updates) is planned, see
[ROADMAP.md](ROADMAP.md).

## Build from source

Each tool builds with CMake. For Media Tools on Fedora:

```sh
sudo dnf install gcc cmake pkgconf gtk4-devel
cmake -S "Video Compressor" -B "Video Compressor/build"
cmake --build "Video Compressor/build" -j
ctest --test-dir "Video Compressor/build" --output-on-failure
"Video Compressor/build/video-compressor"
```

At runtime the app needs `ffmpeg` (with libx265), `ImageMagick` and `pngquant` on your `PATH`. The full setup, other
distros, the Flatpak build and the sanitizer build are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Contributing

Bug reports, ideas and pull requests are welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md).
