# Tests to run

Manual and command-line tests for Media Tools (`video-compressor`), with the result each one should give. Sections 1–2 are the
ones **only you can do**. The rest re-check the checkpoints on your real GNOME desktop; they already passed on Xvfb with ASan
(see [HANDOFF.md](HANDOFF.md), "Verified so far"), so a failure there means something differs on the real desktop.

Run every command from the repo root (the folder name has a space). Tick the box when the result matches.

## 0. Setup

```sh
cmake -S "Video Compressor" -B "Video Compressor/build" -DCMAKE_BUILD_TYPE=Debug
cmake --build "Video Compressor/build" -j
ctest --test-dir "Video Compressor/build" --output-on-failure
"Video Compressor/tests/make-fixtures.sh" "Video Compressor/build/fixtures"     # ~45 s
rm -rf ~/vc-test && cp -a "Video Compressor/build/fixtures" ~/vc-test && rm -rf ~/vc-test/compressed
cd ~/vc-test && sha256sum -- *.* images/*.* > ~/vc-test.sha256                 # to prove originals are untouched
```
- [ ] `ctest` prints `100% tests passed, 0 tests failed out of 5` (test_compress, test_modes, test_web_video, test_frames,
      test_images).
- [ ] `~/vc-test` holds 13 video-named files (12 real ones plus the decoy `fake.mp4`) and the decoy `notes.txt`.
- [ ] `~/vc-test/images` holds 14 files: 12 images, the decoys `anim.gif` and `fake.png` (the fixtures need `magick` and
      `exiftool`).

Use `~/vc-test` (not `build/`) for the Trash tests: the Trash only works on the same filesystem as your home.
Start the app with `"Video Compressor/build/video-compressor"`.

---

## 1. Compress video: tests that need your eyes

### 1.1 Real phone clip (SPEC.md criterion 4)
1. Put a real phone recording of **200 MB or more** in a new folder, e.g. `~/vc-phone/`.
2. Mode `Compress video`, Speed `Slow`, Move to Trash **off**. Choose the folder, note the row's `≈` estimate, press Start.
3. Rename `compressed/` to `compressed-slow/`, then repeat at Speed `Faster` (rename to `compressed-faster/`).
4. Play the original and each output side by side at normal size.

Expected:
- [ ] The `slow` output is **≤ 50 MB**.
- [ ] You can't tell either output from the original at normal viewing.
- [ ] Write the size, %, time and how far off the `≈` estimate was into the empty row of `tasks/plan.md` ("real 200 MB+ phone
      clip"). If the estimate is far off, `EST_VIDEO_KBPS` in `src/compress.c` needs recalibrating.

### 1.2 Real iPhone HDR clip (SPEC.md criterion 12)
1. Compress a real iPhone HDR (Dolby Vision/HLG) clip at `Slow`.
2. Check the tags:
   ```sh
   ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt,profile,color_transfer,color_primaries \
           -of default=nw=1 compressed/<name>.mp4
   ```
3. Play the original and the output side by side.

Expected:
- [ ] `pix_fmt=yuv420p10le`, `profile=Main 10`, `color_transfer=arib-std-b67`, `color_primaries=bt2020`.
- [ ] The row shows a `10-bit HDR` chip.
- [ ] The output is not washed out (grey, flat) compared with the original.

---

## 2. Web video: tests that need your eyes

### 2.1 No location in the output (SPEC-web-video.md criterion 5)
1. Mode `Web video (Safari)`, choose a folder with a real phone clip recorded with location on, press Start.
2. Compare the tags:
   ```sh
   ffprobe -v error -show_entries format_tags -of default=nw=1 <clip>.mov      | grep -i location
   ffprobe -v error -show_entries format_tags -of default=nw=1 web/<clip>.mp4  | grep -i location
   ```

Expected:
- [ ] The first command prints a `location` or `com.apple.quicktime.location.ISO6709` line (the source has GPS).
- [ ] The second prints **nothing**.

### 2.2 Plays in Safari on iOS before it has downloaded (criterion 6)
Python's `http.server` doesn't support byte ranges, and Safari won't play video without them, so use `http-server`.
1. In the `web/` folder from 2.1, write a test page and serve it:
   ```sh
   cd <folder>/web
   printf '<video src="%s" controls playsinline muted autoplay style="width:100%%"></video>\n' "<clip>.mp4" > index.html
   sudo firewall-cmd --add-port=8000/tcp          # temporary, gone after a reboot
   npx --yes http-server -p 8000
   ```
2. On the iPhone (same Wi-Fi), open `http://<laptop IP>:8000/` in Safari. Find the IP with `hostname -I`.
3. Press play if it hasn't started.

Expected:
- [ ] The video plays, with picture and sound (unmute it), and colours look normal.
- [ ] It starts within a second or two while the scrubber's buffered part is still growing. If it waits for the whole file,
      `+faststart` isn't working: run the check in 4.2 on that file.
- [ ] Optional: the same on a Mac in Safari.

Afterwards: stop the server (Ctrl+C), then `sudo firewall-cmd --remove-port=8000/tcp`.

### 2.3 Image quality against TinyJPG / TinyPNG (SPEC-image-compress.md criterion 6, task T27)

1. Put 5 of your real phone photos (JPEG) in `~/vc-photos/` and 5 real PNG screenshots or graphics in `~/vc-pngs/`.
2. Mode `Compress images`, Max width `Original width`, Move to Trash **off**. Run each folder.
3. Upload the same files to tinyjpg.com / tinypng.com and download the results.
4. Fill a row per file: `file | source size | ours (.jpg/.png) | Tiny* | difference %`.
5. Open ours and Tiny's side by side at 100% zoom.

- [ ] Each `.jpg` is within about 15% of TinyJPG's size, and you accept the look.
- [ ] Each `.png` is within about 15% of TinyPNG's size, and you accept the look.
- [ ] If not, say which way it is off; new values for 82 (JPEG) / 65-90 (pngquant) / 80 (WebP) are proposed and asked first.

Send the table: it goes into `tasks/plan.md`.

---

## 3. Compress video: re-run of SPEC.md criteria (Checkpoint 7)

Run these on `~/vc-test` in `Compress video` mode. Before each Start, choose the folder again (rows start fresh).

| # | Steps | Expected |
|---|---|---|
| 3.1 (crit. 1) | Choose Folder → `~/vc-test` | [ ] 13 rows, no `notes.txt`, no `compressed`. Quality chips: `240p.avi` 240p, `480p.webm` 480p, `720p.mp4` 720p, `1080p.mkv` 1080p, `portrait.mov` **1080p**. `fake.mp4` shows `Unreadable`. `hlg-10bit.mov` and `hdr10-pq.mov` show `10-bit HDR`. |
| 3.2 (crit. 2, 11) | Speed `Slow` (the default), Start. While it runs, drag the window, and run `ps -o args= -C ffmpeg \| grep -o -- '-preset [a-z]*'` | [ ] Rows encode one at a time into `~/vc-test/compressed/*.mp4`, the bars move smoothly, the window stays responsive. `odd-853x479.mp4` ends as **`Failed`**: known bug, see the note below the table. [ ] `ps` shows `-preset slow`. |
| 3.3 (crit. 3) | When done: `ls -l compressed/720p.mp4` and `ffmpeg -i compressed/720p.mp4 -i 720p.mp4 -lavfi ssim -f null - 2>&1 \| grep -o 'All:[0-9.]*'` | [ ] Output ≤ 15 MB (25% of 60.9 MB; it is about 1.6 MB). [ ] `All:` ≥ 0.95 (about 0.999). |
| 3.4 (crit. 6) | Look at the row for `it's a "test" $HOME ünïcødé 🎬.mp4` | [ ] Done, with a `✓ −N%` pill, and the same name exists in `compressed/`. |
| 3.5 (crit. 7) | Choose the folder again, Start | [ ] Every row ends as `Already compressed`; nothing is encoded. `lowbitrate.mp4` shows `Already efficient` (encoded, came out bigger, dropped) or `No saving · skipped` and `odd-853x479.mp4` fails again. |
| 3.6 (crit. 5) | `rm -rf compressed`, choose the folder, Speed `Slower`, Start. During `1080p.mkv` press Stop, then run `pgrep -x ffmpeg; ls -A compressed/; sha256sum -c ~/vc-test.sha256` | [ ] `pgrep` prints nothing (ffmpeg gone within 1 s). [ ] No `.part` file. [ ] Every line ends `OK`. |
| 3.7 (crit. 10) | `rm -rf compressed`, Keep subtitles **on**, Speed `Faster`, Start. Open `compressed/subs-srt.mkv` and `subs-movtext.mkv` in VLC (Subtitle → Track 1) | [ ] Both outputs are `.mkv` and show "Hello subtitles" at 0:01. |
| 3.8 (crit. 10) | `rm -rf compressed`, Keep subtitles **off**, Start, then `ffprobe -v error -select_streams s -show_entries stream=codec_name compressed/subs-srt.mp4` | [ ] Outputs are `.mp4`; `ffprobe` prints nothing (no subtitle stream). |
| 3.9 (crit. 12) | ffprobe `compressed/hlg-10bit.mp4` as in 1.2, and `compressed/720p.mp4` | [ ] HLG: `yuv420p10le`, `Main 10`, `arib-std-b67`, `bt2020`. [ ] 720p: `yuv420p` (8-bit stays 8-bit). |
| 3.10 (crit. 9) | `rm -rf compressed`, Move to Trash **on**, Speed `Faster`, Start | [ ] A dialog asks "Move up to 12 originals to Trash?" (11 if the size estimate already predicts no saving for `lowbitrate.mp4`; it then shows `No saving · skipped`); Cancel starts nothing. [ ] After "Compress and Move to Trash": 10 rows say the original is in Trash; `lowbitrate.mp4`, `odd-853x479.mp4` (failed) and `fake.mp4` stay in the folder. [ ] `gio trash --list \| grep -c vc-test` prints `10`. |
| 3.11 (crit. 9) | Restore only the test files (command below the table), then `sha256sum -c ~/vc-test.sha256` | [ ] All `OK`. |
| 3.12 (crit. 9) | Read-only folder: `mkdir -p ~/vc-ro/compressed && cp ~/vc-test/240p.avi ~/vc-ro/ && chmod a-w ~/vc-ro`. Choose `~/vc-ro`, Trash on, Start | [ ] Compressed, but the row says the original was kept (trash failed); `240p.avi` is still in `~/vc-ro`. Clean up: `chmod u+w ~/vc-ro && rm -rf ~/vc-ro` |
| 3.13 (crit. 9) | Failed output check: make a wrapper (below), start the app with it, Trash on, compress `240p.avi` | [ ] The row says the original was kept, with the reason in its tooltip; the output stays; the original is not in the Trash. |
| 3.14 (crit. 9) | Trash **off**, run any batch | [ ] No dialog, the Trash is empty, `sha256sum -c` all `OK`. |
| 3.15 (crit. 8) | `env PATH=/nonexistent "<repo>/Video Compressor/build/video-compressor"` | [ ] A dialog says ffmpeg and ffprobe must be installed; Close quits without a crash. |
| 3.16 (crit. 13) | The ASan run in section 6 | [ ] No sanitizer report. |

**Known bug (found 2026-11-10, not fixed):** Compress video can't encode a video with an odd width or height. x265 rejects
853×479 in 4:2:0 (`Picture width must be an integer multiple of the specified chroma subsampling`), so `odd-853x479.mp4` always
ends `Failed` with its original untouched. Web video handles it (4.3). A fix would make Compress video round odd sizes down to
even, which changes what it encodes, so it is ask-first.

Restore for 3.11. It restores only files that came from `~/vc-test`, so nothing else in your Trash is touched:
```sh
gio trash --list | awk -F'\t' -v d="$HOME/vc-test/" 'index($2, d) == 1 {print $1}' | xargs -r -d '\n' -n1 gio trash --restore
```

Wrapper for 3.13 (fails the output check on anything in `compressed/`):
```sh
mkdir -p ~/vc-wrap && cat > ~/vc-wrap/ffprobe <<'EOF'
#!/bin/sh
case "$*" in */compressed/*) exit 1;; esac
exec /usr/bin/ffprobe "$@"
EOF
chmod +x ~/vc-wrap/ffprobe
env PATH="$HOME/vc-wrap:$PATH" "<repo>/Video Compressor/build/video-compressor"
```

Note on criterion 8: since Phase 8, ffmpeg **without libx265** no longer stops the app. Compress video shows a banner instead
(SPEC-app-shell.md decision 6). Test 5.4 covers that.

---

## 4. Web video: re-run of SPEC-web-video.md criteria 1–4 and 7

`rm -rf ~/vc-test/web`, mode `Web video (Safari)`, choose `~/vc-test`, Max width **Max 1280 px**, Remove audio off, Start.

### 4.1 Format (criterion 1)
```sh
cd ~/vc-test/web
for f in *.mp4; do echo "$f | $(ffprobe -v error -select_streams v:0 -show_entries \
  stream=codec_name,profile,codec_tag_string,pix_fmt,width,height -of csv=p=0 "$f") | $(ffprobe -v error \
  -select_streams a -show_entries stream=codec_name,profile -of csv=p=0 "$f")"; done
```
- [ ] 12 files (no `fake.mp4`). Every line: `h264,High,avc1,yuv420p`, even width and height, audio `aac,LC`.
- [ ] Rows show `✓ −N%`; `lowbitrate.mp4` and `odd-853x479.mp4` show `✓ +N%` and are kept.
- [ ] No Trash switch, Trash button or size estimate is shown in this mode.

### 4.2 Fast start (criterion 2)
```sh
for f in *.mp4; do printf '%s: ' "$f"; ffmpeg -v trace -i "$f" 2>&1 | grep -m1 -oE "type:'(moov|mdat)'"; done
```
- [ ] Every line says `type:'moov'`.

### 4.3 Sizes (criterion 3)
- [ ] From 4.1: `1080p.mp4` is 1280×720, `720p.mp4` 1280×720, `portrait.mp4` **1080×1920**, `odd-853x479.mp4` 852×478.
- [ ] `rm web/odd-853x479.mp4 web/1080p.mp4`, choose the folder again, Max width **Original width**, Remove audio **on**, Start.
      `odd-853x479.mp4` is 852×478, `1080p.mp4` is 1920×1080, and both have no audio stream
      (`ffprobe -v error -select_streams a -show_entries stream=codec_name web/1080p.mp4` prints nothing).
      The other rows show `Already converted`.

### 4.4 HDR to SDR (criterion 4)
```sh
for f in hlg-10bit.mp4 hdr10-pq.mp4; do ffprobe -v error -select_streams v:0 -show_entries \
  stream=pix_fmt,color_transfer -of csv=p=0 "$f"; done
```
- [ ] Both print `yuv420p,bt709`.
- [ ] Open each next to its source in VLC: not washed out, not dark. (These are test patterns; 1.2/2.2 judge real footage.)

### 4.5 Stop, skip, close (criterion 7)
- [ ] `rm web/720p.mp4`, choose the folder again, Start. During `720p.mp4` press Stop: `pgrep -x ffmpeg` prints nothing within 1 s,
      `ls -A web/` has no `.part`.
- [ ] Start again: `720p.mp4` is converted, the others show `Already converted`.
- [ ] `rm web/720p.mp4`, Start, and close the window during `720p.mp4` → **Stop and Quit**: the app exits, `pgrep -x ffmpeg` is
      empty, no `.part` in `web/`.
- [ ] `sha256sum -c ~/vc-test.sha256` all `OK` (web mode never touches originals).

---

## 5. App shell (SPEC-app-shell.md)

| # | Steps | Expected |
|---|---|---|
| 5.1 (crit. 2) | Choose Files, pick one video from `~/vc-test` and one from another folder, Compress video, Start | [ ] Only those 2 rows; header `Chosen files · 2 files`. [ ] Each output is in its **own** folder's `compressed/`. |
| 5.2 | With the files listed, Choose Folder → `~/vc-test`; then Choose Files again | [ ] Each choice replaces the whole list (Files → Folder and Folder → Files). |
| 5.3 (crit. 3) | With `~/vc-test` listed, switch Mode to `Web video (Safari)` and back | [ ] The list reloads each time and every row starts fresh. The bottom bar swaps Speed/subtitles/Trash/estimate for Max width/Remove audio and back. The header column says `SIZE` in web mode, `SIZE → ESTIMATED` in compress mode. |
| 5.4 (crit. 4) | Start any batch | [ ] While it runs, the Mode dropdown is greyed and Choose Folder / Choose Files are hidden. Pausing doesn't unlock them. |
| 5.5 (crit. 5) | Start the app with an ffmpeg that hides libx264 (below), switch to Web video, choose `~/vc-test` | [ ] An orange banner: "Web video (Safari) needs ffmpeg with libx264…", Start greyed. [ ] Switch to Compress video: no banner, Start works. |
| 5.6 (crit. 7) | Section 0 and section 6 | [ ] ctest green, ASan run clean. |

Wrapper for 5.5:
```sh
mkdir -p ~/vc-nox264 && cat > ~/vc-nox264/ffmpeg <<'EOF'
#!/bin/sh
case " $* " in *" -encoders "*) /usr/bin/ffmpeg "$@" | grep -v ' libx264 ';; *) exec /usr/bin/ffmpeg "$@";; esac
EOF
chmod +x ~/vc-nox264/ffmpeg
env PATH="$HOME/vc-nox264:$PATH" "<repo>/Video Compressor/build/video-compressor"
```
App-shell criterion 6 (multi-step stop) is 5c.8. Criterion 5 with pngquant missing: start the app with
`env PATH="$HOME/vc-nopq" …` where `~/vc-nopq` holds only links to `ffmpeg`, `ffprobe` and `magick`
(`mkdir ~/vc-nopq && for t in ffmpeg ffprobe magick; do ln -s /usr/bin/$t ~/vc-nopq/; done`).
- [ ] Compress images shows "Compress images needs pngquant: sudo dnf install pngquant" with Start greyed; Compress video has
      no banner.

---

## 5b. Video → frames (SPEC-video-frames.md)

Switch Mode to `Video → frames` and choose `~/vc-test`. Delete `~/vc-test/frames` between runs (`rm -rf ~/vc-test/frames`), then
switch Mode away and back so the rows start fresh.

| # | Steps | Expected |
|---|---|---|
| 5b.1 (crit. 1) | Defaults (15 fps, WebP, q80, Max 1920 px), Start | [ ] Before Start, `720p.mp4` shows `≈ 900 frames`. [ ] `ls ~/vc-test/frames/720p \| wc -l` → 900 (±1), `frame_0001.webp` … `frame_0900.webp`. [ ] The row ends `900 frames · <size>`. |
| 5b.2 (crit. 2) | 12 fps, Start `10`, End `0:12`, Start | [ ] Rows show `≈ 24 frames` before Start. [ ] 24 files (±1) per video. [ ] `frame_0001` matches the source at 10 s (the counter in the test pattern reads `10`). |
| 5b.3 (crit. 3) | Max 640 px | [ ] `1080p` frames are 640×360, `portrait` 640×1138 (`ffprobe -v error -show_entries stream=width,height -of csv=p=0 <frame>`). |
| 5b.4 (crit. 4, 5) | Format JPG, q80; open `frames/1080p/frame_0001.jpg` and `frames/hlg-10bit/frame_0001.jpg` in Image Viewer next to the source paused in a player | [ ] Neither crushed nor washed out. **With a real iPhone HDR clip** (your eyes): frames look like the clip does on the phone. |
| 5b.5 (crit. 6) | Start, press Stop during a video | [ ] `ls -a ~/vc-test/frames` has no `.NAME.part` and no folder for that video. [ ] Start again extracts it. [ ] A mode switch and Start again shows `Already extracted` for every row. |
| 5b.6 (crit. 7) | Type `abc` in Start | [ ] The field turns red and Start greys. Clear it: both return to normal. |
| 5b.7 (crit. 7) | Start `15` (the `hlg-10bit.mov` and `hdr10-pq.mov` fixtures are 10 s) | [ ] Only those rows say `Starts after the end` (before and after Start); the rest extract. |
| 5b.8 | Close the window mid-extraction → Stop and Quit | [ ] No ffmpeg left (`pgrep -x ffmpeg`), no `.part` folder in `frames/`. |

---

## 5c. Compress images (SPEC-image-compress.md)

Switch Mode to `Compress images` and choose `~/vc-test/images`. Delete `~/vc-test/images/compressed` between runs, then switch
Mode away and back so the rows start fresh.

| # | Steps | Expected |
|---|---|---|
| 5c.1 (crit. 1) | Just list the folder | [ ] 13 rows (no `anim.gif`); `fake.png` shows `Unreadable`. Rows read e.g. `JPEG · 2400×1600`, size on the right. |
| 5c.2 (crit. 2) | Max width `Original width`, Trash off, Start | [ ] Every JPEG/PNG/BMP/TIFF gives its same-type (BMP → `.png`, TIFF → `.jpg`) plus `.webp`; `banner.webp` gives one `.webp`. [ ] The twins give `twin.jpg.jpg`, `twin.jpg.webp`, `twin.png.png`, `twin.png.webp`; every other output keeps the plain name. [ ] Rows read like `jpg 936 kB −78% · webp 1.0 MB −76%`. [ ] Every output opens in Image Viewer. |
| 5c.3 (crit. 3) | `magick identify -format '%f %wx%h %[orientation]\n' compressed/rotated.*` and `exiftool -GPS:all -Orientation compressed/*` | [ ] `rotated` outputs are 1200×1600 (upright), orientation `Undefined`, and exiftool prints no GPS or orientation for any output. |
| 5c.4 (crit. 4) | Open `compressed/alpha.png` and `alpha.webp` on a dark background | [ ] Both keep the transparent background and the half-transparent circle. |
| 5c.5 (crit. 5) | Look at `already-tiny.png` | [ ] `Skipped – already optimized`; no `compressed/already-tiny.*`. No output anywhere is bigger than its source. |
| 5c.6 (crit. 7) | Delete `compressed/`, Max width `Max 1280 px`, Start | [ ] `photo.jpg`/`.webp` are 1280×853, `odd-1001x667.*` stay 1001×667, `screenshot.png` → 1280×800. |
| 5c.7 (crit. 8) | Delete `compressed/`, Max 1280, Move to Trash **on**, Start → "Compress and Move to Trash" | [ ] Every row except `already-tiny.png` and `fake.png` says `Original in Trash`; those two stay. [ ] Restore only the test files (command under 3.11), then `cd ~/vc-test && sha256sum -c ~/vc-test.sha256`: all `OK`. |
| 5c.8 (crit. 9, app-shell 6) | Delete `compressed/`, Start, press Stop while `screenshot.png` runs (slow it down with the wrapper below) | [ ] `ls -A compressed` shows no `.screenshot.part.png` and no `.screenshot.mid.part.png`. [ ] Start again finishes it; a second Start shows `Already compressed` on every finished row. |
| 5c.9 | Close the window while an image is running → Stop and Quit | [ ] No `magick`/`pngquant` left, no hidden `.part` file in `compressed/`. |

Wrapper for 5c.8 and 5c.9 (each magick call waits 2 s): `mkdir -p ~/vc-slowimg && printf '#!/bin/sh\nsleep 2\nexec /usr/bin/magick "$@"\n' > ~/vc-slowimg/magick && chmod +x ~/vc-slowimg/magick`,
then start the app with `env PATH="$HOME/vc-slowimg:$PATH" …`.

---

## 6. Sanitizer run (SPEC.md criterion 13, app-shell criterion 7)

```sh
cmake -S "Video Compressor" -B "Video Compressor/build/asan" -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build "Video Compressor/build/asan" -j
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir "Video Compressor/build/asan"
ASAN_OPTIONS=detect_leaks=0 "Video Compressor/build/asan/video-compressor" 2> ~/vc-asan.log; echo "exit $?"
```
Run 3.2–3.6, 4.1–4.5, 5b.1–5b.8 and 5c.1–5c.9 in that window (including a mode switch with a folder loaded), then close it.
- [ ] ctest `100% tests passed`. [ ] `exit 0`. [ ] `grep -c 'AddressSanitizer\|runtime error' ~/vc-asan.log` prints `0`.

## Clean up
```sh
rm -rf ~/vc-test ~/vc-test.sha256 ~/vc-wrap ~/vc-nox264 ~/vc-nopq ~/vc-slowimg ~/vc-asan.log
```
Check no test files are left in the Trash: `gio trash --list | grep -c vc-` prints `0`.
