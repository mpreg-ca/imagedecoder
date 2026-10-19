#include "decoder_base.h"

#include <algorithm>

namespace imagedecoder {

HdrKind hdr_kind_for_transfer(int transfer) {
  switch (transfer) {
  case CICP_TRANSFER_PQ:
    return HdrKind::PQ;
  case CICP_TRANSFER_HLG:
    return HdrKind::HLG;
  default:
    return HdrKind::None;
  }
}

float hdr_headroom_for(HdrKind kind, float peak_nits) {
  if (kind == HdrKind::None) {
    return 0.0f;
  }
  if (peak_nits > HDR_SDR_WHITE_NITS) {
    return std::log2(peak_nits / HDR_SDR_WHITE_NITS);
  }
  if (kind == HdrKind::PQ) {
    return std::log2(10000.0f / HDR_SDR_WHITE_NITS);
  }
  return std::log2(HDR_HLG_PEAK_NITS / HDR_SDR_WHITE_NITS);
}

cmsUInt32Number cms_format(const ImageInfo &info) {
  bool wide = info.bits == 16;
  if (info.color == ColorFamily::Gray && info.components == 1) {
    return wide ? TYPE_GRAY_16 : TYPE_GRAY_8;
  }
  if (info.color == ColorFamily::Gray && info.components == 2) {
    return wide ? TYPE_GRAYA_16 : TYPE_GRAYA_8;
  }
  if (info.color == ColorFamily::RGB) {
    if (info.components == 3) {
      return wide ? TYPE_RGB_16 : TYPE_RGB_8;
    }
    if (info.components == 4) {
      return wide ? TYPE_RGBA_16 : TYPE_RGBA_8;
    }
  }
  return 0;
}

void check_dimensions(uint64_t width, uint64_t height) {
  if (width == 0 || height == 0) {
    throw std::runtime_error("Image has no pixels");
  }
  if (width * height > MAX_IMAGE_PIXELS) {
    throw std::runtime_error("Image exceeds the maximum supported pixel count");
  }
}

size_t checked_buffer_size(const ImageInfo &info) {
  uint64_t bytes_per_pixel = (uint64_t)info.components * ((info.bits + 7) / 8);
  uint64_t pixels = (uint64_t)info.original_width * info.original_height;

  if (pixels == 0 || bytes_per_pixel == 0) {
    throw std::runtime_error("Image has no pixels");
  }
  if (pixels > MAX_IMAGE_PIXELS) {
    throw std::runtime_error("Image exceeds the maximum supported pixel count");
  }

  uint64_t total = pixels * bytes_per_pixel;
  if (total / pixels != bytes_per_pixel) {
    throw std::runtime_error("Pixel buffer size overflows");
  }
  if (total > MAX_IMAGE_BYTES || total > (uint64_t)SIZE_MAX) {
    throw std::runtime_error("Pixel buffer exceeds the maximum image size");
  }

  return (size_t)total;
}

size_t checked_buffer_bytes(size_t stride, uint32_t height) {
  if (stride == 0 || height == 0) {
    throw std::runtime_error("Image has no pixels");
  }
  uint64_t total = (uint64_t)stride * height;
  if (total > MAX_IMAGE_BYTES || total > (uint64_t)SIZE_MAX) {
    throw std::runtime_error("Pixel buffer exceeds the maximum image size");
  }
  return (size_t)total;
}

uint32_t checked_frame_count(uint64_t frames) {
  if (frames == 0) {
    return 1;
  }
  return frames > MAX_FRAMES ? MAX_FRAMES : (uint32_t)frames;
}

cmsHPROFILE create_srgb_gray() {
  cmsFloat64Number params[5] = {2.4, 1. / 1.055, 0.055 / 1.055, 1. / 12.92,
                                0.04045};
  cmsToneCurve *gamma = cmsBuildParametricToneCurve(NULL, 4, params);
  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsHPROFILE profile = cmsCreateGrayProfile(&d65, gamma);
  cmsFreeToneCurve(gamma);
  return profile;
}

void convert_rows_to_rgba8(uint8_t *base, size_t row_stride,
                           const ImageInfo &src, uint32_t y, uint32_t height) {
  const bool wide = src.bits == 16;
  const uint32_t comps = src.components;
  const bool gray = src.color == ColorFamily::Gray;
  const size_t src_pixel = (size_t)comps * (wide ? 2 : 1);
  const bool forward = 4 > src_pixel ? false : true;

  for (uint32_t i = 0; i < height; i++) {
    uint8_t *row = base + row_stride * (y + i);

    for (uint32_t k = 0; k < src.original_width; k++) {
      uint32_t x = forward ? k : src.original_width - 1 - k;
      const uint8_t *p = row + (size_t)x * src_pixel;

      uint8_t v[4]; // Read whole before writing back over it.
      for (uint32_t c = 0; c < 4; c++) {
        if (c < 3) {
          uint32_t idx = gray ? 0 : c;
          v[c] = wide ? p[idx * 2 + 1] : p[idx];
          continue;
        }
        if (!src.has_alpha) {
          v[3] = 255;
          continue;
        }
        uint32_t idx = comps - 1;
        v[3] = wide ? p[idx * 2 + 1] : p[idx];
      }

      uint8_t *o = row + (size_t)x * 4;
      o[0] = v[0];
      o[1] = v[1];
      o[2] = v[2];
      o[3] = v[3];
    }
  }
}

template <typename T>
void expand_rows_to_rgba_t(uint8_t *base, size_t src_row_stride,
                           size_t dst_row_stride, uint32_t width,
                           uint32_t components, bool gray, bool has_alpha,
                           uint32_t y, uint32_t height) {
  const T opaque = std::numeric_limits<T>::max();

  for (uint32_t i = height; i-- > 0;) {
    const T *src = (const T *)(base + src_row_stride * (y + i));
    T *dst = (T *)(base + dst_row_stride * (y + i));

    for (uint32_t x = width; x-- > 0;) {
      const T *p = src + (size_t)x * components;
      T r = p[0];
      T g = gray ? r : p[1];
      T b = gray ? r : p[2];
      T a = has_alpha ? p[components - 1] : opaque;

      T *o = dst + (size_t)x * 4;
      o[0] = r;
      o[1] = g;
      o[2] = b;
      o[3] = a;
    }
  }
}

void convert_rows_to_f16(uint8_t *base, size_t src_row_stride,
                         size_t dst_row_stride, const ImageInfo &src_info,
                         HdrKind hdr_kind, int primaries, uint32_t y,
                         uint32_t height) {
  const bool wide = src_info.bits == 16;
  const float scale = wide ? 1.0f / 65535.0f : 1.0f / 255.0f;
  const float *matrix = hdr_matrix_to_srgb(primaries);

  for (uint32_t i = height; i-- > 0;) {
    const uint8_t *src_row = base + src_row_stride * (y + i);
    uint16_t *dst = (uint16_t *)(base + dst_row_stride * (y + i));

    for (uint32_t x = src_info.original_width; x-- > 0;) {
      float rgba[4];
      if (wide) {
        const uint16_t *p = (const uint16_t *)src_row + (size_t)x * 4;
        for (int c = 0; c < 4; c++) {
          rgba[c] = p[c] * scale;
        }
      } else {
        const uint8_t *p = src_row + (size_t)x * 4;
        for (int c = 0; c < 4; c++) {
          rgba[c] = p[c] * scale;
        }
      }

      uint16_t *o = dst + (size_t)x * 4;

      switch (hdr_kind) {
      case HdrKind::PQ:
        for (int c = 0; c < 3; c++) {
          rgba[c] = hdr_pq_eotf(rgba[c]);
        }
        break;
      case HdrKind::HLG:
        for (int c = 0; c < 3; c++) {
          rgba[c] = hdr_hlg_inverse_oetf(rgba[c]);
        }
        hdr_hlg_ootf(rgba);
        break;
      case HdrKind::Linear:
        break;
      default: // Already sRGB-encoded; an SDR pixel keeps its value untouched.
        o[0] = hdr_float_to_half(rgba[0]);
        o[1] = hdr_float_to_half(rgba[1]);
        o[2] = hdr_float_to_half(rgba[2]);
        o[3] = hdr_encode_alpha(rgba[3]);
        continue;
      }

      if (matrix) {
        float r =
            matrix[0] * rgba[0] + matrix[1] * rgba[1] + matrix[2] * rgba[2];
        float g =
            matrix[3] * rgba[0] + matrix[4] * rgba[1] + matrix[5] * rgba[2];
        float b =
            matrix[6] * rgba[0] + matrix[7] * rgba[1] + matrix[8] * rgba[2];
        rgba[0] = r;
        rgba[1] = g;
        rgba[2] = b;
      }

      o[0] = hdr_encode_half(rgba[0]);
      o[1] = hdr_encode_half(rgba[1]);
      o[2] = hdr_encode_half(rgba[2]);
      o[3] = hdr_encode_alpha(rgba[3]);
    }
  }
}

void expand_rows_to_rgba(uint8_t *base, size_t src_row_stride,
                         size_t dst_row_stride, const ImageInfo &info,
                         uint32_t y, uint32_t height) {
  if (info.components == 4 && src_row_stride == dst_row_stride) {
    return;
  }

  const bool gray = info.color == ColorFamily::Gray;
  if (info.bits == 16) {
    expand_rows_to_rgba_t<uint16_t>(base, src_row_stride, dst_row_stride,
                                    info.original_width, info.components, gray,
                                    info.has_alpha, y, height);
  } else {
    expand_rows_to_rgba_t<uint8_t>(base, src_row_stride, dst_row_stride,
                                   info.original_width, info.components, gray,
                                   info.has_alpha, y, height);
  }
}

SrgbTransform::SrgbTransform(const ImageInfo &info, cmsHPROFILE src_profile,
                             bool wanted)
    : m_width(info.original_width) {
  cmsUInt32Number format = cms_format(info);
  if (!wanted || !format || !src_profile) {
    return;
  }

  bool gray = info.color == ColorFamily::Gray;
  cmsHPROFILE dst = gray ? create_srgb_gray() : cmsCreate_sRGBProfile();
  if (!dst) {
    throw std::runtime_error("Failed to create sRGB profile");
  }

  m_transform = cmsCreateTransform(src_profile, format, dst, format,
                                   cmsGetHeaderRenderingIntent(src_profile),
                                   cmsFLAGS_COPY_ALPHA);
  cmsCloseProfile(dst);
  if (!m_transform) {
    throw std::runtime_error("Failed to create sRGB transform");
  }
}

void SrgbTransform::apply(uint8_t *base, size_t row_stride, uint32_t y,
                          uint32_t height) const {
  if (!m_transform) {
    return;
  }
  for (uint32_t i = 0; i < height; i++) {
    uint8_t *row = base + row_stride * (y + i);
    cmsDoTransform(m_transform, row, row, m_width);
  }
}

void BaseDecoder::finish_layout(uint8_t *base, size_t stride,
                                const ImageInfo &dec, const DecodeOptions &opts,
                                uint32_t y, uint32_t height) {
  switch (opts.output_mode) {
  case OutputMode::Original:
  case OutputMode::YUVtoRGB:
    return;

  case OutputMode::Rgba8:
    convert_rows_to_rgba8(base, stride, dec, y, height);
    return;

  case OutputMode::RgbaF16: {
    expand_rows_to_rgba(base, stride, stride, dec, y, height);
    ImageInfo rgba = dec;
    rgba.components = 4;
    rgba.has_alpha = true;
    convert_rows_to_f16(base, stride, stride, rgba, hdr_kind(), hdr_primaries(),
                        y, height);
    return;
  }
  }
}

std::vector<uint8_t> orient_copy(const uint8_t *src, size_t stride,
                                 const ImageInfo &layout,
                                 uint32_t orientation) {
  const size_t pixel = (size_t)layout.components * ((layout.bits + 7) / 8);
  const uint32_t w = layout.original_width;
  const uint32_t h = layout.original_height;
  const bool swap_axes = orientation_swaps_axes(orientation);
  const uint32_t dst_w = swap_axes ? h : w;
  const uint32_t dst_h = swap_axes ? w : h;
  const size_t dst_stride = (size_t)dst_w * pixel;
  std::vector<uint8_t> out((size_t)dst_w * dst_h * pixel);

  if (orientation < 2 || orientation > 8) {
    for (uint32_t y = 0; y < h; y++) {
      memcpy(out.data() + dst_stride * y, src + stride * y, dst_stride);
    }
    return out;
  }

  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *row = src + stride * y;
    for (uint32_t x = 0; x < w; x++) {
      uint32_t dx;
      uint32_t dy;
      switch (orientation) {
      case 2:
        dx = w - 1 - x;
        dy = y;
        break;
      case 3:
        dx = w - 1 - x;
        dy = h - 1 - y;
        break;
      case 4:
        dx = x;
        dy = h - 1 - y;
        break;
      case 5:
        dx = y;
        dy = x;
        break;
      case 6:
        dx = h - 1 - y;
        dy = x;
        break;
      case 7:
        dx = h - 1 - y;
        dy = w - 1 - x;
        break;
      default:
        dx = y;
        dy = w - 1 - x;
        break; // 8
      }
      memcpy(out.data() + dst_stride * dy + (size_t)dx * pixel,
             row + (size_t)x * pixel, pixel);
    }
  }
  return out;
}

void BaseDecoder::rewind() {
  if (m_error) {
    std::rethrow_exception(m_error);
  }
  if (!source_complete()) {
    throw std::runtime_error("Rewinding needs the whole file");
  }
  try {
    rewind_codec();
    m_started = false;
    m_region_done = false;
    m_laid_out = false;
    m_shown_valid = false;
    m_frame = 0;
    m_duration_ms = 0;
    // Now, so what the header gives (frame count, HDR kind) holds throughout.
    if (!read_header()) {
      throw std::runtime_error("Image header no longer reads");
    }
  } catch (...) {
    // A restart that threw leaves the codec half built; later calls repeat
    // this.
    m_error = std::current_exception();
    throw;
  }
}

DecodeProgress BaseDecoder::decode(const DecodeOptions &options) {
  if (m_error) {
    std::rethrow_exception(m_error);
  }
  if (m_region_done) {
    rewind();
  }
  try {
    if (!read_header()) {
      return {};
    }

    DecodeOptions opts = options;
    // HDR samples are the caller's to map; a gain map's SDR base is not.
    if (is_hdr() && hdr_kind() != HdrKind::Gainmap) {
      opts.srgb_output = false;
    }
    // Converted YCbCr can always expand; otherwise the file's layout decides.
    const bool converted = info.color == ColorFamily::YUV &&
                           opts.output_mode != OutputMode::Original;
    if (!converted && !can_expand_rgba(info)) {
      opts.output_mode = OutputMode::Original;
    }

    if (m_laid_out && !(m_layout_opts == opts)) {
      restart();
      m_laid_out = false;
      m_shown_valid = false;
      if (!read_header()) {
        return {};
      }
    }

    m_started = true;
    const StepResult r = decode_impl(opts);
    m_laid_out = true;
    m_layout_opts = opts;

    DecodeProgress progress;
    progress.complete = r.complete;
    progress.frame = m_frame;
    progress.duration_ms = m_duration_ms;

    if (r.finished_stride == 0) {
      if (m_shown_valid && r.partial.empty()) {
        // Nothing new: the last finished frame is still what buffer() holds.
        progress.info = m_shown;
        progress.stride = m_shown_stride;
        progress.finished = true;
        return progress;
      }
      m_shown_valid = false;
    }

    ImageInfo layout = info;
    layout.width = layout.original_width;
    layout.height = layout.original_height;
    if (converted) {
      layout.color = ColorFamily::RGB;
      layout.components = layout.has_alpha ? 4 : 3;
      layout.subsampling_w = 0;
      layout.subsampling_h = 0;
      layout.full_range = true;
    }
    if (expands_to_rgba(opts.output_mode)) {
      const bool f16 = opts.output_mode == OutputMode::RgbaF16;
      layout.components = 4;
      layout.has_alpha = true;
      layout.color = ColorFamily::RGB;
      layout.full_range = true;
      layout.bits = f16 ? 16 : 8;
      layout.sample_type = f16 ? SampleType::Float : SampleType::Integer;
    }
    progress.info = layout;

    if (r.finished_stride == 0) {
      progress.rect = r.partial;
      progress.changed = !r.partial.empty();
      progress.stride = r.stride;
      info.width = layout.width;
      info.height = layout.height;
      return progress;
    }

    const size_t stride = r.finished_stride;
    progress.stride = stride;

    // Planar YUV is exact-sized and has no whole pixels to compact or turn;
    // streamed rows already went to the sampler.
    if (can_expand_rgba(layout) && !(m_sampler && m_sampler->streamed)) {
      const size_t out_stride = row_bytes(layout);
      const uint32_t h = layout.original_height;
      if (out_stride > stride ||
          (h > 0 && (uint64_t)m_buffer.size() <
                        (uint64_t)stride * (h - 1) + out_stride)) {
        throw std::runtime_error("Output buffer is smaller than its layout");
      }
      const bool turn = opts.apply_orientation && layout.orientation >= 2 &&
                        layout.orientation <= 8;
      if (turn) {
        m_buffer =
            orient_copy(m_buffer.data(), stride, layout, layout.orientation);
        if (orientation_swaps_axes(layout.orientation)) {
          std::swap(progress.info.width, progress.info.height);
        }
      } else {
        // Close the gap a decoded row wider than the output one left behind.
        if (out_stride != stride) {
          for (uint32_t y = 1; y < h; y++) {
            memmove(m_buffer.data() + out_stride * y,
                    m_buffer.data() + stride * y, out_stride);
          }
        }
        m_buffer.resize(out_stride * h);
      }
      // The turned width: rows are original_height wide after a quarter turn.
      progress.stride = (size_t)progress.info.width * progress.info.components *
                        ((progress.info.bits + 7) / 8);
    }

    info.width = progress.info.width;
    info.height = progress.info.height;
    progress.rect =
        InvalidRect{0, 0, progress.info.width, progress.info.height};
    progress.changed = !progress.rect.empty();
    progress.finished = true;
    m_shown = progress.info;
    m_shown_stride = progress.stride;
    m_shown_valid = true;
    return progress;
  } catch (...) {
    m_error = std::current_exception();
    throw;
  }
}

// Where stored pixel (x, y) lands once turned; as orient_copy places it.
static void turn_point(uint32_t orientation, uint32_t w, uint32_t h, uint32_t x,
                       uint32_t y, uint32_t *dx, uint32_t *dy) {
  switch (orientation) {
  case 2:
    *dx = w - 1 - x;
    *dy = y;
    return;
  case 3:
    *dx = w - 1 - x;
    *dy = h - 1 - y;
    return;
  case 4:
    *dx = x;
    *dy = h - 1 - y;
    return;
  case 5:
    *dx = y;
    *dy = x;
    return;
  case 6:
    *dx = h - 1 - y;
    *dy = x;
    return;
  case 7:
    *dx = h - 1 - y;
    *dy = w - 1 - x;
    return;
  case 8:
    *dx = y;
    *dy = w - 1 - x;
    return;
  default:
    *dx = x;
    *dy = y;
    return;
  }
}

InvalidRect turn_rect(const InvalidRect &r, uint32_t orientation, uint32_t w,
                      uint32_t h) {
  uint32_t x0, y0, x1, y1;
  turn_point(orientation, w, h, r.x, r.y, &x0, &y0);
  turn_point(orientation, w, h, r.x + r.width - 1, r.y + r.height - 1, &x1,
             &y1);
  const uint32_t left = std::min(x0, x1);
  const uint32_t top = std::min(y0, y1);
  return InvalidRect{left, top, std::max(x0, x1) - left + 1,
                     std::max(y0, y1) - top + 1};
}

InvalidRect unturn_rect(const InvalidRect &r, uint32_t orientation, uint32_t w,
                        uint32_t h) {
  // 6 and 8 undo each other; every other orientation undoes itself.
  const uint32_t inverse = orientation == 6   ? 8
                           : orientation == 8 ? 6
                                              : orientation;
  const bool swap = orientation_swaps_axes(orientation);
  return turn_rect(r, inverse, swap ? h : w, swap ? w : h);
}

DecodeProgress BaseDecoder::decode_region(const DecodeOptions &options,
                                          const InvalidRect *rect,
                                          uint32_t sample) {
  DecodeProgress progress = decode_region_once(options, rect, sample);
  // Frees the codec's working memory now: a viewer may hold the decoder idle
  // long after. A failure is the next call's to report.
  std::vector<uint8_t> result = std::move(m_buffer);
  try {
    rewind();
  } catch (...) {
  }
  m_buffer = std::move(result);
  return progress;
}

DecodeProgress BaseDecoder::decode_region_once(const DecodeOptions &options,
                                               const InvalidRect *rect,
                                               uint32_t sample) {
  if (m_error) {
    std::rethrow_exception(m_error);
  }
  if (!source_complete()) {
    throw std::runtime_error("Region decoding needs the whole file");
  }
  if (m_started) {
    rewind();
  }
  try {
    if (!read_header()) {
      throw std::runtime_error("Truncated image");
    }
  } catch (...) {
    m_error = std::current_exception();
    throw;
  }

  const ImageInfo full = info;
  const uint32_t sw = full.original_width;
  const uint32_t sh = full.original_height;
  const uint32_t orientation = options.apply_orientation ? full.orientation : 1;
  const bool swap = orientation_swaps_axes(orientation);
  const uint32_t tw = swap ? sh : sw;
  const uint32_t th = swap ? sw : sh;

  InvalidRect want{0, 0, tw, th};
  if (rect) {
    if (rect->empty() || rect->x >= tw || rect->y >= th ||
        rect->width > tw - rect->x || rect->height > th - rect->y) {
      throw std::runtime_error("Region lies outside the image");
    }
    want = *rect;
  }
  if (sample > 1) {
    return decode_sampled(options, want, sample);
  }
  const bool whole =
      want.x == 0 && want.y == 0 && want.width == tw && want.height == th;

  // What the codec makes, in stored coordinates: a superset of the request.
  InvalidRect made{0, 0, sw, sh};
  if (!whole) {
    try {
      const InvalidRect stored = unturn_rect(want, orientation, sw, sh);
      made = begin_region(stored);
      // The crop below relies on it: inside the image, around the request.
      if (made.empty() || made.x > sw || made.width > sw - made.x ||
          made.y > sh || made.height > sh - made.y || made.x > stored.x ||
          made.y > stored.y || made.x + made.width < stored.x + stored.width ||
          made.y + made.height < stored.y + stored.height) {
        throw std::logic_error("Codec region does not cover the request");
      }
    } catch (...) {
      m_error = std::current_exception();
      throw;
    }
    // The codec lays out a made-sized image, so the turn and crop below do too.
    info.original_width = made.width;
    info.original_height = made.height;
    info.width = made.width;
    info.height = made.height;
  }

  DecodeProgress progress;
  try {
    for (;;) {
      progress = decode(options);
      if (progress.finished && progress.changed) {
        break;
      }
      if (progress.complete || !progress.changed) {
        throw std::runtime_error("Image has no frame to decode");
      }
    }
  } catch (...) {
    info = full;
    m_region = InvalidRect{};
    m_region_done = true;
    throw;
  }
  info = full;
  m_region = InvalidRect{};
  m_region_done = true;
  // buffer() no longer holds the whole frame decode() would report.
  m_shown_valid = false;
  m_laid_out = false;

  if (whole) {
    return progress;
  }

  // The request, relative to the made rect once both are turned.
  const InvalidRect made_turned = turn_rect(made, orientation, sw, sh);
  const InvalidRect crop{want.x - made_turned.x, want.y - made_turned.y,
                         want.width, want.height};
  progress.info.original_width = full.original_width;
  progress.info.original_height = full.original_height;
  if (crop.x == 0 && crop.y == 0 && crop.width == progress.info.width &&
      crop.height == progress.info.height) {
    return progress;
  }
  if (!can_expand_rgba(progress.info)) {
    throw std::runtime_error("Planar output cannot be cropped");
  }

  try {
    // In place: each cropped row lands at or before where it is read from.
    const size_t pixel =
        (size_t)progress.info.components * ((progress.info.bits + 7) / 8);
    const size_t out_stride = (size_t)crop.width * pixel;
    if (progress.info.width != made_turned.width ||
        progress.info.height != made_turned.height ||
        progress.stride < (size_t)progress.info.width * pixel ||
        (uint64_t)m_buffer.size() <
            (uint64_t)progress.stride * (crop.y + crop.height - 1) +
                (uint64_t)(crop.x + crop.width) * pixel) {
      throw std::logic_error("Region frame does not match its layout");
    }
    for (uint32_t y = 0; y < crop.height; y++) {
      memmove(m_buffer.data() + out_stride * y,
              m_buffer.data() + progress.stride * (crop.y + y) +
                  (size_t)crop.x * pixel,
              out_stride);
    }
    m_buffer.resize(out_stride * crop.height);
    m_buffer.shrink_to_fit();

    progress.info.width = crop.width;
    progress.info.height = crop.height;
    progress.stride = out_stride;
    progress.rect = InvalidRect{0, 0, crop.width, crop.height};
    return progress;
  } catch (...) {
    m_error = std::current_exception();
    throw;
  }
}

static uint32_t nearest(const RowSampler::Axis &a, uint32_t i) {
  const int64_t block = (int64_t)a.start + (int64_t)i * a.k - a.phase;
  const int64_t at = a.last ? block + a.k - 1 : block;
  return (uint32_t)std::min<int64_t>(std::max<int64_t>(at, a.start), a.end - 1);
}

RowSampler::RowSampler(size_t pixel_bytes, Axis x, Axis y)
    : m_pixel(pixel_bytes), m_x(x), m_y(y) {
  if (x.out == 0 || y.out == 0 || x.k == 0 || y.k == 0 || x.start >= x.end ||
      y.start >= y.end || pixel_bytes == 0) {
    throw std::logic_error("Empty sampling");
  }
  m_cols.resize(x.out);
  for (uint32_t i = 0; i < x.out; i++) {
    m_cols[i] = nearest(x, i);
  }
  m_rows.resize(y.out);
  for (uint32_t i = 0; i < y.out; i++) {
    m_rows[i] = nearest(y, i);
  }
  m_out.assign(checked_buffer_bytes((size_t)x.out * pixel_bytes, y.out), 0);
}

void RowSampler::feed(const uint8_t *first, size_t stride, uint32_t y,
                      uint32_t n) {
  const size_t out_stride = (size_t)m_x.out * m_pixel;
  for (uint32_t r = 0; r < n; r++, y++) {
    if (y != m_next) {
      throw std::logic_error("Sampled rows out of order");
    }
    m_next++;
    if (y < m_y.start || y >= m_y.end) {
      continue;
    }
    m_fed++;
    const uint8_t *row = first + stride * r;
    // Rows only increase, so output rows (clamped ones share) fill in order.
    while (m_row < m_y.out && m_rows[m_row] == y) {
      uint8_t *dst = m_out.data() + out_stride * m_row;
      for (uint32_t i = 0; i < m_x.out; i++) {
        memcpy(dst + (size_t)i * m_pixel, row + (size_t)m_cols[i] * m_pixel,
               m_pixel);
      }
      m_row++;
    }
  }
}

std::vector<uint8_t> RowSampler::finish() {
  if (m_row != m_y.out) {
    throw std::runtime_error("Sampled region got too few rows");
  }
  return std::move(m_out);
}

void BaseDecoder::emit_rows(const uint8_t *first, size_t stride, uint32_t y,
                            uint32_t n) {
  if (!m_sampler) {
    throw std::logic_error("Rows handed on outside a sampled decode");
  }
  m_sampler->streamed = true;
  m_sampler->feed(first, stride, y, n);
}

void BaseDecoder::emit_frame(const uint8_t *src, size_t src_stride,
                             const ImageInfo &dec, const DecodeOptions &opts,
                             const SrgbTransform *srgb, uint32_t rows) {
  const uint32_t band = 64;
  const size_t stride = layout_stride(dec, opts);
  const size_t row = row_bytes(dec);
  if (row > src_stride) {
    throw std::logic_error("Frame rows are wider than their stride");
  }
  m_buffer.resize(checked_buffer_bytes(stride, std::min(band, rows)));
  for (uint32_t y = 0; y < rows; y += band) {
    const uint32_t n = std::min(band, rows - y);
    for (uint32_t r = 0; r < n; r++) {
      memcpy(m_buffer.data() + stride * r, src + src_stride * (y + r), row);
    }
    if (srgb) {
      srgb->apply(m_buffer.data(), stride, 0, n);
    }
    finish_layout(m_buffer.data(), stride, dec, opts, 0, n);
    emit_rows(m_buffer.data(), stride, y, n);
  }
}

static uint32_t div_ceil(uint32_t a, uint32_t b) {
  return (uint32_t)(((uint64_t)a + b - 1) / b);
}

DecodeProgress BaseDecoder::decode_sampled(const DecodeOptions &options,
                                           const InvalidRect &want,
                                           uint32_t sample, bool native) {
  const ImageInfo full = info;
  const uint32_t sw = full.original_width;
  const uint32_t sh = full.original_height;
  const uint32_t orientation = options.apply_orientation &&
                                       full.orientation >= 2 &&
                                       full.orientation <= 8
                                   ? full.orientation
                                   : 1;

  if (!expands_to_rgba(options.output_mode)) {
    throw std::runtime_error("Sampling needs an Rgba output mode");
  }
  // Past the image's size every sample gives one pixel; capped there, all the
  // block arithmetic below stays well inside 32 bits.
  sample = std::min(sample, std::max(sw, sh));
  const bool converted = full.color == ColorFamily::YUV;
  if (!converted && !can_expand_rgba(full)) {
    throw std::runtime_error("This image cannot be sampled");
  }
  const bool f16 = options.output_mode == OutputMode::RgbaF16;

  // Decoded unturned; only the small result is turned.
  DecodeOptions inner = options;
  inner.apply_orientation = false;

  const InvalidRect stored = unturn_rect(want, orientation, sw, sh);
  // Where turning reverses a stored axis, its partial block goes first.
  const bool rev_x = orientation == 2 || orientation == 3 || orientation == 7 ||
                     orientation == 8;
  const bool rev_y = orientation == 3 || orientation == 4 || orientation == 6 ||
                     orientation == 7;

  const uint32_t ow = div_ceil(stored.width, sample);
  const uint32_t oh = div_ceil(stored.height, sample);
  DecodeProgress progress;
  InvalidRect made;
  InvalidRect req;
  std::vector<uint8_t> out;
  uint32_t f = 1;
  try {
    // Blocks start where the turned region does: its far end on a reversed
    // axis.
    const uint32_t ax = rev_x ? stored.x + stored.width : stored.x;
    const uint32_t ay = rev_y ? stored.y + stored.height : stored.y;
    uint32_t aligned = sample & (~sample + 1);
    while (aligned > 1 && (ax % aligned || ay % aligned)) {
      aligned /= 2;
    }
    // The request widened to whole blocks, from where the turned region
    // starts, and clipped to the image: what a codec scaling all the way by
    // the sample makes, so each of its pixels is one block.
    auto widen = [&](uint32_t pos, uint32_t len, uint32_t limit, bool rev,
                     uint32_t *gpos, uint32_t *glen) {
      const int64_t span = (int64_t)div_ceil(len, sample) * sample;
      int64_t a = rev ? (int64_t)pos + len - span : (int64_t)pos;
      int64_t b = a + span;
      a = std::max<int64_t>(a, 0);
      b = std::min<int64_t>(b, limit);
      *gpos = (uint32_t)a;
      *glen = (uint32_t)(b - a);
    };
    InvalidRect grid;
    widen(stored.x, stored.width, sw, rev_x, &grid.x, &grid.width);
    widen(stored.y, stored.height, sh, rev_y, &grid.y, &grid.height);

    f = native ? begin_scale(sample, aligned, grid) : 1;
    if (f == 0 || sample % f != 0) {
      throw std::logic_error("Codec scale does not divide the sample");
    }
    req = f == sample ? grid : stored;
    made = begin_region(req);
    if (made.empty() || made.x > sw || made.width > sw - made.x ||
        made.y > sh || made.height > sh - made.y || made.x > req.x ||
        made.y > req.y || made.x + made.width < req.x + req.width ||
        made.y + made.height < req.y + req.height) {
      throw std::logic_error("Codec region does not cover the request");
    }
    const uint32_t cw = div_ceil(made.width, f);
    const uint32_t ch = div_ceil(made.height, f);
    info.original_width = cw;
    info.original_height = ch;
    info.width = cw;
    info.height = ch;

    // The request in the codec's (scaled) pixels, inside the made image.
    auto axis = [&](uint32_t pos, uint32_t len, uint32_t mpos, uint32_t clen,
                    bool rev, uint32_t want_len) {
      RowSampler::Axis a;
      a.start = std::min((pos - mpos) / f, clen - 1);
      a.end =
          std::max(std::min(div_ceil(pos + len - mpos, f), clen), a.start + 1);
      a.k = sample / f;
      a.out = div_ceil(want_len, sample);
      const uint64_t span = (uint64_t)a.out * a.k;
      a.phase = rev && span > a.end - a.start
                    ? (uint32_t)(span - (a.end - a.start))
                    : 0;
      a.last = rev;
      return a;
    };
    const RowSampler::Axis sx =
        axis(req.x, req.width, made.x, cw, rev_x, stored.width);
    const RowSampler::Axis sy =
        axis(req.y, req.height, made.y, ch, rev_y, stored.height);
    m_sampler = std::make_unique<RowSampler>(f16 ? 8 : 4, sx, sy);

    for (;;) {
      progress = decode(inner);
      if (progress.finished && progress.changed) {
        break;
      }
      if (progress.complete || !progress.changed) {
        throw std::runtime_error("Image has no frame to decode");
      }
    }
    if (!m_sampler->streamed) {
      const ImageInfo &l = progress.info;
      if (l.components != 4 || l.bits != (f16 ? 16u : 8u) || l.width != cw ||
          l.height != ch ||
          (uint64_t)m_buffer.size() < (uint64_t)progress.stride * ch) {
        throw std::logic_error("Sampled frame does not match its layout");
      }
      m_sampler->feed(m_buffer.data(), progress.stride, 0, ch);
    }
    if (m_sampler->fed() != sy.end - sy.start) {
      throw std::logic_error("Sampled frame is missing rows");
    }
    out = m_sampler->finish();
    // The codec's buffers are done with before the turn copies the result.
    m_sampler.reset();
    std::vector<uint8_t>().swap(m_buffer);
    if (orientation != 1) {
      ImageInfo stored_layout = progress.info;
      stored_layout.original_width = ow;
      stored_layout.original_height = oh;
      out = orient_copy(out.data(), (size_t)ow * (f16 ? 8 : 4), stored_layout,
                        orientation);
    }
    return finish_sampled(progress, std::move(out), full, ow, oh, orientation);
  } catch (const std::bad_alloc &) {
    info = full;
    m_region = InvalidRect{};
    m_scale = 1;
    m_sampler.reset();
    m_region_done = true;
    m_error = std::current_exception();
    throw;
  } catch (...) {
    info = full;
    m_region = InvalidRect{};
    m_scale = 1;
    m_sampler.reset();
    m_region_done = true;
    if (f <= 1) {
      m_error = std::current_exception();
      throw;
    }
    // A codec's own scaling can refuse what its plain decode takes (a JPEG
    // 2000 tile with fewer levels than the header said): once more without.
    m_error = nullptr;
  }
  rewind();
  try {
    if (!read_header()) {
      throw std::runtime_error("Truncated image");
    }
  } catch (...) {
    m_error = std::current_exception();
    throw;
  }
  return decode_sampled(options, want, sample, false);
}

DecodeProgress BaseDecoder::finish_sampled(DecodeProgress progress,
                                           std::vector<uint8_t> &&out,
                                           const ImageInfo &full, uint32_t ow,
                                           uint32_t oh, uint32_t orientation) {
  info = full;
  m_region = InvalidRect{};
  m_scale = 1;
  m_region_done = true;
  m_shown_valid = false;
  m_laid_out = false;

  const bool f16 = progress.info.bits == 16;
  ImageInfo layout = progress.info;
  layout.original_width = ow;
  layout.original_height = oh;
  layout.width = ow;
  layout.height = oh;
  if (orientation_swaps_axes(orientation)) {
    std::swap(layout.width, layout.height);
  }
  m_buffer = std::move(out);

  progress.info = layout;
  progress.info.original_width = full.original_width;
  progress.info.original_height = full.original_height;
  progress.stride = (size_t)layout.width * 4 * (f16 ? 2 : 1);
  progress.rect = InvalidRect{0, 0, layout.width, layout.height};
  progress.changed = true;
  progress.finished = true;
  return progress;
}

} // namespace imagedecoder
