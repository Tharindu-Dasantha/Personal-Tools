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

# Odd size: web mode must still write even dimensions. 4:4:4, since x264 4:2:0 needs even ones.
echo "odd-853x479.mp4"
ff $(src 853x479 10) -c:v libx264 -preset veryfast -pix_fmt yuv444p -c:a aac odd-853x479.mp4

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

# Compress images (SPEC-image-compress.md). exiftool writes the EXIF tags magick can't set on new images.
echo "images/"
mkdir -p images
(
cd images
mg() { magick "$@"; }
# Photo-like: smooth plasma plus grain, so it compresses like a real photo, not like flat colour.
mg -seed 1 -size 2400x1600 plasma:tomato-steelblue -attenuate 0.6 +noise Gaussian -quality 95 photo.jpg
mg -seed 2 -size 1600x1200 plasma:green-yellow -attenuate 0.4 +noise Gaussian -quality 95 rotated.jpg
exiftool -q -overwrite_original -Orientation#=6 -GPSLatitude=6.9271 -GPSLatitudeRef=N \
         -GPSLongitude=79.8612 -GPSLongitudeRef=E rotated.jpg
mg -seed 3 -size 1001x667 plasma: -attenuate 0.4 +noise Gaussian -quality 95 odd-1001x667.jpg
# Flat UI colours and text, like a real screenshot (pngquant's best case); opaque 8-bit RGB.
mg -size 1440x900 xc:white -fill '#3584e4' -draw 'rectangle 0,0 1440,60' \
   -fill '#deddda' -draw 'rectangle 40,100 400,860' -fill '#26a269' -draw 'circle 700,500 700,400' \
   -fill '#e5a50a' -draw 'roundrectangle 900,300 1300,600 24,24' -fill black -pointsize 28 \
   -draw "text 460,140 'Settings'" -draw "text 460,200 'Some text in a screenshot'" \
   -alpha off -depth 8 screenshot.png
# Shapes on a transparent background, one of them half transparent.
mg -size 800x600 xc:none -fill 'rgba(230,97,0,0.6)' -draw 'circle 400,300 400,60' \
   -fill '#3584e4' -draw 'roundrectangle 100,100 300,250 20,20' -fill white -pointsize 48 \
   -draw "text 320,320 'Alpha'" PNG32:alpha.png
# 16-colour noise: pngquant saves under 5% and the webp comes out bigger, so nothing is kept.
mg -seed 8 -size 96x96 xc: +noise Random -colors 16 -strip PNG8:already-tiny.png
mg -seed 5 -size 1700x2200 plasma:white-gray -colorspace Gray -attenuate 0.3 +noise Gaussian \
   -compress None scan.tiff
mg -size 400x200 xc:none -fill '#e01b24' -draw 'roundrectangle 10,10 390,190 30,30' \
   -fill white -pointsize 60 -draw "text 90,125 'LOGO'" -define bmp:format=bmp4 BMP:logo.bmp
mg -seed 6 -size 1200x800 plasma:purple-orange -quality 95 banner.webp
mg -seed 7 -size 900x600 plasma:navy-gold -attenuate 0.4 +noise Gaussian -quality 95 \
   "it's a \"test\" \$HOME ünïcødé 📷.jpg"
# Twins: same stem, so each one's outputs carry its full name (twin.jpg.webp, twin.png.webp).
mg -seed 9 -size 800x600 plasma: -attenuate 0.4 +noise Gaussian -quality 95 twin.jpg
mg -size 800x600 xc:'#f6f5f4' -fill '#3584e4' -draw 'circle 400,300 400,100' -alpha off -depth 8 twin.png
mg -size 64x64 xc:red xc:blue -set delay 50 anim.gif
echo "this is text pretending to be an image" > fake.png
)

echo "fixtures written to $out"
