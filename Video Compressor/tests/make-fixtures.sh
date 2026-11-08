#!/usr/bin/env bash
# Generates sample videos for manual/integration checks. Not committed.
# Usage: make-fixtures.sh <output-dir>
set -euo pipefail

out="${1:?usage: $0 <output-dir>}"
mkdir -p "$out"
cd "$out"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# testsrc + sine tone, `$1` = WxH, `$2` = seconds
src() { echo -f lavfi -i "testsrc=size=$1:rate=30:duration=$2" \
             -f lavfi -i "sine=frequency=440:duration=$2"; }

# CBR filler forces the real bitrate; testsrc alone compresses to almost nothing.
cbr8m=(-c:v libx264 -preset veryfast -b:v 8M -minrate 8M -maxrate 8M -bufsize 8M
       -x264-params nal-hrd=cbr -pix_fmt yuv420p)

echo "240p.avi";      ff $(src 426x240 60)  "${cbr8m[@]}" -c:a aac -b:a 128k 240p.avi
echo "480p.webm";     ff $(src 854x480 60)  -c:v libvpx -b:v 8M -minrate 8M -maxrate 8M \
                         -deadline realtime -cpu-used 8 -c:a libopus 480p.webm
echo "720p.mp4";      ff $(src 1280x720 60) "${cbr8m[@]}" -c:a aac -b:a 128k 720p.mp4
echo "1080p.mkv";     ff $(src 1920x1080 20) "${cbr8m[@]}" -c:a aac -b:a 128k 1080p.mkv
echo "portrait.mov";  ff $(src 1080x1920 20) "${cbr8m[@]}" -c:a aac -b:a 128k portrait.mov

name="it's a \"test\" \$HOME ünïcødé 🎬.mp4"
echo "$name";         ff $(src 640x360 10) "${cbr8m[@]}" -c:a aac "$name"

# setparams, not -color_* output flags: ffmpeg 8 prefers frame tags over those flags.
echo "hlg-10bit.mov"
ff $(src 1280x720 10) \
   -vf setparams=color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc,format=yuv422p10le \
   -c:v prores_ks -profile:v 2 -c:a aac hlg-10bit.mov

echo "hdr10-pq.mov"
ff $(src 1280x720 10) \
   -vf setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,format=yuv422p10le \
   -c:v prores_ks -profile:v 2 -c:a aac hdr10-pq.mov

# Already HEVC at a low bitrate: re-encoding cannot save 5%.
echo "lowbitrate.mp4"
ff $(src 426x240 10) -c:v libx265 -crf 40 -preset fast -x265-params log-level=error \
   -c:a aac -b:a 32k lowbitrate.mp4

subs=$(mktemp --suffix=.srt)
trap 'rm -f "$subs"' EXIT
printf '1\n00:00:01,000 --> 00:00:04,000\nHello subtitles\n\n2\n00:00:05,000 --> 00:00:09,000\nSecond line\n' > "$subs"
echo "subs-srt.mkv";      ff $(src 640x360 10) -i "$subs" -map 0 -map 1 -map 2 \
                             "${cbr8m[@]}" -c:a aac -c:s srt subs-srt.mkv
echo "subs-movtext.mp4";  ff $(src 640x360 10) -i "$subs" -map 0 -map 1 -map 2 \
                             "${cbr8m[@]}" -c:a aac -c:s mov_text subs-movtext.mp4

echo "decoys"
echo "not a video" > notes.txt
echo "this is text pretending to be a video" > fake.mp4
mkdir -p compressed

echo "fixtures written to $out"
