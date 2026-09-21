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

  const ImageInfo &dec = info;
  const size_t stride = layout_stride(dec, opts);
  m_buffer.assign(checked_buffer_bytes(stride, info.original_height), 0);

  for (uint32_t c = 0; c < dec.components; c++) {
    const opj_image_comp_t &comp = m_image->comps[c];
    const int32_t *src = comp.data;
    if (!src) {
      throw std::runtime_error("JPEG 2000 component has no data");
    }

    if (comp.w < info.original_width || comp.h < info.original_height) {
      throw std::runtime_error("JPEG 2000 component is smaller than its image");
    }
    if (comp.prec == 0 || comp.prec > 32) {
      throw std::runtime_error("JPEG 2000 component has an invalid precision");
    }

    const int64_t shift = comp.sgnd ? ((int64_t)1 << (comp.prec - 1)) : 0;
    const int64_t max_in = ((int64_t)1 << comp.prec) - 1;
    const uint32_t out_bits = dec.bits;
    const int64_t max_out = out_bits == 16 ? 65535 : 255;

    for (uint32_t y = 0; y < info.original_height; y++) {
      uint8_t *row = m_buffer.data() + stride * y;
      for (uint32_t x = 0; x < info.original_width; x++) {
        int64_t v = (int64_t)src[(size_t)y * comp.w + x] + shift;
        v = std::clamp<int64_t>(v, 0, max_in);
        // Scaled, not shifted, so the input's top lands on the output's.
        int64_t scaled =
            max_in == max_out ? v : (v * max_out + max_in / 2) / max_in;

        if (out_bits == 16) {
          ((uint16_t *)row)[(size_t)x * dec.components + c] = (uint16_t)scaled;
        } else {
          row[(size_t)x * dec.components + c] = (uint8_t)scaled;
        }
      }
    }
  }

  if (opts.srgb_output) {
    SrgbTransform(dec, m_src_profile, true)
        .apply(m_buffer.data(), stride, 0, dec.original_height);
  }

  finish_layout(m_buffer.data(), stride, dec, opts, 0, dec.original_height);

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

void Jp2Decoder::close() {
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
  ok = ok && opj_decode(codec, stream, image) &&
       opj_end_decompress(codec, stream);

  opj_stream_destroy(stream);
  opj_destroy_codec(codec);

  if (!ok) {
    if (image) {
      opj_image_destroy(image);
    }
    throw std::runtime_error("Corrupt JPEG 2000");
  }

  m_image = image;
  return true;
}

} // namespace imagedecoder
