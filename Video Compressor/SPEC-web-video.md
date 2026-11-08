# Spec: web-video

Module of [CAPABILITIES.md](CAPABILITIES.md). Depends on `app-shell` ([SPEC-app-shell.md](SPEC-app-shell.md)).

## Objective

A `Web video (Safari)` mode that turns any video into one MP4 that plays everywhere on the web, Safari and iOS included, and
starts playing before it has fully downloaded. It is for web developers putting a hero/background or content video on a site.

**User stories**
- I pick `Web video (Safari)`, choose videos, and press Start. Each one becomes `web/<name>.mp4`.
- I choose **Max width** (`Original`, `3840`, `2560`, `1920` (**default**), `1280`, `960`, `640`). Wider videos are scaled
  down to that width with the aspect ratio kept. Narrower ones are never scaled up.
- I tick **Remove audio** (off by default) for muted autoplay backgrounds, and the output has no audio track.
- An iPhone HDR clip comes out looking normal (not washed out or dark) in Safari, because it is converted to SDR.
- Each row shows the result size and the change against the source, e.g. `4.1 MB · −63%` (or `+12%`).

## Decisions
1. **H.264 MP4 only**, confirmed with the user. It is the one format every browser plays, so there is no WebM or HEVC.
2. **No "not smaller" rule.** The point is compatibility, not size. An HEVC source may come out bigger, and the output is
   kept anyway (the row shows `+N%`).
3. **Width means display width** (after rotation; ffmpeg auto-rotates). A portrait 1080×1920 clip at Max width 1280 stays
   1080×1920.
4. **Dimensions are always even** (yuv420p requires it): width `trunc(min(iw,W)/2)*2`, height `-2`. This also applies at
   `Original`, so a 853×479 source becomes 852×478.
5. **HDR → SDR.** When `vc_probe_is_hdr()` is true (transfer `arib-std-b67` or `smpte2084`), the video is tone-mapped with
   zscale + `tonemap=hable`. Input transfer, primaries and matrix are passed explicitly from the probe (`bt2020` when
   primaries/matrix are missing). Other 10-bit sources are just converted to 8-bit `yuv420p`.
6. **Metadata is stripped** (`-map_metadata -1 -map_chapters -1`). Phone clips carry GPS location, which shouldn't be published.
7. **Only the first video and first audio stream are kept.** Subtitles and data streams are dropped.
8. **Fixed quality:** `-crf 23 -preset slow`. No speed or quality control, until the user asks for one.
9. **Trash is not offered** in this mode (app-shell decision 5).
10. Frame rate is kept as it is.

## Encode settings

```
ffmpeg -nostdin -y -i IN
  -map 0:v:0 -map 0:a:0?                     (no audio map with Remove audio)
  -vf "scale='trunc(min(iw,W)/2)*2':-2[,TONEMAP],format=yuv420p"     (W = iw at Original)
  -c:v libx264 -profile:v high -preset slow -crf 23 -pix_fmt yuv420p
  -c:a aac -b:a 128k -ac 2                   (-an with Remove audio)
  -sn -dn -map_metadata -1 -map_chapters -1
  -movflags +faststart
  -progress pipe:1 -nostats
  web/.NAME.part.mp4
```
`TONEMAP` = `zscale=tin=T:pin=P:min=M:t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=hable:desat=0,
zscale=t=bt709:m=bt709:r=tv`. This chain was smoke-tested on 2026-11-08 with a 10-bit HLG clip: the output was `yuv420p`,
High profile, `bt709`.

Output: `web/<name>.mp4`, lowercase extension. If it already exists, the row says `Skipped – web/<name>.mp4 exists`.

## Tech Stack, Commands, Code Style
Same as [SPEC.md](SPEC.md). New code: `src/web_video.c`, called from `vc_mode_steps()`.

```c
/* one step; NULL + skip_reason when web/<name>.mp4 exists */
GPtrArray *vc_web_video_steps (const char *input, const VcProbe *probe, const VcOptions *opt,
                               char **skip_reason);
```

## Project Structure (additions)
```
src/web_video.c         → output path + ffmpeg argv for web mode
tests/test_web_video.c  → unit tests
tests/make-fixtures.sh  → + odd-853x479.mp4, hdr10-pq.mov (10-bit PQ)
```

## Testing Strategy
- **Unit (`tests/test_web_video.c`):**
  - argv for an 8-bit 1080p source at Max width 1920 / 1280 / Original: every element checked, scale expression correct
  - Remove audio gives `-an` and no `0:a` map; off gives `-c:a aac -b:a 128k -ac 2`
  - HLG and PQ probes produce the tone-map chain with the right `tin`; a 10-bit SDR probe and an 8-bit probe don't
  - missing primaries/matrix on an HDR probe fall back to `bt2020`
  - output path `clip.MOV` → `web/clip.mp4`, the existing-output skip, and a filename with spaces/quotes/unicode
- **Manual (fixtures, Xvfb):** run the mode on the fixture folder, then check each output with ffprobe (criteria below).
- **Manual (the user):** open outputs in Safari on an iPhone or Mac.

## Boundaries
- **Always:** keep the shared rules from SPEC.md and SPEC-app-shell.md · leave originals untouched.
- **Ask first:** adding WebM/HEVC outputs · a quality or speed control · offering trash in this mode.
- **Never:** upscale · keep GPS/location metadata · produce odd dimensions or a non-yuv420p file.

## Success Criteria
1. Every fixture video produces `web/<name>.mp4` where ffprobe shows video `h264`, profile `High`, `yuv420p`, codec tag
   `avc1`, even width and height, and audio `aac` (LC) or no audio with Remove audio.
2. `moov` comes before `mdat` (`ffmpeg -v trace -i out.mp4 2>&1 | grep -m2 -oE "type:'(moov|mdat)'"` prints `moov` first).
3. At Max width 1280, the 1080p fixture comes out 1280 wide, the 720p fixture keeps its 1280, and portrait keeps 1080×1920.
   `odd-853x479.mp4` at Original comes out 852×478.
4. `hlg-10bit.mov` and `hdr10-pq.mov` come out `yuv420p` with `color_transfer=bt709` and look neither washed out nor dark in a
   screenshot compared with the source.
5. ffprobe shows no `location`/`com.apple.quicktime.location.ISO6709` tag on outputs from a real phone clip.
6. A real web output plays in Safari on iOS and starts before it has fully downloaded (judged by the user).
7. Stop, re-run skip and close-mid-encode behave as in `Compress video`, with no `.part` left behind.

## Open Questions
None.
