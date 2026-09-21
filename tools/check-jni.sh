#!/bin/sh
# Every native method Kotlin declares needs a matching exported symbol, and every
# constructor JNI looks up by signature string must match the compiled descriptor.
# Neither is checked by the compiler - both fail only at runtime, on first call.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
AAR=$(ls "$ROOT"/android/library/build/outputs/aar/*.aar 2>/dev/null | head -1)
SO=$(ls "$ROOT"/android/library/build/intermediates/cxx/*/*/obj/*/libimagedecoder.so 2>/dev/null | head -1)

if [ -z "$AAR" ] || [ -z "$SO" ]; then
  echo "build the library first: (cd android && ./gradlew :library:assembleRelease)" >&2
  exit 1
fi

NM=$(ls "$HOME"/android-sdk/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-nm 2>/dev/null | tail -1)
[ -n "$NM" ] || NM=nm

# Without javap, every lookup comes back empty and looks like a failed check
# rather than one that couldn't run.
command -v javap >/dev/null 2>&1 || {
  echo "javap is needed to read the compiled descriptors; install a JDK" >&2
  exit 1
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
unzip -q "$AAR" classes.jar && unzip -qo classes.jar

CLASSES=$(find . -name '*.class' | sed 's|^\./||;s|\.class$||;s|/|.|g')

# Kotlin's native methods, by name.
for c in $CLASSES; do javap -p -cp . "$c"; done \
  | grep -E '\bnative\b' | sed 's/.*native //;s/(.*//;s/.* //' | sort -u > declared.txt

# Exported JNI entry points, by method name.
"$NM" -D --defined-only "$SO" | grep -o 'Java_[A-Za-z0-9_]*' \
  | sed 's/.*_//' | sort -u > exported.txt

status=0
missing=$(comm -23 declared.txt exported.txt)
if [ -n "$missing" ]; then
  echo "declared in Kotlin but not exported by the .so:"
  echo "$missing" | sed 's/^/  /'
  status=1
fi

# Constructor signatures the JNI looks up, against the compiled descriptors.
for c in $CLASSES; do javap -p -s -cp . "$c"; done \
  | grep -A1 '(' | grep 'descriptor:' | sed 's/.*descriptor: //' | sort -u > descriptors.txt

python3 - "$ROOT/android/jni/imagedecoder_jni.cpp" descriptors.txt <<'PY' || status=1
import re, sys
jni, desc_file = sys.argv[1], sys.argv[2]
have = set(open(desc_file).read().split())
bad = []
for m in re.finditer(r'"<init>",?\s*((?:"[^"]*"\s*)+)\)', open(jni).read()):
    sig = ''.join(re.findall(r'"([^"]*)"', m.group(1)))
    if sig not in have:
        bad.append(sig)
if bad:
    print("JNI constructor signatures with no matching compiled descriptor:")
    for s in bad:
        print("  " + s)
    sys.exit(1)
PY

[ $status -eq 0 ] && echo "JNI bindings OK: $(wc -l < declared.txt) native methods, signatures match"
exit $status
