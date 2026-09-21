#include "decoder_tiff.h"

#include <algorithm>
#include <string.h>

namespace imagedecoder {

// Straight alpha from premultiplied, in place; alpha is the last channel.
template <typename T>
static void unpremultiply(uint8_t *base, size_t stride, uint32_t width,
                          uint32_t height, uint32_t channels) {
  const uint64_t max = (T) ~(T)0;
  for (uint32_t y = 0; y < height; y++) {
    T *p = (T *)(base + stride * y);
    for (uint32_t x = 0; x < width; x++, p += channels) {
      const uint64_t a = p[channels - 1];
      if (a == 0 || a == max) {
        continue;
      }
      for (uint32_t c = 0; c + 1 < channels; c++) {
        p[c] = (T)std::min<uint64_t>((p[c] * max + a / 2) / a, max);
      }
    }
  }
}

TiffDecoder::TiffDecoder() = default;

TiffDecoder::~TiffDecoder() { close(); }

bool TiffDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (!m_source.complete()) {
    return false;
  }

  if (!open()) {
    throw std::runtime_error("Invalid TIFF");
  }

  uint32_t width = 0;
  uint32_t height = 0;
  if (!TIFFGetField(m_tiff, TIFFTAG_IMAGEWIDTH, &width) ||
      !TIFFGetField(m_tiff, TIFFTAG_IMAGELENGTH, &height) || width == 0 ||
      height == 0) {
    throw std::runtime_error("TIFF has no image dimensions");
  }
  check_dimensions(width, height);

  TIFFGetFieldDefaulted(m_tiff, TIFFTAG_BITSPERSAMPLE, &m_bits_per_sample);
  TIFFGetFieldDefaulted(m_tiff, TIFFTAG_SAMPLESPERPIXEL, &m_samples_per_pixel);
  TIFFGetFieldDefaulted(m_tiff, TIFFTAG_PLANARCONFIG, &m_planar_config);
  uint16_t extra_count = 0;
  uint16_t *extra = nullptr;
  m_associated_alpha =
      TIFFGetField(m_tiff, TIFFTAG_EXTRASAMPLES, &extra_count, &extra) &&
      extra_count > 0 && extra && extra[0] == EXTRASAMPLE_ASSOCALPHA;
  if (!TIFFGetField(m_tiff, TIFFTAG_PHOTOMETRIC, &m_photometric)) {
    m_photometric = PHOTOMETRIC_MINISBLACK;
  }

  uint16_t sample_format = SAMPLEFORMAT_UINT;
  if (!TIFFGetField(m_tiff, TIFFTAG_SAMPLEFORMAT, &sample_format)) {
    sample_format = SAMPLEFORMAT_UINT;
  }

  m_native_layout = m_planar_config == PLANARCONFIG_CONTIG &&
                    (sample_format == SAMPLEFORMAT_UINT ||
                     sample_format == SAMPLEFORMAT_VOID) &&
                    (m_bits_per_sample == 8 || m_bits_per_sample == 16) &&
                    ((m_photometric == PHOTOMETRIC_MINISBLACK &&
                      (m_samples_per_pixel == 1 || m_samples_per_pixel == 2)) ||
                     (m_photometric == PHOTOMETRIC_RGB &&
                      (m_samples_per_pixel == 3 || m_samples_per_pixel == 4)));

  if (m_native_layout) {
    const uint64_t native_row =
        (uint64_t)width * m_samples_per_pixel * ((m_bits_per_sample + 7) / 8);
    const tmsize_t scanline = TIFFScanlineSize(m_tiff);
    if (scanline <= 0 || (uint64_t)scanline > native_row) {
      m_native_layout = false;
    }
  }

  uint32_t icc_size = 0;
  void *icc_data = nullptr;
  if (TIFFGetField(m_tiff, TIFFTAG_ICCPROFILE, &icc_size, &icc_data) &&
      icc_data && icc_size > 0) {
    m_src_profile = cmsOpenProfileFromMem(icc_data, icc_size);
    if (m_src_profile) {
      cmsColorSpaceSignature space = cmsGetColorSpace(m_src_profile);
      bool rgb_out = !m_native_layout || m_photometric == PHOTOMETRIC_RGB;
      if ((rgb_out && space != cmsSigRgbData) ||
          (!rgb_out && space != cmsSigGrayData)) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      }
    }
  }

  uint32_t components;
  uint32_t bits;
  ColorFamily color;
  bool has_alpha;
  if (m_native_layout) {
    components = m_samples_per_pixel;
    bits = m_bits_per_sample;
    color =
        m_photometric == PHOTOMETRIC_RGB ? ColorFamily::RGB : ColorFamily::Gray;
    has_alpha = m_samples_per_pixel == 2 || m_samples_per_pixel == 4;
  } else {
    components = 4;
    bits = 8;
    color = ColorFamily::RGB;
    has_alpha = true;
  }

  info = {
      .width = width,
      .height = height,
      .original_width = width,
      .original_height = height,
      .components = components,
      .has_alpha = has_alpha,
      .color = color,
      .sample_type = SampleType::Integer,
      .bits = bits,
  };
  {
    uint16_t tag = 1;
    if (TIFFGetField(m_tiff, TIFFTAG_ORIENTATION, &tag) && tag >= 1 &&
        tag <= 8) {
      info.orientation = tag;
    }
  }

  m_has_info = true;
  return true;
}

BaseDecoder::StepResult TiffDecoder::decode_impl(const DecodeOptions &opts) {
  if (!read_header()) {
    return {};
  }

  if (m_complete) {
    return {true, 0, {}};
  }

  const ImageInfo &dec = info;

  if (m_native_layout) {
    read_native(opts);
  } else {
    read_rgba(opts);
  }

  const size_t stride = layout_stride(dec, opts);

  if (opts.srgb_output) {
    SrgbTransform(dec, m_src_profile, true)
        .apply(m_buffer.data(), stride, 0, dec.original_height);
  }

  finish_layout(m_buffer.data(), stride, dec, opts, 0, dec.original_height);

  m_complete = true;
  return {true, stride, {}};
}

void TiffDecoder::restart() {
  close();
  m_has_info = false;
  m_read_pos = 0;
  m_buffer.clear();
  m_complete = false;
}

void TiffDecoder::close() {
  if (m_tiff) {
    TIFFClose(m_tiff);
    m_tiff = nullptr;
  }
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
}

bool TiffDecoder::open() {
  if (m_tiff) {
    return true;
  }

  auto readProc = [](thandle_t h, void *buf, tmsize_t size) -> tmsize_t {
    auto *self = (TiffDecoder *)h;
    size_t total = self->m_source.size();
    if (self->m_read_pos >= total) {
      return 0;
    }
    size_t n = std::min((size_t)size, total - self->m_read_pos);
    memcpy(buf, self->m_source.data() + self->m_read_pos, n);
    self->m_read_pos += n;
    return (tmsize_t)n;
  };
  auto writeProc = [](thandle_t, void *, tmsize_t) -> tmsize_t { return 0; };
  auto seekProc = [](thandle_t h, toff_t off, int whence) -> toff_t {
    auto *self = (TiffDecoder *)h;
    size_t total = self->m_source.size();
    size_t base = whence == SEEK_CUR   ? self->m_read_pos
                  : whence == SEEK_END ? total
                                       : 0;
    self->m_read_pos = base + (size_t)off;
    return (toff_t)self->m_read_pos;
  };
  auto closeProc = [](thandle_t) -> int { return 0; };
  auto mapProc = [](thandle_t h, void **base, toff_t *size) -> int {
    auto *self = (TiffDecoder *)h;
    if (self->m_source.size() == 0) {
      return 0;
    }
    *base = (void *)self->m_source.data();
    *size = (toff_t)self->m_source.size();
    return 1;
  };
  auto unmapProc = [](thandle_t, void *, toff_t) {};
  auto sizeProc = [](thandle_t h) -> toff_t {
    return (toff_t)((TiffDecoder *)h)->m_source.size();
  };

  // Cap libtiff's header-sized allocations; handlers per handle, not global.
  TIFFOpenOptions *options = TIFFOpenOptionsAlloc();
  if (!options) {
    throw std::bad_alloc();
  }
  auto quiet = [](TIFF *, void *, const char *, const char *, va_list) {
    return 1;
  };
  TIFFOpenOptionsSetMaxSingleMemAlloc(options, (tmsize_t)(MAX_IMAGE_BYTES / 2));
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(options, (tmsize_t)MAX_IMAGE_BYTES);
  TIFFOpenOptionsSetErrorHandlerExtR(options, quiet, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(options, quiet, nullptr);

  m_read_pos = 0;
  m_tiff = TIFFClientOpenExt("imagedecoder", "r", (thandle_t)this, readProc,
                             writeProc, seekProc, closeProc, sizeProc, mapProc,
                             unmapProc, options);
  TIFFOpenOptionsFree(options);
  return m_tiff != nullptr;
}

void TiffDecoder::read_native(const DecodeOptions &opts) {
  const ImageInfo dec = info;
  const size_t src_stride = row_bytes(dec);

  const tmsize_t scanline = TIFFScanlineSize(m_tiff);
  if (scanline <= 0 || (size_t)scanline > src_stride) {
    throw std::runtime_error("TIFF scanline does not match its header");
  }

  const size_t row_stride = layout_stride(dec, opts);
  m_buffer.assign(checked_buffer_bytes(row_stride, info.original_height), 0);

  if (TIFFIsTiled(m_tiff)) {
    // Not through TIFFReadRGBAImage: it premultiplies unassociated alpha to
    // 8 bits, losing colour wherever alpha is low.
    uint32_t tw = 0;
    uint32_t th = 0;
    TIFFGetField(m_tiff, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(m_tiff, TIFFTAG_TILELENGTH, &th);
    const tmsize_t tile_bytes = TIFFTileSize(m_tiff);
    const tmsize_t tile_row = TIFFTileRowSize(m_tiff);
    const size_t pixel = src_stride / dec.original_width;
    if (tw == 0 || th == 0 || tile_bytes <= 0 || tile_row <= 0 ||
        (size_t)tile_row < (size_t)tw * pixel ||
        (uint64_t)tile_bytes < (uint64_t)tile_row * th) {
      throw std::runtime_error("TIFF tiles do not match its header");
    }
    std::vector<uint8_t> tile((size_t)tile_bytes);
    for (uint32_t ty = 0; ty < info.original_height; ty += th) {
      for (uint32_t tx = 0; tx < info.original_width; tx += tw) {
        if (TIFFReadTile(m_tiff, tile.data(), tx, ty, 0, 0) < 0) {
          throw std::runtime_error("Truncated or corrupt TIFF");
        }
        const uint32_t rows = std::min(th, info.original_height - ty);
        const size_t span =
            (size_t)std::min(tw, info.original_width - tx) * pixel;
        for (uint32_t r = 0; r < rows; r++) {
          memcpy(m_buffer.data() + row_stride * (ty + r) + (size_t)tx * pixel,
                 tile.data() + (size_t)tile_row * r, span);
        }
      }
    }
  } else {
    for (uint32_t y = 0; y < info.original_height; y++) {
      if (TIFFReadScanline(m_tiff, m_buffer.data() + row_stride * y, y, 0) <
          0) {
        throw std::runtime_error("Truncated or corrupt TIFF");
      }
    }
  }
  if (m_associated_alpha && dec.has_alpha) {
    if (dec.bits == 16) {
      unpremultiply<uint16_t>(m_buffer.data(), row_stride, dec.original_width,
                              dec.original_height, dec.components);
    } else {
      unpremultiply<uint8_t>(m_buffer.data(), row_stride, dec.original_width,
                             dec.original_height, dec.components);
    }
  }
}

void TiffDecoder::read_rgba(const DecodeOptions &opts) {
  const ImageInfo &rgba_dec = info;
  const size_t row_stride = layout_stride(rgba_dec, opts);
  m_buffer.assign(checked_buffer_bytes(row_stride, info.original_height), 0);

  std::vector<uint32_t> scratch;
  uint32_t *dst;
  if (row_stride == (size_t)info.original_width * 4) {
    dst = (uint32_t *)m_buffer.data();
  } else {
    scratch.resize((size_t)info.original_width * info.original_height);
    dst = scratch.data();
  }

  // The file's own orientation, so libtiff doesn't turn; decode() does.
  if (!TIFFReadRGBAImageOriented(m_tiff, info.original_width,
                                 info.original_height, dst,
                                 (int)info.orientation, 0)) {
    throw std::runtime_error("Failed to decode TIFF");
  }

  auto unpack = [](uint8_t *out, const uint32_t *in, size_t n) {
    for (size_t i = 0; i < n; i++) {
      uint32_t p = in[i];
      out[i * 4 + 0] = (uint8_t)TIFFGetR(p);
      out[i * 4 + 1] = (uint8_t)TIFFGetG(p);
      out[i * 4 + 2] = (uint8_t)TIFFGetB(p);
      out[i * 4 + 3] = (uint8_t)TIFFGetA(p);
    }
  };

  if (scratch.empty()) {
    unpack(m_buffer.data(), (const uint32_t *)m_buffer.data(),
           (size_t)info.original_width * info.original_height);
  } else {
    for (uint32_t y = 0; y < info.original_height; y++) {
      unpack(m_buffer.data() + row_stride * y,
             scratch.data() + (size_t)y * info.original_width,
             info.original_width);
    }
  }
  // TIFFReadRGBAImage premultiplies, whatever the file stored.
  unpremultiply<uint8_t>(m_buffer.data(), row_stride, info.original_width,
                         info.original_height, 4);
}

} // namespace imagedecoder
