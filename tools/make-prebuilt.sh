#!/bin/sh
# Packs a built fakeroot into a relocatable tarball of the third-party
# dependencies, so a consumer does not have to build all sixteen of them.
#
#   tools/make-prebuilt.sh [build-dir] [output-dir]
#
# In: static archives, headers, pkg-config files, with absolute paths
# rewritten to be relocatable. Out: libimagedecoder and the CLI - this
# repo's own code, changing every commit, which shipping prebuilt would
# risk silently linking stale.
#
# Only valid for a matching platform AND C runtime: static archives bake in
# libc references, so build each tarball in the environment that consumes it.
set -eu

# Resolved once so the script works from any directory, not just the repo root.
ROOT=$(cd "$(dirname "$0")/.." && pwd)

BUILD_DIR=${1:-build}
OUT_DIR=${2:-dist}
FAKEROOT="$BUILD_DIR/fakeroot"

[ -d "$FAKEROOT/lib/pkgconfig" ] || {
  echo "no fakeroot at $FAKEROOT - build the superbuild first" >&2
  exit 1
}

# Named after the platform and exact dependency set, so a tarball can't be
# mistaken for one built from different pins.
triple=$(cmake -P "$ROOT/tools/platform.cmake")
pins=$(cmake -P "$ROOT/tools/dep-pins.cmake")
name="imagedecoder-deps-${triple}-${pins}"

echo "packing $name"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
root="$stage/$name"
mkdir -p "$root/lib/pkgconfig" "$root/include"

# Both suffixes: MSVC names a static library .lib, and globbing only *.a there
# packs nothing, producing an empty lib directory instead of an obvious error.
copied=0
for a in "$FAKEROOT"/lib/*.a "$FAKEROOT"/lib/*.lib; do
  [ -f "$a" ] || continue
  case $(basename "$a") in
    libimagedecoder.a|imagedecoder.lib) continue ;;
  esac
  cp "$a" "$root/lib/"
  copied=$((copied + 1))
done
if [ "$copied" -eq 0 ]; then
  echo "no static libraries in $FAKEROOT/lib - nothing to pack" >&2
  exit 1
fi
echo "  $copied archives"

# The target's own strip from the build, not the host's: for a cross build
# the host strip fails silently per archive member (exits 0 while reporting
# "Unable to recognise the architecture") - only visible as Android tarballs
# five times the expected size.
STRIP=$(sed -n 's/^CMAKE_STRIP:FILEPATH=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)
# Emscripten's toolchain names no strip, so CMake records the host's, which
# can't read wasm objects.
# The toolchain file is emsdk's upstream/emscripten/cmake/Modules/Platform/,
# and llvm-strip upstream/bin/.
toolchain=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)
case $toolchain in
  *Emscripten.cmake)
    STRIP="$(cd "$(dirname "$toolchain")/../../../.." && pwd)/bin/llvm-strip"
    ;;
esac
[ -n "${STRIP:-}" ] && [ -x "$STRIP" ] || STRIP=strip

before=$(du -sk "$root/lib" | cut -f1)
"$STRIP" --strip-debug "$root"/lib/*.a 2>/dev/null || true
after=$(du -sk "$root/lib" | cut -f1)
echo "  stripped with $STRIP: ${before}K -> ${after}K"

cp -R "$FAKEROOT"/include/. "$root/include/"
rm -rf "$root/include/imagedecoder"

# libheif bakes the plugin search path into its header. Dead value here
# (plugin loading is off, codecs are static), but still a build machine's
# path that shouldn't travel inside a published artifact.
if [ -f "$root/include/libheif/heif_version.h" ]; then
  sed -i.bak 's|^#define LIBHEIF_PLUGIN_DIRECTORY .*|#define LIBHEIF_PLUGIN_DIRECTORY ""|' \
    "$root/include/libheif/heif_version.h"
  rm -f "$root/include/libheif/heif_version.h.bak"
fi

# pkg-config files, made relocatable: every absolute path starts at the
# fakeroot, and pcfiledir (the .pc's own directory) covers prefix,
# exec_prefix, libdir and includedir in one substitution.
#
# On Windows the shell's $PWD (/d/a/imagedecoder) and CMake's native path
# (D:/a/imagedecoder) differ, so both are substituted.
abs=$(cd "$FAKEROOT" && pwd)
if command -v cygpath >/dev/null 2>&1; then
  abs_native=$(cygpath -m "$abs")
else
  abs_native=$abs
fi
for pc in "$FAKEROOT"/lib/pkgconfig/*.pc; do
  case $(basename "$pc") in
    imagedecoder.pc) continue ;;
  esac
  out="$root/lib/pkgconfig/$(basename "$pc")"
  sed -e "s|$abs_native|\${pcfiledir}/../..|g" \
      -e "s|$abs|\${pcfiledir}/../..|g" "$pc" > "$out"

  # A double -l - openjpeg does this to CMAKE_THREAD_LIBS_INIT - is invisible
  # until a consumer links and gets "cannot find -l-lpthread". Only appears
  # where pthread is a separate library, so a modern glibc build never sees it.
  if grep -qE -- '-l-' "$out"; then
    echo "$(basename "$pc") has a malformed link flag:" >&2
    grep -nE -- '-l-' "$out" >&2
    exit 1
  fi

  # A surviving absolute path is the build machine's and points nowhere on
  # the consumer's disk; catch it here rather than as a mystifying compiler
  # error.
  if grep -qE '^(prefix|exec_prefix|libdir|includedir)=([A-Za-z]:|/)' "$out"; then
    echo "$(basename "$pc") is not relocatable:" >&2
    grep -nE '^(prefix|exec_prefix|libdir|includedir)=' "$out" >&2
    exit 1
  fi
done

{
  echo "name:    $name"
  echo "built:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "platform: $triple"
  echo "pins:    $pins"
  echo
  echo "dependency versions:"
  # NASM_VERSION too: it's part of the pin hash despite not shipping here.
  (cd "$ROOT" && grep -HE 'GIT_TAG|set\(NASM_VERSION' cmake/*.cmake) |
    sed 's|cmake/||;s/: *GIT_TAG/ /;s/: *set(NASM_VERSION /  /;s/)$//' | sort
} > "$root/MANIFEST.txt"

mkdir -p "$OUT_DIR"
tar -C "$stage" -czf "$OUT_DIR/$name.tar.gz" "$name"
sha=$(cmake -E sha256sum "$OUT_DIR/$name.tar.gz" | cut -d' ' -f1)
echo "$sha  $name.tar.gz" > "$OUT_DIR/$name.tar.gz.sha256"

echo "  $OUT_DIR/$name.tar.gz"
echo "  $(du -h "$OUT_DIR/$name.tar.gz" | cut -f1)  sha256 $sha"
