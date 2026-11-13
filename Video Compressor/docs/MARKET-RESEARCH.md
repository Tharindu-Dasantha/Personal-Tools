# Market Research: Video Compressor (Media Tools)

> Desk research, 2026-11-13. No user interviews and no download or search-volume data, so the estimates are judgment.
> Purpose: know where the app stands, what to copy from competitors, and what to protect. The app stays open source and
> is not a business. The release plan is in [../ROADMAP.md](../ROADMAP.md).

## What the app is

A GTK4 desktop app for Linux, written in C, that runs ffmpeg, pngquant and ImageMagick behind four modes:

1. **Compress video:** batch H.265 compression of a folder, with quality settings picked from each video's resolution.
2. **Web video (Safari):** one MP4 that plays everywhere, including Safari and iOS.
3. **Video → frames:** numbered image sequences for scroll animations.
4. **Compress images:** TinyPNG-style compression plus a `.webp` copy.

## Is there a need?

- **Shrinking videos, especially from phones:** the need is big. People run out of disk and cloud storage all the time.
  But most of them use Windows, macOS or their phone. Linux is roughly 4–5% of desktops, and Linux users who want a GUI
  for this mostly already have HandBrake.
- **Web video, frames and images:** web developers need these, but they solve it today with an ffmpeg one-liner,
  Squoosh, Cloudinary or ezgif.
- **The four modes serve two unrelated audiences.** A person cleaning up phone videos doesn't care about scroll-animation
  frames, and a web developer doesn't want a "Move originals to Trash" workflow. The README and store listing should
  lead with one audience.

## Competitors (all free)

| Our mode | Main competitors | What they do better |
|---|---|---|
| Compress video | **HandBrake**, **Shutter Encoder**, **Constrict** (GNOME Circle), **Unmanic / Tdarr** | HandBrake: runs on all three platforms, years of trust, AV1, NVIDIA/Intel/AMD GPU encoders, presets. Constrict: compresses to a target file size (e.g. under 10 MB for Discord), and it's on Flathub. Unmanic and Tdarr: watch a folder and convert a whole library to HEVC automatically. |
| Compress images | **Curtail** (GNOME Circle, updated May 2026), Caesium, Squoosh | Curtail: one-click Flathub install, lossless and lossy modes, SVG support. Squoosh: AVIF and a visual before/after comparison. |
| Web video | HandBrake's "Web Optimized" setting, ffmpeg | Nothing better as such, but it's a single checkbox in HandBrake. |
| Video → frames | Cloudinary, the Scrollsequence plugin, ffmpeg plus cwebp | They run in the browser or inside the tools web developers already use. |

## Where competitors beat us

1. **Distribution.** We have to be built from source, are installed in `~/.local/bin`, and only run on Fedora.
   Competitors are a one-click install from Flathub. → ROADMAP Phases 1–2.
2. **Install barrier.** We depend on the system ffmpeg having `libx265`. Fedora's default `ffmpeg-free` probably leaves
   out the x265 encoder (unverified), so users would need RPM Fusion first. → Fixed by bundling ffmpeg in the Flatpak.
3. **Codec.** H.265 only. AV1 (SVT-AV1) is where new size savings come from, and HandBrake already offers it.
4. **GPU encoding.** VA-API only (Intel, possibly AMD). No NVIDIA.
5. **Polish.** No translations, no Libadwaita look, no AppStream metadata. The custom UI won't look native next to
   GNOME Circle apps.
6. **Maintainability.** `src/main.c` is about 4,470 lines in one file. Python competitors (Constrict, Curtail) can add
   features faster.
7. **Missing features:** no target-size mode (Constrict), no AVIF (Squoosh), no automatic folder watching (Unmanic/Tdarr).

## Where we are better (protect these)

- **No settings, honest numbers.** Quality comes from a per-resolution table tuned with VMAF measurements. We show an
  estimated size before encoding, skip files predicted not to get smaller, and delete outputs that aren't at least 5%
  smaller. HandBrake makes you choose a preset and can produce a bigger file.
- **Safe deletion.** Originals go to the Trash only after the output passes checks: ffmpeg exited cleanly, the file was
  renamed atomically, it probes correctly and its duration matches. Nothing is deleted permanently. We know of no other
  GUI that does this.
- **Keeps phone metadata.** Recording date, GPS location, camera details and the file's modified time survive. HandBrake
  is commonly criticised for dropping these (unverified for its latest version).
- **HDR done right.** 10-bit kept, HLG/PQ colour tags carried over, the `hdr10` flag, and the `hvc1` tag so files play
  on Apple devices. iPhone HDR clips don't come out washed out.
- **Runs well alongside the desktop.** Pause and resume, run in background, prevents sleep during a batch, and ffmpeg
  runs at low CPU and disk priority.

**Positioning in one line:** *Shrink a folder of phone videos to a fraction of the size, keep dates and locations, and
safely clear out the originals.* No other app owns this.

## Verdict

- **As a personal tool:** worth it. It exists, it's tested, and it solves the problem.
- **As an open-source Linux app:** worth it once it's on Flathub and leads with the safe phone-library cleanup.
  Expect a few thousand installs at best.
- **As portfolio and interview material:** strong. The measured decisions, tests and sanitizer builds, plus a Flathub
  listing, show engineering judgment, not just features.
- **As a business:** out of scope by choice.

## Sources

- Constrict: [Flathub](https://flathub.org/en/apps/io.github.wartybix.Constrict) ·
  [GNOME Apps](https://apps.gnome.org/Constrict/) ·
  [Korben](https://korben.info/en/constrict-videos-target-file-size.html)
- Curtail: [GNOME Apps](https://apps.gnome.org/Curtail/) · [It's FOSS](https://itsfoss.com/curtail-image-compress) ·
  [Linux image compression toolkit 2026](https://www.starryhope.com/linux/compress-images-with-trimage)
- HandBrake: [Alternatives for Linux](https://alternativeto.net/software/handbrake/?platform=linux) ·
  [HandBrake on Ubuntu (2026)](https://oneuptime.com/blog/post/2026-03-02-how-to-transcode-video-with-handbrake-on-ubuntu/markdown)
- Shutter Encoder: [LinuxLinks](https://www.linuxlinks.com/shutter-encoder-video-converter-tool/) ·
  [Version history](https://videohelp.com/software/Shutter-Encoder/version-history)
- Tdarr / Unmanic: [Tdarr + Jellyfin 2026](https://jellywatch.app/blog/jellyfin-tdarr-automated-library-transcoding-2026) ·
  [Tdarr on Unraid](https://unraid.net/use-cases/tdarr)
- Frames: [Codrops: video to frame sequences](https://tympanus.net/codrops/2025/10/16/creating-smooth-scroll-synchronized-animation-for-optikka-from-html5-video-to-frame-sequences/) ·
  [Cloudinary video to image sequence](https://cloudinary.com/tools/video-to-image-sequence) ·
  [Scrollsequence](https://wordpress.org/plugins/scrollsequence/)
