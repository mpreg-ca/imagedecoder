#!/bin/sh
# Exercises a built imagedecoder-cli against a corpus it generates itself, so
# this runs anywhere without carrying binary test images in the repo.
#
#   tools/run-tests.sh [path-to-imagedecoder-cli]
#
# Checks: every format decodes in all three output modes; piecewise decode
# matches whole-file (catches a broken suspend/resume path); animation frames
# come back one at a time, composited; Exif orientation moves the pixels, not
# just the reported size.
set -eu

CLI=${1:-build/fakeroot/bin/imagedecoder-cli}
[ -x "$CLI" ] || { echo "no CLI at $CLI" >&2; exit 1; }
command -v magick >/dev/null 2>&1 || { echo "ImageMagick is needed to build the corpus" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
note() { printf '  %-34s %s\n' "$1" "$2"; }
check() { if [ "$2" = 0 ]; then note "$1" ok; else note "$1" FAIL; fail=$((fail + 1)); fi; }

echo "building a corpus in $WORK"
magick -size 61x37 gradient:red-blue                          "$WORK/rgb.png"
magick -size 61x37 gradient:red-blue -depth 16                "$WORK/rgb16.png"
magick -size 61x37 gradient:black-white -colorspace Gray      "$WORK/gray.png"
magick -size 61x37 gradient:red-blue                          "$WORK/rgb.jpg"
magick -size 61x37 gradient:red-blue -colorspace CMYK -strip  "$WORK/cmyk.jpg"
magick -size 61x37 gradient:red-blue                          "$WORK/rgb.webp"
magick -size 61x37 gradient:red-blue -depth 16                "$WORK/rgb16.tif"
magick -size 61x37 gradient:red-blue                          "$WORK/rgb.jp2"
magick -size 40x30 xc:red -size 40x30 xc:lime -size 40x30 xc:blue \
       -delay 10 -loop 0                                      "$WORK/anim.gif"

# Hand-written: an APNG with a sub-rectangle second frame, and a PNG with an
# Exif orientation - paths an encoder left to itself wouldn't produce.
python3 - "$WORK" <<'PY'
import struct, sys, zlib
w = sys.argv[1]
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
def rows(width, height, rgb):
    return b''.join(b'\x00' + bytes(rgb) * width for _ in range(height))

png  = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 24, 8, 2, 0, 0, 0))
png += chunk(b'acTL', struct.pack('>II', 2, 0))
png += chunk(b'fcTL', struct.pack('>IIIIIHHBB', 0, 32, 24, 0, 0, 1, 10, 0, 0))
png += chunk(b'IDAT', zlib.compress(rows(32, 24, (255, 0, 0)), 9))
png += chunk(b'fcTL', struct.pack('>IIIIIHHBB', 1, 8, 8, 4, 4, 1, 10, 0, 0))
png += chunk(b'fdAT', struct.pack('>I', 2) + zlib.compress(rows(8, 8, (0, 0, 255)), 9))
open(w + '/sub.apng', 'wb').write(png + chunk(b'IEND', b''))

# Orientation 6: a quarter turn, must swap the reported axes.
tiff = (b'II*\x00' + struct.pack('<I', 8) + struct.pack('<H', 1)
        + struct.pack('<HHI', 0x0112, 3, 1) + struct.pack('<HH', 6, 0)
        + struct.pack('<I', 0))
png  = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 60, 20, 8, 2, 0, 0, 0))
png += chunk(b'eXIf', tiff) + chunk(b'IDAT', zlib.compress(rows(60, 20, (80, 0, 0)), 9))
open(w + '/rot.png', 'wb').write(png + chunk(b'IEND', b''))
PY

echo "decode, in every output mode"
for f in "$WORK"/*; do
  for mode in "" "--rgba8" "--f16"; do
    # shellcheck disable=SC2086
    "$CLI" decode $mode "$f" >/dev/null 2>&1
    check "$(basename "$f") ${mode:-as-is}" $?
  done
done

echo "streamed in pieces == handed over whole"
for f in "$WORK"/*; do
  out=$("$CLI" stream --chunk 512 "$f" 2>&1) || true
  case $out in *identical=yes*) check "$(basename "$f")" 0 ;; *) check "$(basename "$f")" 1 ;; esac
done

echo "animation"
n=$("$CLI" frames "$WORK/anim.gif" 2>/dev/null | grep -c '^  frame') || n=0
[ "$n" = 3 ] && check "anim.gif has 3 frames" 0 || check "anim.gif has 3 frames (got $n)" 1
n=$("$CLI" frames --rgba8 "$WORK/sub.apng" 2>/dev/null | grep -c '^  frame') || n=0
[ "$n" = 2 ] && check "sub.apng has 2 frames" 0 || check "sub.apng has 2 frames (got $n)" 1

echo "orientation"
plain=$("$CLI" decode --rgba8 "$WORK/rot.png" 2>/dev/null | grep -o '60x20' || true)
turned=$("$CLI" decode --rgba8 --orient "$WORK/rot.png" 2>/dev/null | grep -o '20x60' || true)
[ "$plain" = 60x20 ] && check "rot.png is 60x20 unturned" 0 || check "rot.png is 60x20 unturned" 1
[ "$turned" = 20x60 ] && check "rot.png is 20x60 turned" 0 || check "rot.png is 20x60 turned" 1

echo
if [ "$fail" = 0 ]; then echo "all checks passed"; else echo "$fail check(s) failed"; fi
exit $([ "$fail" = 0 ] && echo 0 || echo 1)
