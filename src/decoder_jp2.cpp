#include "decoder_jp2.h"

#include <algorithm>
#include <string.h>

namespace imagedecoder {

static OPJ_SIZE_T jp2_read(void *buf, OPJ_SIZE_T size, void *data) {
  auto *self = (Jp2Decoder *)data;
  return self->stream_read(buf, size);
}

static OPJ_OFF_T jp2_skip(OPJ_OFF_T bytes, void *data) {
  auto *self = (Jp2Decoder *)data;
  return self->stream_skip(bytes);
}

static OPJ_BOOL jp2_seek(OPJ_OFF_T pos, void *data) {
  auto *self = (Jp2Decoder *)data;
  return self->stream_seek(pos) ? OPJ_TRUE : OPJ_FALSE;
}

size_t Jp2Decoder::stream_read(void *buf, size_t size) {
  if (m_read_pos >= m_source.size()) {
    return (size_t)-1;
  } // libopenjp2's end-of-stream
  size_t n = std::min(size, m_source.size() - m_read_pos);
  memcpy(buf, m_source.data() + m_read_pos, n);
  m_read_pos += n;
  return n;
}

int64_t Jp2Decoder::stream_skip(int64_t bytes) {
  if (bytes < 0) {
    return -1;
  }
  size_t room = m_source.size() - std::min(m_read_pos, m_source.size());
  size_t n = std::min((size_t)bytes, room);
  m_read_pos += n;
  return (int64_t)n;
}

bool Jp2Decoder::stream_seek(int64_t pos) {
  if (pos < 0 || (size_t)pos > m_source.size()) {
    return false;
  }
  m_read_pos = (size_t)pos;
  return true;
}

Jp2Decoder::~Jp2Decoder() { close(); }

bool Jp2Decoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (!m_source.complete()) {
    return false;
  }

  close(); // A header that threw last time left its image behind.
  open();

  const uint32_t numcomps = m_image->numcomps;
  if (numcomps == 0) {
    throw std::runtime_error("JPEG 2000 has no components");
  }

  const opj_image_comp_t &first = m_image->comps[0];
  check_dimensions(first.w, first.h);
  m_comp_x0 = first.x0;
  m_comp_y0 = first.y0;

  for (uint32_t c = 1; c < numcomps; c++) {
    if (m_image->comps[c].w != first.w || m_image->comps[c].h != first.h) {
      throw std::runtime_error("JPEG 2000 components differ in size");
    }
  }

  // Refused rather than emitted as RGB: opj_decode does not convert these.
  if (m_image->color_space != OPJ_CLRSPC_UNKNOWN &&
      m_image->color_space != OPJ_CLRSPC_UNSPECIFIED &&
      m_image->color_space != OPJ_CLRSPC_SRGB &&
      m_image->color_space != OPJ_CLRSPC_GRAY) {
    throw std::runtime_error("Unsupported JPEG 2000 colour space");
  }

  bool gray = numcomps < 3 || m_image->color_space == OPJ_CLRSPC_GRAY;
  // Alpha if marked, or if it is the only extra channel.
  const uint32_t colour = gray ? 1 : 3;
  bool alpha = numcomps > colour &&
               (m_image->comps[colour].alpha || numcomps == colour + 1);
  uint32_t components = gray ? (alpha ? 2u : 1u) : (alpha ? 4u : 3u);
  uint32_t bits = first.prec > 8 ? 16 : 8;

  if (m_image->icc_profile_buf && m_image->icc_profile_len > 0) {
    m_src_profile = cmsOpenProfileFromMem(m_image->icc_profile_buf,
                                          m_image->icc_profile_len);
    if (m_src_profile) {
      cmsColorSpaceSignature space = cmsGetColorSpace(m_src_profile);
      if ((gray && space != cmsSigGrayData) ||
          (!gray && space != cmsSigRgbData)) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      }
    }
  }

  info = {
      .width = first.w,
      .height = first.h,
      .original_width = first.w,
      .original_height = first.h,
      .components = components,
      .has_alpha = alpha,
      .color = gray ? ColorFamily::Gray : ColorFamily::RGB,
      .sample_type = SampleType::Integer,
      .bits = bits,
  };
  m_has_info = true;
  return true;
}

BaseDecoder::StepResult Jp2Decoder::decode_impl(const DecodeOptions &opts) {
  if (!read_header()) {
    return {};
  }

  if (m_complete) {
    return {true, 0, {}};
  }

  decode_data();

  const ImageInfo &dec = info;
  const size_t stride = layout_stride(dec, opts);
  // Sampled rows go on as they're made, so one is all that's kept.
  const bool stream = sampling_rows();
  m_buffer.assign(
      checked_buffer_bytes(stride, stream ? 1 : info.original_height), 0);

  for (uint32_t c = 0; c < dec.components; c++) {
    const opj_image_comp_t &comp = m_image->comps[c];
    if (!comp.data) {
      throw std::runtime_error("JPEG 2000 component has no data");
    }
    // A region decode's comps are the made rect, which info now is.
    if (comp.w < info.original_width || comp.h < info.original_height) {
      throw std::runtime_error("JPEG 2000 component is smaller than its image");
    }
    if (comp.prec == 0 || comp.prec > 32) {
      throw std::runtime_error("JPEG 2000 component has an invalid precision");
    }
  }

  auto convert_row = [&](uint32_t y, uint8_t *row) {
    for (uint32_t c = 0; c < dec.components; c++) {
      const opj_image_comp_t &comp = m_image->comps[c];
      const int32_t *src = comp.data + (size_t)y * comp.w;
      const int64_t shift = comp.sgnd ? ((int64_t)1 << (comp.prec - 1)) : 0;
      const int64_t max_in = ((int64_t)1 << comp.prec) - 1;
      const int64_t max_out = dec.bits == 16 ? 65535 : 255;
      for (uint32_t x = 0; x < info.original_width; x++) {
        int64_t v = std::clamp<int64_t>((int64_t)src[x] + shift, 0, max_in);
        // Scaled, not shifted, so the input's top lands on the output's.
        int64_t scaled =
            max_in == max_out ? v : (v * max_out + max_in / 2) / max_in;
        if (dec.bits == 16) {
          ((uint16_t *)row)[(size_t)x * dec.components + c] = (uint16_t)scaled;
        } else {
          row[(size_t)x * dec.components + c] = (uint8_t)scaled;
        }
      }
    }
  };

  if (stream) {
    const SrgbTransform srgb(dec, m_src_profile, opts.srgb_output);
    for (uint32_t y = 0; y < info.original_height; y++) {
      convert_row(y, m_buffer.data());
      srgb.apply(m_buffer.data(), stride, 0, 1);
      finish_layout(m_buffer.data(), stride, dec, opts, 0, 1);
      emit_rows(m_buffer.data(), stride, y, 1);
    }
  } else {
    for (uint32_t y = 0; y < info.original_height; y++) {
      convert_row(y, m_buffer.data() + stride * y);
    }
    if (opts.srgb_output) {
      SrgbTransform(dec, m_src_profile, true)
          .apply(m_buffer.data(), stride, 0, dec.original_height);
    }
    finish_layout(m_buffer.data(), stride, dec, opts, 0, dec.original_height);
  }

  // Four bytes a sample, no longer needed; a restart decodes again anyway.
  opj_image_destroy(m_image);
  m_image = nullptr;
  m_complete = true;
  return {true, stride, {}};
}

void Jp2Decoder::restart() {
  close();
  m_has_info = false;
  m_complete = false;
  m_read_pos = 0;
  m_frame = 0;
  m_duration_ms = 0;
  m_buffer.clear();
}

/* Whole resolution levels dropped: each halves the image, and the wavelet's
 * levels below are never decoded. Only where every component is on the full
 * grid and the image starts on the reduced one, so pixels stay aligned. */
uint32_t Jp2Decoder::begin_scale(uint32_t sample, uint32_t aligned,
                                 const InvalidRect &want) {
  (void)sample;
  (void)want;
  if (!m_codec || !m_image) {
    return 1;
  }
  for (uint32_t c = 0; c < m_image->numcomps; c++) {
    if (m_image->comps[c].dx != 1 || m_image->comps[c].dy != 1) {
      return 1;
    }
  }
  uint32_t r = 0;
  while ((2u << r) <= aligned && r < 31) {
    r++;
  }
  // openjpeg refuses a factor the codestream has too few levels for - after
  // storing it, so a refusal is undone by setting 0.
  bool tried = false;
  for (; r > 0; r--) {
    const uint32_t f = 1u << r;
    if (m_comp_x0 % f || m_comp_y0 % f) {
      continue;
    }
    tried = true;
    if (opj_set_decoded_resolution_factor(m_codec, r)) {
      m_scale = f;
      return f;
    }
  }
  if (tried && !opj_set_decoded_resolution_factor(m_codec, 0)) {
    throw std::runtime_error("Failed to reset JPEG 2000 resolution");
  }
  return 1;
}

InvalidRect Jp2Decoder::begin_region(const InvalidRect &want) {
  if (m_scale > 1) {
    // In reduced pixels opj makes ceil(x1 / f) - x0 / f: with x0 a multiple
    // of f, that's ceil(size / f), as the base expects.
    const uint32_t f = m_scale;
    const uint32_t x0 = want.x / f * f, y0 = want.y / f * f;
    const uint32_t x1 = want.x + want.width, y1 = want.y + want.height;
    if (!opj_set_decode_area(m_codec, m_image, (OPJ_INT32)(m_comp_x0 + x0),
                             (OPJ_INT32)(m_comp_y0 + y0),
                             (OPJ_INT32)(m_comp_x0 + x1),
                             (OPJ_INT32)(m_comp_y0 + y1))) {
      throw std::runtime_error("Failed to set JPEG 2000 decode area");
    }
    for (uint32_t c = 0; c < m_image->numcomps; c++) {
      const opj_image_comp_t &comp = m_image->comps[c];
      if (comp.w != (x1 - x0 + f - 1) / f || comp.h != (y1 - y0 + f - 1) / f) {
        throw std::runtime_error("JPEG 2000 reduced area is not as expected");
      }
    }
    m_region = InvalidRect{x0, y0, x1 - x0, y1 - y0};
    return m_region;
  }
  const opj_image_comp_t &first = m_image->comps[0];
  const uint64_t dx = first.dx, dy = first.dy;
  // Stored pixels to the reference grid; opj updates the comps to what it
  // makes.
  auto rx = [&](uint64_t x) {
    return (OPJ_INT32)std::min<uint64_t>((m_comp_x0 + x) * dx, m_image->x1);
  };
  auto ry = [&](uint64_t y) {
    return (OPJ_INT32)std::min<uint64_t>((m_comp_y0 + y) * dy, m_image->y1);
  };
  if (!opj_set_decode_area(m_codec, m_image, rx(want.x), ry(want.y),
                           rx((uint64_t)want.x + want.width),
                           ry((uint64_t)want.y + want.height))) {
    throw std::runtime_error("Failed to set JPEG 2000 decode area");
  }
  for (uint32_t c = 1; c < m_image->numcomps; c++) {
    const opj_image_comp_t &comp = m_image->comps[c];
    if (comp.x0 != first.x0 || comp.y0 != first.y0 || comp.w != first.w ||
        comp.h != first.h) {
      throw std::runtime_error("JPEG 2000 components differ in size");
    }
  }
  m_region =
      InvalidRect{first.x0 - m_comp_x0, first.y0 - m_comp_y0, first.w, first.h};
  return m_region;
}

void Jp2Decoder::decode_data() {
  if (!m_codec || !m_stream) {
    throw std::runtime_error("JPEG 2000 already decoded");
  }
  bool ok = opj_decode(m_codec, m_stream, m_image) &&
            opj_end_decompress(m_codec, m_stream);
  opj_stream_destroy(m_stream);
  opj_destroy_codec(m_codec);
  m_stream = nullptr;
  m_codec = nullptr;
  if (!ok) {
    throw std::runtime_error("Corrupt JPEG 2000");
  }
}

void Jp2Decoder::close() {
  if (m_stream) {
    opj_stream_destroy(m_stream);
    m_stream = nullptr;
  }
  if (m_codec) {
    opj_destroy_codec(m_codec);
    m_codec = nullptr;
  }
  if (m_image) {
    opj_image_destroy(m_image);
    m_image = nullptr;
  }
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
}

bool Jp2Decoder::open() {
  bool boxed = m_source.size() >= 12 && m_source.data()[0] == 0 &&
               m_source.data()[1] == 0;
  opj_codec_t *codec =
      opj_create_decompress(boxed ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
  if (!codec) {
    throw std::runtime_error("Failed to create JPEG 2000 decoder");
  }

  // Silenced: libopenjp2 would otherwise write to a CLI host's stderr.
  opj_set_error_handler(codec, nullptr, nullptr);
  opj_set_warning_handler(codec, nullptr, nullptr);
  opj_set_info_handler(codec, nullptr, nullptr);

  opj_dparameters_t params;
  opj_set_default_decoder_parameters(&params);
  if (!opj_setup_decoder(codec, &params)) {
    opj_destroy_codec(codec);
    throw std::runtime_error("Failed to configure JPEG 2000 decoder");
  }

  m_read_pos = 0;
  opj_stream_t *stream = opj_stream_default_create(OPJ_TRUE);
  if (!stream) {
    opj_destroy_codec(codec);
    throw std::runtime_error("Failed to create JPEG 2000 stream");
  }
  opj_stream_set_user_data(stream, this, nullptr);
  opj_stream_set_user_data_length(stream, (OPJ_UINT64)m_source.size());
  opj_stream_set_read_function(stream, jp2_read);
  opj_stream_set_skip_function(stream, jp2_skip);
  opj_stream_set_seek_function(stream, jp2_seek);

  opj_image_t *image = nullptr;
  bool ok = opj_read_header(stream, codec, &image) && image;
  // opj_decode allocates at the declared sizes, so bound them first.
  if (ok) {
    const uint64_t w = image->x1 > image->x0 ? image->x1 - image->x0 : 0;
    const uint64_t h = image->y1 > image->y0 ? image->y1 - image->y0 : 0;
    const bool sane = image->numcomps > 0 && w > 0 && h > 0 &&
                      w * h <= MAX_IMAGE_PIXELS &&
                      (uint64_t)image->numcomps * w * h * 4 <= MAX_IMAGE_BYTES;
    if (!sane) {
      const char *why = "JPEG 2000 image is too large";
      opj_stream_destroy(stream);
      opj_destroy_codec(codec);
      opj_image_destroy(image);
      throw std::runtime_error(why);
    }
  }
  if (!ok) {
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    if (image) {
      opj_image_destroy(image);
    }
    throw std::runtime_error("Corrupt JPEG 2000");
  }

  m_codec = codec;
  m_stream = stream;
  m_image = image;
  return true;
}

} // namespace imagedecoder
