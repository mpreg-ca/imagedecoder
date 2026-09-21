#include "decoder_jxl.h"

#include <algorithm>

namespace imagedecoder {

JpegXlDecoder::JpegXlDecoder() { create(); }

JpegXlDecoder::~JpegXlDecoder() { close(); }

bool JpegXlDecoder::read_header() {
  // The colour encoding decides HDR and the profile, so it is part of it.
  if (m_has_info && m_colour_read) {
    return true;
  }

  refresh_input();

  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(m_dec);

    if (status == JXL_DEC_BASIC_INFO) {
      read_basic_info();
      continue;
    }
    if (status == JXL_DEC_COLOR_ENCODING) {
      read_colour();
      m_colour_read = true;
      break;
    }
    if (status == JXL_DEC_NEED_MORE_INPUT) {
      if (m_source.complete()) {
        throw std::runtime_error("Truncated JXL: no header");
      }
      return false;
    }
    if (status == JXL_DEC_ERROR) {
      throw std::runtime_error("Invalid JXL");
    }
    if (status == JXL_DEC_SUCCESS) {
      break;
    }
    break;
  }

  return m_has_info && m_colour_read;
}

BaseDecoder::StepResult JpegXlDecoder::decode_impl(const DecodeOptions &opts) {
  if (!m_runner || m_retried_unthreaded) {
    return decode_threaded(opts);
  }

  try {
    return decode_threaded(opts);
  } catch (const std::exception &) {
    if (m_emitted) {
      throw;
    }
    m_retried_unthreaded = true;
    std::vector<uint8_t>().swap(m_buffer);
    restart();
    return decode_threaded(opts);
  }
}

BaseDecoder::StepResult
JpegXlDecoder::decode_threaded(const DecodeOptions &opts) {
  DirtyRegion dirty;
  if (!read_header()) {
    return {};
  }

  if (m_complete) {
    return {true, 0, {}};
  }

  if (m_row_stride == 0) {
    // libjxl redraws into m_raw, so conversion always starts from raw rows.
    const size_t stride = layout_stride(info, opts);
    const size_t bytes = checked_buffer_bytes(stride, info.original_height);
    m_raw.assign(checked_buffer_bytes(row_bytes(info), info.original_height),
                 0);
    m_srgb = SrgbTransform(info, m_src_profile, opts.srgb_output);
    m_buffer.assign(bytes, 0);
    m_row_stride = stride;
  }

  JxlPixelFormat format = {info.components,
                           info.bits == 16 ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8,
                           JXL_NATIVE_ENDIAN, 0};

  refresh_input();
  m_frame_ready = false;

  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(m_dec);

    if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      if (JxlDecoderSetImageOutBuffer(m_dec, &format, m_raw.data(),
                                      m_raw.size()) != JXL_DEC_SUCCESS) {
        throw std::runtime_error("Failed to set JXL output buffer");
      }
      continue;
    }

    if (status == JXL_DEC_FRAME) {
      // A new frame's passes carry its own index, not the last one's.
      if (m_advance_frame) {
        m_advance_frame = false;
        m_frame++;
      }
      JxlFrameHeader header = {};
      const bool got =
          JxlDecoderGetFrameHeader(m_dec, &header) == JXL_DEC_SUCCESS;
      m_last_frame = got && header.is_last;
      if (got && m_basic.have_animation &&
          m_basic.animation.tps_numerator > 0) {
        // In double: 32-bit ABIs have no 128-bit integer to hold the product.
        const double ms = (double)header.duration * 1000.0 *
                          m_basic.animation.tps_denominator /
                          m_basic.animation.tps_numerator;
        m_duration_ms =
            ms >= MAX_FRAME_DURATION_MS ? MAX_FRAME_DURATION_MS : (uint32_t)ms;
      }
      continue;
    }

    if (status == JXL_DEC_FRAME_PROGRESSION) {
      if (JxlDecoderFlushImage(m_dec) != JXL_DEC_SUCCESS) {
        continue;
      }

      present(opts);
      m_emitted = true;
      return {false, 0,
              InvalidRect{0, 0, info.original_width, info.original_height},
              m_row_stride};
    }

    if (status == JXL_DEC_FULL_IMAGE) {
      present(opts);
      dirty.replace(
          InvalidRect{0, 0, info.original_width, info.original_height});
      m_frame_ready = true;
      break;
    }

    if (status == JXL_DEC_NEED_MORE_INPUT) {
      if (m_source.complete()) {
        throw std::runtime_error("Truncated JXL");
      }
      return {};
    }

    if (status == JXL_DEC_SUCCESS) {
      m_complete = true;
      break;
    }

    if (status == JXL_DEC_ERROR) {
      throw std::runtime_error("Corrupt JXL");
    }
  }

  if (m_frame_ready) {
    if (m_advance_frame) {
      m_advance_frame = false;
      m_frame++;
    }

    if (m_frame >= MAX_FRAMES) {
      throw std::runtime_error("Image has too many frames");
    }

    // Exact at the last frame; before it, at least one more.
    const bool last = !m_basic.have_animation || m_last_frame;
    m_frame_count = last ? m_frame + 1 : std::max(m_frame_count, m_frame + 2);

    m_emitted = true;
    if (last) {
      m_complete = true;
      std::vector<uint8_t>().swap(m_raw); // Done with; restart() remakes it.
      return {true, m_row_stride, {}};
    }

    m_advance_frame = true;
    return {false, m_row_stride, {}};
  }

  // The last frame was already reported when it finished.
  if (m_complete) {
    return {true, 0, {}};
  }
  return {false, 0, dirty.rect(), m_row_stride};
}

void JpegXlDecoder::present(const DecodeOptions &opts) {
  const size_t raw_stride = row_bytes(info);
  const uint32_t h = info.original_height;
  // decode() compacts or turns a finished frame, so it is laid out again.
  m_buffer.resize(m_row_stride * h);
  if (raw_stride == m_row_stride) {
    memcpy(m_buffer.data(), m_raw.data(), raw_stride * h);
  } else {
    for (uint32_t y = 0; y < h; y++) {
      memcpy(m_buffer.data() + m_row_stride * y, m_raw.data() + raw_stride * y,
             raw_stride);
    }
  }
  m_srgb.apply(m_buffer.data(), m_row_stride, 0, h);
  finish_layout(m_buffer.data(), m_row_stride, info, opts, 0, h);
}

void JpegXlDecoder::restart() {
  close();
  m_basic = JxlBasicInfo{};
  m_has_info = false;
  m_complete = false;
  m_frame_ready = false;
  m_frame_count = 1;
  m_hdr_kind = HdrKind::None;
  m_hdr_headroom = 0.0f;
  m_primaries = CICP_PRIMARIES_BT709;
  m_frame = 0;
  m_advance_frame = false;
  m_duration_ms = 0;
  m_row_stride = 0;
  m_srgb = SrgbTransform();
  m_attached = false;
  m_closed = false;
  m_attached_end = 0;
  m_colour_read = false;
  m_last_frame = false;
  m_emitted = false;
  m_raw.clear();
  m_buffer.clear();
  m_exif.clear();
  m_source.consumed = 0;
  create();
}

void JpegXlDecoder::create() {
  m_dec = JxlDecoderCreate(nullptr);
  if (!m_dec) {
    throw std::runtime_error("Failed to create JXL decoder");
  }

#ifdef IMAGEDECODER_SINGLE_THREADED
  // Even one worker is a thread; without a runner libjxl decodes on this one.
  m_runner = nullptr;
#else
  m_runner = m_retried_unthreaded ? nullptr
                                  : JxlResizableParallelRunnerCreate(nullptr);
#endif
  if (m_runner && JxlDecoderSetParallelRunner(m_dec, JxlResizableParallelRunner,
                                              m_runner) != JXL_DEC_SUCCESS) {
    JxlResizableParallelRunnerDestroy(m_runner);
    m_runner = nullptr;
  }

  if (JxlDecoderSubscribeEvents(m_dec,
                                JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING |
                                    JXL_DEC_FRAME | JXL_DEC_FRAME_PROGRESSION |
                                    JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("Failed to configure JXL decoder");
  }

  // Straight alpha is what every output layout holds.
  JxlDecoderSetUnpremultiplyAlpha(m_dec, JXL_TRUE);

  if (JxlDecoderSetProgressiveDetail(m_dec, kPasses) != JXL_DEC_SUCCESS) {
    JxlDecoderSetProgressiveDetail(m_dec, kDC);
  }
}

void JpegXlDecoder::close() {
  if (m_dec) {
    JxlDecoderDestroy(m_dec);
    m_dec = nullptr;
  }
  if (m_runner) {
    JxlResizableParallelRunnerDestroy(m_runner);
    m_runner = nullptr;
  }
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
}

void JpegXlDecoder::refresh_input() {
  if (m_closed) {
    return;
  }

  if (m_attached) {
    m_source.consumed = m_attached_end - JxlDecoderReleaseInput(m_dec);
    m_attached = false;
  }

  JxlDecoderSetInput(m_dec, m_source.data() + m_source.consumed,
                     m_source.available());
  m_attached = true;
  m_attached_end = m_source.size();

  if (m_source.complete()) {
    JxlDecoderCloseInput(m_dec);
    m_closed = true;
  }
}

void JpegXlDecoder::read_basic_info() {
  if (JxlDecoderGetBasicInfo(m_dec, &m_basic) != JXL_DEC_SUCCESS) {
    throw std::runtime_error("Invalid JXL: no basic info");
  }

  check_dimensions(m_basic.xsize, m_basic.ysize);

  if (m_runner) {
    uint32_t threads =
        JxlResizableParallelRunnerSuggestThreads(m_basic.xsize, m_basic.ysize);
    if (threads > MAX_DECODE_THREADS) {
      threads = MAX_DECODE_THREADS;
    }
    JxlResizableParallelRunnerSetThreads(m_runner, threads);
  }

  bool alpha = m_basic.alpha_bits > 0;
  const bool gray = m_basic.num_color_channels == 1;
  uint32_t bits = m_basic.bits_per_sample > 8 ? 16 : 8;

  // The header says animated, not how many frames: at least 2 until counted.
  if (m_basic.have_animation) {
    m_frame_count = 2;
  }

  info = {
      .width = m_basic.xsize,
      .height = m_basic.ysize,
      .original_width = m_basic.xsize,
      .original_height = m_basic.ysize,
      .components = gray ? (alpha ? 2u : 1u) : (alpha ? 4u : 3u),
      .has_alpha = alpha,
      .color = gray ? ColorFamily::Gray : ColorFamily::RGB,
      .sample_type = SampleType::Integer,
      .bits = bits,
  };
  m_has_info = true;
}

void JpegXlDecoder::read_colour() {
  size_t icc_size = 0;
  if (JxlDecoderGetICCProfileSize(m_dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                  &icc_size) == JXL_DEC_SUCCESS &&
      icc_size > 0 && icc_size < (size_t)64 * 1024 * 1024) {
    std::vector<uint8_t> icc(icc_size);
    if (JxlDecoderGetColorAsICCProfile(m_dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                       icc.data(),
                                       icc.size()) == JXL_DEC_SUCCESS) {
      m_src_profile =
          cmsOpenProfileFromMem(icc.data(), (cmsUInt32Number)icc.size());
      // The data decides the output; a profile of the wrong family is dropped.
      const cmsColorSpaceSignature want =
          info.color == ColorFamily::Gray ? cmsSigGrayData : cmsSigRgbData;
      if (m_src_profile && cmsGetColorSpace(m_src_profile) != want) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      }
    }
  }

  JxlColorEncoding encoding = {};
  if (JxlDecoderGetColorAsEncodedProfile(m_dec, JXL_COLOR_PROFILE_TARGET_DATA,
                                         &encoding) == JXL_DEC_SUCCESS) {
    switch (encoding.transfer_function) {
    case JXL_TRANSFER_FUNCTION_PQ:
      m_hdr_kind = HdrKind::PQ;
      break;
    case JXL_TRANSFER_FUNCTION_HLG:
      m_hdr_kind = HdrKind::HLG;
      break;
    case JXL_TRANSFER_FUNCTION_LINEAR:
      if (info.bits > 8) {
        m_hdr_kind = HdrKind::Linear;
      }
      break;
    default:
      break;
    }

    switch (encoding.primaries) {
    case JXL_PRIMARIES_2100:
      m_primaries = CICP_PRIMARIES_BT2020;
      break;
    case JXL_PRIMARIES_P3:
      m_primaries = CICP_PRIMARIES_P3;
      break;
    default:
      m_primaries = CICP_PRIMARIES_BT709;
      break;
    }

    m_hdr_headroom = hdr_headroom_for(m_hdr_kind, 0.0f);
  }
}

} // namespace imagedecoder
