#include "decoder_avif.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace imagedecoder {

namespace {

// Streamed-in size is unknown until complete; an upper bound for libavif's
// allocation checks.
constexpr uint64_t STREAMING_SIZE_HINT = (uint64_t)1 << 32;

void no_destroy(avifIO *) {}

[[noreturn]] void fail(const char *what, avifResult r, const avifDecoder *dec) {
  std::string msg = std::string(what) + ": " + avifResultToString(r);
  if (dec && dec->diag.error[0]) {
    msg += " (";
    msg += dec->diag.error;
    msg += ")";
  }
  throw std::runtime_error(msg);
}

// Scales samples of `depth` bits to the full 16-bit range, in place.
void widen(uint16_t *p, size_t n, uint32_t depth) {
  if (depth >= 16) {
    return;
  }
  const uint32_t max = (1u << depth) - 1;
  for (size_t i = 0; i < n; i++) {
    const uint32_t v = std::min<uint32_t>(p[i], max);
    p[i] = (uint16_t)((v * 65535u + max / 2) / max);
  }
}

bool fraction(const avifSignedFraction &f, double &out) {
  if (f.d == 0) {
    return false;
  }
  out = (double)f.n / f.d;
  return true;
}

bool fraction(const avifUnsignedFraction &f, double &out) {
  if (f.d == 0) {
    return false;
  }
  out = (double)f.n / f.d;
  return true;
}

} // namespace

AvifDecoder::~AvifDecoder() { close(); }

avifResult AvifDecoder::read(avifIO *io, uint32_t, uint64_t offset, size_t size,
                             avifROData *out) {
  const auto *self = (const AvifDecoder *)io->data;
  const size_t have = self->m_source.size();
  const bool complete = self->m_source.complete();
  if (offset > have) {
    return complete ? AVIF_RESULT_IO_ERROR : AVIF_RESULT_WAITING_ON_IO;
  }
  const size_t left = have - (size_t)offset;
  if (size > left) {
    if (!complete) {
      return AVIF_RESULT_WAITING_ON_IO;
    }
    size = left;
  }
  out->data = self->m_source.data() + offset;
  out->size = size;
  return AVIF_RESULT_OK;
}

bool AvifDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (!m_dec) {
    m_dec = avifDecoderCreate();
    if (!m_dec) {
      throw std::runtime_error("Failed to create AVIF decoder");
    }
    // As lenient as libheif was: a missing pixi or ispe shouldn't refuse it.
    m_dec->strictFlags = AVIF_STRICT_DISABLED;
    m_dec->allowProgressive = AVIF_TRUE;
    m_dec->allowIncremental = AVIF_TRUE;
    m_dec->maxThreads = m_threads;
    m_dec->ignoreXMP = AVIF_TRUE;
    // Colour and alpha only: the gain map's metadata comes regardless, and its
    // pixels from a decoder of their own - a bad map mustn't fail the image.
    m_dec->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA;

    m_io = {};
    m_io.destroy = no_destroy;
    m_io.read = &AvifDecoder::read;
    m_io.persistent = AVIF_FALSE; // The source grows, so it may move.
    m_io.data = this;
    avifDecoderSetIO(m_dec, &m_io);
  }
  m_io.sizeHint = m_source.complete() ? m_source.size() : STREAMING_SIZE_HINT;

  const avifResult r = avifDecoderParse(m_dec);
  if (r == AVIF_RESULT_WAITING_ON_IO) {
    if (m_source.complete()) {
      throw std::runtime_error("Truncated AVIF");
    }
    return false;
  }
  if (r != AVIF_RESULT_OK) {
    fail("Invalid AVIF", r, m_dec);
  }

  const avifImage *img = m_dec->image;
  m_progressive = m_dec->progressiveState == AVIF_PROGRESSIVE_STATE_ACTIVE;
  const uint32_t count =
      m_dec->imageCount > 0 ? (uint32_t)m_dec->imageCount : 1;
  m_frame_count = m_progressive ? 1 : checked_frame_count(count);
  if (m_frame_count > 1) {
    // n repetitions are n + 1 plays; 0 plays forever.
    const int reps = m_dec->repetitionCount;
    m_loop_count = reps < 0 ? 0 : (uint32_t)reps + 1;
  }

  m_coded_width = img->width;
  m_coded_height = img->height;
  m_depth = img->depth;
  m_format = img->yuvFormat;
  if (m_depth < 8 || m_depth > 16 || m_format == AVIF_PIXEL_FORMAT_NONE) {
    throw std::runtime_error("AVIF has no usable pixel format");
  }

  uint32_t width = img->width;
  uint32_t height = img->height;
  m_cropped = false;
  if (img->transformFlags & AVIF_TRANSFORM_CLAP) {
    avifCropRect rect;
    avifDiagnostics diag;
    if (avifCropRectFromCleanApertureBox(&rect, &img->clap, width, height,
                                         &diag) &&
        rect.width > 0 && rect.height > 0 && rect.x <= width - rect.width &&
        rect.y <= height - rect.height &&
        (rect.width != width || rect.height != height)) {
      m_crop = rect;
      m_cropped = true;
      width = rect.width;
      height = rect.height;
    }
  }
  // irot turns anticlockwise, then imir mirrors: axis 1 left-right, axis 0
  // top-bottom (a left-right mirror after a half turn).
  uint32_t quarter =
      (img->transformFlags & AVIF_TRANSFORM_IROT) ? (img->irot.angle & 3) : 0;
  const bool mirror = (img->transformFlags & AVIF_TRANSFORM_IMIR) != 0;
  if (mirror && img->imir.axis == 0) {
    quarter = (quarter + 2) & 3;
  }
  static const uint32_t kTurn[2][4] = {{1, 8, 3, 6}, {2, 7, 4, 5}};
  m_turn = kTurn[mirror ? 1 : 0][quarter];
  if (orientation_swaps_axes(m_turn)) {
    std::swap(width, height);
  }
  check_dimensions(width, height);
  if (m_frame_count > 1 &&
      (uint64_t)m_frame_count * m_coded_width * m_coded_height >
          MAX_ANIMATION_PIXELS) {
    throw std::runtime_error("AVIF animation is too large");
  }

  const bool alpha = m_dec->alphaPresent != 0;
  const uint32_t bits = m_depth > 8 ? 16 : 8;

  m_hdr_kind = hdr_kind_for_transfer((int)img->transferCharacteristics);
  m_primaries = (int)img->colorPrimaries;
  if (m_hdr_kind == HdrKind::None &&
      (int)img->transferCharacteristics == CICP_TRANSFER_LINEAR && bits > 8) {
    m_hdr_kind = HdrKind::Linear;
  }
  m_hdr_headroom = hdr_headroom_for(m_hdr_kind, 0.0f);
  const int matrix = (int)img->matrixCoefficients;

  read_metadata();
  if (m_frame_count == 1 && m_hdr_kind == HdrKind::None) {
    find_gainmap();
  }

  m_mono = m_format == AVIF_PIXEL_FORMAT_YUV400;
  m_yuv = false;
  uint32_t sub_w = 0;
  uint32_t sub_h = 0;
  // Planes only when they need no crop, turn or unpremultiplying.
  if (!m_mono && matrix > 0 && !img->alphaPremultiplied && !m_cropped &&
      m_turn == 1) {
    if (m_format == AVIF_PIXEL_FORMAT_YUV420) {
      m_yuv = true;
      sub_w = 1;
      sub_h = 1;
    } else if (m_format == AVIF_PIXEL_FORMAT_YUV422) {
      m_yuv = true;
      sub_w = 1;
    }
  }

  // The data decides the output; a profile of the wrong family is dropped.
  if (m_src_profile &&
      (cmsGetColorSpace(m_src_profile) == cmsSigGrayData) != m_mono) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }

  info = {
      .width = width,
      .height = height,
      .original_width = width,
      .original_height = height,
      .components = m_mono ? (alpha ? 2u : 1u) : (alpha ? 4u : 3u),
      .has_alpha = alpha,
      .color = m_yuv    ? ColorFamily::YUV
               : m_mono ? ColorFamily::Gray
                        : ColorFamily::RGB,
      .sample_type = SampleType::Integer,
      .bits = bits,
      .subsampling_w = sub_w,
      .subsampling_h = sub_h,
      .yuv_matrix = m_yuv ? matrix : 1,
      .full_range = img->yuvRange == AVIF_RANGE_FULL,
  };
  info.orientation = 1;

  m_has_info = true;
  return true;
}

BaseDecoder::StepResult AvifDecoder::decode_impl(const DecodeOptions &opts) {
  if (!read_header()) {
    return {};
  }
  if (m_complete) {
    return {true, 0, {}};
  }

  ImageInfo dec = info;
  if (dec.color == ColorFamily::YUV &&
      opts.output_mode != OutputMode::Original) {
    dec.color = ColorFamily::RGB;
    dec.components = dec.has_alpha ? 4 : 3;
    dec.subsampling_w = 0;
    dec.subsampling_h = 0;
    dec.full_range = true;
  }
  const bool planar = dec.color == ColorFamily::YUV;
  const size_t work_stride = layout_stride(dec, opts);

  m_io.sizeHint = m_source.complete() ? m_source.size() : STREAMING_SIZE_HINT;
  avifResult r = avifDecoderNextImage(m_dec);
  // An earlier progressive layer may be coded smaller: skipped, not shown.
  while (r == AVIF_RESULT_OK && m_progressive &&
         !matches_header(m_dec->image) &&
         m_dec->imageIndex + 1 < m_dec->imageCount) {
    r = avifDecoderNextImage(m_dec);
  }

  if (r == AVIF_RESULT_WAITING_ON_IO) {
    if (m_source.complete()) {
      throw std::runtime_error("Truncated AVIF");
    }
    // A grid's cells as they decode; rows map simply only when untransformed.
    const uint32_t rows =
        std::min(avifDecoderDecodedRowCount(m_dec), dec.original_height);
    if (planar || m_cropped || m_turn != 1 || rows <= m_rows ||
        !matches_header(m_dec->image)) {
      return {};
    }
    convert(dec, work_stride, rows);
    if (opts.srgb_output) {
      SrgbTransform(dec, m_src_profile, true)
          .apply(m_buffer.data(), work_stride, 0, rows);
    }
    finish_layout(m_buffer.data(), work_stride, dec, opts, 0, rows);
    m_emitted = true;
    const InvalidRect grown{0, m_rows, dec.original_width, rows - m_rows};
    m_rows = rows;
    return {false, 0, grown, work_stride};
  }
  if (r == AVIF_RESULT_NO_IMAGES_REMAINING) {
    m_complete = true;
    return {true, 0, {}};
  }
  if (r != AVIF_RESULT_OK && m_threads > 1 && !m_emitted) {
    // Once, from the top, on one thread; a real error recurs there.
    const int threads = 1;
    restart();
    m_threads = threads;
    return decode_impl(opts);
  }
  if (r != AVIF_RESULT_OK) {
    fail("Failed to decode AVIF", r, m_dec);
  }
  m_emitted = true;
  m_rows = 0;

  if (!m_progressive && m_advance_frame) {
    m_advance_frame = false;
    m_frame++;
  }

  const bool last = m_dec->imageIndex + 1 >= m_dec->imageCount;
  if (!matches_header(m_dec->image)) {
    throw std::runtime_error("AVIF frame does not match its header");
  }

  if (planar) {
    copy_planes(dec);
  } else {
    convert(dec, work_stride, dec.original_height);
    if (opts.srgb_output) {
      SrgbTransform(dec, m_src_profile, true)
          .apply(m_buffer.data(), work_stride, 0, dec.original_height);
    }
    finish_layout(m_buffer.data(), work_stride, dec, opts, 0,
                  dec.original_height);
  }

  if (m_progressive) {
    // An earlier layer: the whole picture, at lower quality.
    if (!last) {
      return {false, 0,
              InvalidRect{0, 0, dec.original_width, dec.original_height},
              work_stride};
    }
    m_complete = true;
    return {true, work_stride, {}};
  }

  if (m_frame_count > 1) {
    const double ms = m_dec->imageTiming.duration * 1000.0;
    m_duration_ms = ms >= MAX_FRAME_DURATION_MS ? MAX_FRAME_DURATION_MS
                    : ms > 0                    ? (uint32_t)ms
                                                : 0;
  }
  if (last) {
    m_complete = true;
    return {true, work_stride, {}};
  }
  m_advance_frame = true;
  return {false, work_stride, {}};
}

bool AvifDecoder::matches_header(const avifImage *img) const {
  if (!img || img->width != m_coded_width || img->height != m_coded_height ||
      img->depth != m_depth || img->yuvFormat != m_format ||
      !img->yuvPlanes[AVIF_CHAN_Y]) {
    return false;
  }
  const size_t sample = m_depth > 8 ? 2 : 1;
  if (img->yuvRowBytes[AVIF_CHAN_Y] < (size_t)img->width * sample ||
      (img->alphaPlane && img->alphaRowBytes < (size_t)img->width * sample)) {
    return false;
  }
  if (m_format != AVIF_PIXEL_FORMAT_YUV400) {
    const uint32_t shift = m_format == AVIF_PIXEL_FORMAT_YUV444 ? 0 : 1;
    const size_t chroma = (size_t)((img->width + shift) >> shift) * sample;
    for (int c = AVIF_CHAN_U; c <= AVIF_CHAN_V; c++) {
      if (!img->yuvPlanes[c] || img->yuvRowBytes[c] < chroma) {
        return false;
      }
    }
  }
  return true;
}

// The top [rows] of the current image into m_buffer as dec lays it out:
// straight in, or at native size first when it has to be cropped and turned
// (whole images only). The image already matches the header.
void AvifDecoder::convert(const ImageInfo &dec, size_t work_stride,
                          uint32_t rows) {
  const avifImage *img = m_dec->image;
  const bool direct = !m_cropped && m_turn == 1;
  // [rows] counts the image's own rows; a crop or turn needs all of them.
  if (!direct) {
    rows = img->height;
  }

  ImageInfo native = dec;
  native.width = native.original_width = img->width;
  native.height = native.original_height = img->height;
  const size_t native_stride = direct ? work_stride : row_bytes(native);
  uint8_t *dst;
  if (direct) {
    // Rows below [rows] keep what an earlier step left, or zeroes.
    m_buffer.resize(checked_buffer_bytes(work_stride, dec.original_height));
    dst = m_buffer.data();
  } else {
    m_native.resize(checked_buffer_bytes(native_stride, img->height));
    dst = m_native.data();
  }

  if (m_mono) {
    // Luma as stored, like libheif's monochrome; alpha interleaved after it,
    // opaque if this frame has none.
    const bool wide = dec.bits == 16;
    const size_t n = dec.components;
    const uint32_t opaque = wide ? 65535u : 255u;
    for (uint32_t y = 0; y < rows; y++) {
      const uint8_t *ys = img->yuvPlanes[AVIF_CHAN_Y] +
                          (size_t)img->yuvRowBytes[AVIF_CHAN_Y] * y;
      const uint8_t *as = dec.has_alpha && img->alphaPlane
                              ? img->alphaPlane + (size_t)img->alphaRowBytes * y
                              : nullptr;
      uint8_t *row = dst + native_stride * y;
      for (uint32_t x = 0; x < img->width; x++) {
        if (wide) {
          uint16_t *p = (uint16_t *)row + (size_t)x * n;
          p[0] = ((const uint16_t *)ys)[x];
          if (n == 2) {
            p[1] = as ? ((const uint16_t *)as)[x] : (uint16_t)opaque;
          }
        } else {
          uint8_t *p = row + (size_t)x * n;
          p[0] = ys[x];
          if (n == 2) {
            p[1] = as ? as[x] : (uint8_t)opaque;
          }
        }
      }
      if (wide) {
        // Opaque was already full scale; widening leaves it there.
        widen((uint16_t *)row, (size_t)img->width * n, img->depth);
      }
    }
    if (img->alphaPremultiplied && dec.has_alpha && img->alphaPlane) {
      for (uint32_t y = 0; y < rows; y++) {
        uint8_t *row = dst + native_stride * y;
        for (uint32_t x = 0; x < img->width; x++) {
          const uint32_t a =
              wide ? ((uint16_t *)row)[x * 2 + 1] : row[x * 2 + 1];
          if (a == 0 || a >= opaque) {
            continue;
          }
          if (wide) {
            uint16_t &g = ((uint16_t *)row)[x * 2];
            g = (uint16_t)std::min<uint32_t>((g * opaque + a / 2) / a, opaque);
          } else {
            uint8_t &g = row[x * 2];
            g = (uint8_t)std::min<uint32_t>((g * opaque + a / 2) / a, opaque);
          }
        }
      }
    }
  } else {
    // Only the decoded rows: a view keeps the rest of the planes unread.
    struct View {
      avifImage *img = avifImageCreateEmpty();
      ~View() {
        if (img) {
          avifImageDestroy(img);
        }
      }
    } view;
    const avifImage *src = img;
    if (rows < img->height) {
      const avifCropRect top = {0, 0, img->width, rows};
      if (!view.img ||
          avifImageSetViewRect(view.img, img, &top) != AVIF_RESULT_OK) {
        throw std::runtime_error("Failed to view AVIF rows");
      }
      src = view.img;
    }
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, src);
    rgb.format = dec.has_alpha ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
    rgb.depth = dec.bits;
    rgb.pixels = dst;
    rgb.rowBytes = (uint32_t)native_stride;
    // Straight alpha is what every layout holds; libavif unpremultiplies, and
    // fills alpha opaque for a frame without it.
    rgb.alphaPremultiplied = AVIF_FALSE;
    const avifResult r = avifImageYUVToRGB(src, &rgb);
    if (r != AVIF_RESULT_OK) {
      fail("Failed to convert AVIF", r, nullptr);
    }
  }

  if (direct) {
    return;
  }
  ImageInfo cropped = native;
  const size_t bpp = (size_t)dec.components * (dec.bits / 8);
  const uint8_t *src = m_native.data();
  if (m_cropped) {
    cropped.width = cropped.original_width = m_crop.width;
    cropped.height = cropped.original_height = m_crop.height;
    src += native_stride * m_crop.y + bpp * m_crop.x;
  }
  const std::vector<uint8_t> turned =
      orient_copy(src, native_stride, cropped, m_turn);
  const size_t row = row_bytes(dec);
  if (turned.size() < row * dec.original_height) {
    throw std::runtime_error("AVIF turn produced too little");
  }
  m_buffer.resize(checked_buffer_bytes(work_stride, dec.original_height));
  for (uint32_t y = 0; y < dec.original_height; y++) {
    memcpy(m_buffer.data() + work_stride * y, turned.data() + row * y, row);
  }
}

void AvifDecoder::copy_planes(const ImageInfo &dec) {
  const avifImage *img = m_dec->image;
  const int count = dec.has_alpha ? 4 : 3;
  const size_t bytes = dec.bits >> 3;

  const uint32_t pw = padded_width(dec);
  const uint32_t ph = padded_height(dec);
  const uint32_t cw = pw >> dec.subsampling_w;
  const uint32_t chh = ph >> dec.subsampling_h;

  checked_buffer_size(dec);
  m_buffer.assign(
      ((size_t)pw * ph * (dec.has_alpha ? 2 : 1) + 2 * (size_t)cw * chh) *
          bytes,
      0);

  uint8_t *dst = m_buffer.data();
  for (int p = 0; p < count; p++) {
    const bool sub = p == 1 || p == 2;
    const uint32_t w = sub ? cw : pw;
    const uint32_t h = sub ? chh : ph;
    const size_t plane = (size_t)w * h * bytes;

    const uint8_t *src = p == 3 ? img->alphaPlane : img->yuvPlanes[p];
    const size_t stride = p == 3 ? img->alphaRowBytes : img->yuvRowBytes[p];
    if (p == 3 && !src) {
      memset(dst, 0xff, plane);
      dst += plane;
      continue;
    }
    if (!src) {
      throw std::runtime_error("AVIF image is missing a plane");
    }

    const uint32_t have_w =
        sub ? (img->width + dec.subsampling_w) >> dec.subsampling_w
            : img->width;
    const uint32_t have_h =
        sub ? (img->height + dec.subsampling_h) >> dec.subsampling_h
            : img->height;
    const uint32_t take_w = std::min(have_w, w);
    const uint32_t take_h = std::min(have_h, h);

    for (uint32_t y = 0; y < h; y++) {
      uint8_t *row = dst + (size_t)w * bytes * y;
      memcpy(row, src + stride * std::min(y, take_h - 1),
             (size_t)take_w * bytes);
      for (uint32_t x = take_w; x < w; x++) {
        memcpy(row + (size_t)x * bytes, row + (size_t)(take_w - 1) * bytes,
               bytes);
      }
      if (bytes == 2) {
        widen((uint16_t *)row, w, img->depth);
      }
    }
    dst += plane;
  }
}

void AvifDecoder::read_metadata() {
  const avifImage *img = m_dec->image;
  if (img->icc.size > 0 && img->icc.size < (size_t)64 * 1024 * 1024) {
    m_src_profile =
        cmsOpenProfileFromMem(img->icc.data, (cmsUInt32Number)img->icc.size);
    const cmsColorSpaceSignature space =
        m_src_profile ? cmsGetColorSpace(m_src_profile) : cmsSigRgbData;
    if (m_src_profile && space != cmsSigRgbData && space != cmsSigGrayData) {
      cmsCloseProfile(m_src_profile);
      m_src_profile = nullptr;
    }
  }

  size_t offset = 0;
  if (img->exif.size > 0 &&
      avifGetExifTiffHeaderOffset(img->exif.data, img->exif.size, &offset) ==
          AVIF_RESULT_OK &&
      offset < img->exif.size) {
    m_exif.assign(img->exif.data + offset, img->exif.data + img->exif.size);
  }
}

// ISO 21496-1 metadata, parsed with the header; the map's pixels come with
// the image's decode.
void AvifDecoder::find_gainmap() {
  const avifGainMap *gm = m_dec->image->gainMap;
  // A crop would have to be scaled onto the map.
  if (!gm || m_cropped) {
    return;
  }
  GainmapData meta;
  double base, alternate;
  if (!fraction(gm->baseHdrHeadroom, base) ||
      !fraction(gm->alternateHdrHeadroom, alternate) || base > 64 ||
      alternate > 64 || base > alternate) {
    // An HDR base (map toward SDR) isn't handled.
    return;
  }
  meta.base_headroom = (float)base;
  meta.alternate_headroom = (float)alternate;
  for (int c = 0; c < 3; c++) {
    double min, max, gamma, base_offset, alt_offset;
    if (!fraction(gm->gainMapMin[c], min) ||
        !fraction(gm->gainMapMax[c], max) ||
        !fraction(gm->gainMapGamma[c], gamma) ||
        !fraction(gm->baseOffset[c], base_offset) ||
        !fraction(gm->alternateOffset[c], alt_offset)) {
      return;
    }
    // Stops of gain, bounded so exp2 stays a finite float.
    if (min > max || min < -64 || max > 64 || !(gamma > 0) || gamma > 1e6 ||
        std::fabs(base_offset) > 1e6 || std::fabs(alt_offset) > 1e6) {
      return;
    }
    meta.min_content_boost[c] = (float)std::exp2(min);
    meta.max_content_boost[c] = (float)std::exp2(max);
    meta.gamma[c] = (float)gamma;
    meta.offset_sdr[c] = (float)base_offset;
    meta.offset_hdr[c] = (float)alt_offset;
  }
  const float boost =
      std::max({meta.max_content_boost[0], meta.max_content_boost[1],
                meta.max_content_boost[2]});
  if (!(boost > 1.0f) || !std::isfinite(boost)) {
    return;
  }
  m_gainmap = std::move(meta);
  m_hdr_kind = HdrKind::Gainmap;
  m_hdr_headroom = std::log2(boost);
}

const GainmapData *AvifDecoder::gainmap() {
  if (m_hdr_kind != HdrKind::Gainmap || !m_dec) {
    return nullptr;
  }
  if (m_gainmap_read) {
    return m_gainmap.empty() ? nullptr : &m_gainmap;
  }
  const avifGainMap *gm = m_dec->image->gainMap;
  const avifImage *gi = gm ? gm->image : nullptr;
  // Decoded with the image; before that, alone by a decoder of its own.
  struct Own {
    avifDecoder *dec = nullptr;
    ~Own() {
      if (dec) {
        avifDecoderDestroy(dec);
      }
    }
  } own;
  if (!gi || !gi->yuvPlanes[AVIF_CHAN_Y]) {
    if (!m_source.complete()) {
      return nullptr;
    }
    own.dec = avifDecoderCreate();
    if (!own.dec) {
      return nullptr;
    }
    own.dec->strictFlags = AVIF_STRICT_DISABLED;
    own.dec->imageContentToDecode = AVIF_IMAGE_CONTENT_GAIN_MAP;
    if (avifDecoderSetIOMemory(own.dec, m_source.data(), m_source.size()) !=
            AVIF_RESULT_OK ||
        avifDecoderParse(own.dec) != AVIF_RESULT_OK ||
        avifDecoderNextImage(own.dec) != AVIF_RESULT_OK ||
        !own.dec->image->gainMap || !own.dec->image->gainMap->image) {
      m_gainmap_read = true;
      return nullptr;
    }
    gi = own.dec->image->gainMap->image;
  }
  m_gainmap_read = true;

  const uint32_t w = gi->width;
  const uint32_t h = gi->height;
  const avifImage *img = m_dec->image;
  const bool mono = gi->yuvFormat == AVIF_PIXEL_FORMAT_YUV400;
  const size_t sample = gi->depth > 8 ? 2 : 1;
  if (w == 0 || h == 0 || w > img->width || h > img->height || gi->depth < 8 ||
      gi->depth > 16 || !gi->yuvPlanes[AVIF_CHAN_Y] ||
      gi->yuvRowBytes[AVIF_CHAN_Y] < (size_t)w * sample ||
      (!mono && (!gi->yuvPlanes[AVIF_CHAN_U] || !gi->yuvPlanes[AVIF_CHAN_V]))) {
    return nullptr;
  }
  const uint32_t channels = mono ? 1 : 3;
  std::vector<uint8_t> pixels((size_t)w * h * channels);

  if (mono) {
    // Deeper maps are narrowed: the Kotlin side reads 8-bit gains.
    const uint32_t max = (1u << gi->depth) - 1;
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t *row =
          gi->yuvPlanes[AVIF_CHAN_Y] + (size_t)gi->yuvRowBytes[AVIF_CHAN_Y] * y;
      uint8_t *dst = pixels.data() + (size_t)w * y;
      if (gi->depth <= 8) {
        memcpy(dst, row, w);
        continue;
      }
      for (uint32_t x = 0; x < w; x++) {
        const uint32_t v = std::min<uint32_t>(((const uint16_t *)row)[x], max);
        dst[x] = (uint8_t)((v * 255 + max / 2) / max);
      }
    }
  } else {
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, gi);
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.depth = 8;
    rgb.pixels = pixels.data();
    rgb.rowBytes = w * 3;
    if (avifImageYUVToRGB(gi, &rgb) != AVIF_RESULT_OK) {
      return nullptr;
    }
  }

  ImageInfo layout{};
  layout.width = layout.original_width = w;
  layout.height = layout.original_height = h;
  layout.components = channels;
  layout.bits = 8;
  layout.color = mono ? ColorFamily::Gray : ColorFamily::RGB;
  if (m_turn != 1) {
    pixels = orient_copy(pixels.data(), (size_t)w * channels, layout, m_turn);
  }
  const bool swaps = orientation_swaps_axes(m_turn);
  m_gainmap.width = swaps ? h : w;
  m_gainmap.height = swaps ? w : h;
  m_gainmap.channels = channels;
  m_gainmap.pixels = std::move(pixels);
  return &m_gainmap;
}

void AvifDecoder::restart() {
  close();
  m_has_info = false;
  m_complete = false;
  m_progressive = false;
  m_advance_frame = false;
  m_rows = 0;
  m_emitted = false;
  m_cropped = false;
  m_turn = 1;
  m_native.clear();
  m_hdr_kind = HdrKind::None;
  m_hdr_headroom = 0.0f;
  m_gainmap = GainmapData{};
  m_gainmap_read = false;
  m_primaries = CICP_PRIMARIES_BT709;
  m_frame = 0;
  m_frame_count = 1;
  m_loop_count = 0;
  m_duration_ms = 0;
  m_buffer.clear();
  m_exif.clear();
}

void AvifDecoder::close() {
  if (m_dec) {
    avifDecoderDestroy(m_dec);
    m_dec = nullptr;
  }
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
}

} // namespace imagedecoder
