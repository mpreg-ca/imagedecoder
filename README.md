# imagedecoder

- Streaming
- Animation
- Colour management
- HDR
- EXIF/TIFF tags

## Formats

| Format    | Library           | Streaming                      | Animation             | Colour    | Exif | HDR                       | Also                     |
| --------- | ----------------- | ------------------------------ | --------------------- | --------- | ---- | ------------------------- | ------------------------ |
| PNG       | libpng            | scanlines, Adam7 passes        | APNG                  | ICC       | yes  |                           | 16-bit, palette, alpha   |
| JPEG      | libjpeg-turbo     | scanlines, progressive passes  |                       | ICC, CMYK | yes  | Ultra HDR gain map        | planar YUV output        |
| WebP      | libwebp           | scanlines                      | yes                   | ICC       | yes  |                           | lossless, alpha          |
| GIF       | giflib            | whole file                     | yes, with loop count  |           |      |                           | transparency             |
| AVIF      | libavif, dav1d    | progressive layers, grid cells | yes (image sequences) | ICC, CICP | yes  | PQ, HLG, linear, gain map | planar YUV output, alpha |
| HEIF      | libheif, libde265 | whole file                     | image sequences       | ICC, CICP | yes  | PQ, HLG, linear, gain map | planar YUV output, alpha |
| JPEG XL   | libjxl            | progressive passes             | yes                   | ICC       | yes  | PQ, HLG, linear           | alpha                    |
| TIFF      | libtiff           | whole file                     | first page only       | ICC       | yes  |                           | 8/16-bit, alpha          |
| JPEG 2000 | OpenJPEG          | whole file                     |                       | ICC       |      |                           | alpha                    |

## Android

```kotlin
dependencies {
    implementation("ca.mpreg:imagedecoder:<version>")
}
```

```kotlin
ImageDecoder.open(inputStream).use { dec ->
    val frame = dec.decodeNext()          // RGBA8, or RGBA half-float if dec.isHdr
    frame.use { upload(it.image, it.width, it.height) }
    while (dec.hasNext) dec.decodeNext().use { /* next animation frame */ }
}
```

Frame pixels live in native memory, not the Java heap: close each `Frame` once you have the pixels.

To decode while downloading, open with `complete = false`, then call `pushData(bytes)` as they arrive and `markComplete()` at the end. `decodeNext()` throws `NeedMoreDataException` until it can show something, and returns frames with `partial = true` (and a dirty rectangle) until the image is whole.

## Building

CMake 3.22+, Ninja, Meson (for dav1d), and a C++20 compiler. The superbuild fetches and builds every dependency into `build/fakeroot`:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

To download the dependencies prebuilt for these pins instead (a source build is the fallback if none is published):

```sh
cmake -S . -B build -G Ninja -DIMAGEDECODER_FETCH_PREBUILT_DEPS=ON
```

| Target               | How                                                                                                               |
| -------------------- | ----------------------------------------------------------------------------------------------------------------- |
| Host library and CLI | as above; the CLI is `build/fakeroot/bin/imagedecoder-cli`                                                        |
| Android AAR          | `cd android && ./gradlew :library:assembleRelease`                                                                |
| WebAssembly          | `emcmake cmake -S . -B build-wasm -G Ninja`, add `-DIMAGEDECODER_WASM_THREADS=OFF` for the single-threaded module |
| Meson subproject     | drop the checkout into `subprojects/`; `meson.build` wraps the CMake build                                        |
