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

DecodeProgress BaseDecoder::decode(const DecodeOptions &options) {
  if (m_error) {
    std::rethrow_exception(m_error);
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

    // Planar YUV is exact-sized and has no whole pixels to compact or turn.
    if (can_expand_rgba(layout)) {
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

} // namespace imagedecoder
