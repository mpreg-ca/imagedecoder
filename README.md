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

To decode part of a still, call `decode(Rect(...))` on a fully read decoder: it returns that region of the first frame (in oriented coordinates) and can be called again for region after region on the same decoder. `decode()` with no rect gives the whole first frame. How much work a region saves depends on the format:

| Format    | Region decoding                                                                 |
| --------- | ------------------------------------------------------------------------------- |
| JPEG      | skips columns and rows outside it (rows above are still entropy-decoded); progressive saves only the IDCT |
| JPEG 2000 | decodes only the code-blocks it covers                                          |
| TIFF      | reads only the tiles or strips it touches                                       |
| HEIF      | decodes only the grid tiles it touches                                          |
| PNG       | stops after its last row, keeps only its pixels; Adam7 decodes whole            |
| WebP      | stops after its last row, converts only the region; animations decode whole    |
| AVIF      | converts only the region                                                        |
| JPEG XL   | keeps and converts only the region                                              |
| GIF       | composites only the region                                                      |

`decode(rect, sampleSize)` also shrinks the result to `ceil(size / sampleSize)` as it decodes, so the region never exists at full size where the format allows. `decodeToBitmap(rect, sampleSize)` does the same straight into a premultiplied `Bitmap` (ARGB_8888, or RGBA_F16 for HDR):

```kotlin
ImageDecoder.open(inputStream).use { dec ->
    val tile = dec.decodeToBitmap(Rect(0, 0, 1024, 1024), sampleSize = 2)  // 512x512
    imageView.setImageBitmap(tile)
    val thumb = dec.decodeToBitmap(null, sampleSize = 8)  // whole image at 1/8
}
```

| Format    | Sampling                                                                        |
| --------- | ------------------------------------------------------------------------------- |
| JPEG      | scales by 1/2, 1/4 or 1/8 in the IDCT (as far as the region's origin is aligned), nearest pixels for the rest; rows streamed |
| WebP      | libwebp scales while decoding; lossless and alpha still decode a full-size plane |
| JPEG 2000 | drops whole resolution levels (as far as the origin is aligned), nearest pixels for the rest; rows streamed |
| PNG       | nearest pixels, rows streamed; Adam7 and APNG hold the region first             |
| TIFF      | nearest pixels, a tile row or 64 strip rows at a time                           |
| HEIF      | nearest pixels, a grid tile row (or 64 rows) at a time                          |
| AVIF      | nearest pixels, converted 64 rows at a time; the YUV planes are still whole     |
| JPEG XL   | nearest pixels from libjxl's region-sized output                                |
| GIF       | nearest pixels from the region-sized canvas                                     |

Peak memory for a 3000x12000 page at `sampleSize = 8`, against a plain decode: JPEG 282 → 9 MB, PNG 284 → 11 MB, TIFF 285 → 19 MB, JPEG 2000 566 → 29 MB, lossy WebP 280 → 10 MB.

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
