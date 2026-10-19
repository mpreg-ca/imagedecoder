#include "decoder_webp.h"
#include "exif.h"

#include <algorithm>
#include <string.h>

namespace imagedecoder {

WebpDecoder::WebpDecoder() { WebPInitDecoderConfig(&m_config); }

WebpDecoder::~WebpDecoder() { close(); }

bool WebpDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  WebPBitstreamFeatures features;
  VP8StatusCode status =
      WebPGetFeatures(m_source.data(), m_source.size(), &features);
  if (status == VP8_STATUS_NOT_ENOUGH_DATA) {
    if (m_source.complete()) {
      throw std::runtime_error("Truncated WebP: no header");
    }
    return false;
  }
  if (status != VP8_STATUS_OK) {
    throw std::runtime_error("Invalid WebP");
  }

  check_dimensions(features.width < 0 ? 0 : (uint64_t)features.width,
                   features.height < 0 ? 0 : (uint64_t)features.height);

  m_animated = features.has_animation != 0;
  m_lossy = features.format == 1;

  bool alpha = features.has_alpha != 0 || m_animated;

  if (m_animated) {
    if (m_source.complete()) {
      WebPData data = {m_source.data(), m_source.size()};
      WebPDemuxer *demux = WebPDemux(&data);
      if (demux) {
        uint32_t frames = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
        m_frame_count = checked_frame_count(frames);
        m_loop_count = WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);
        WebPDemuxDelete(demux);
      }
    } else {
      return false;
    }
  }

  info = {
      .width = (uint32_t)features.width,
      .height = (uint32_t)features.height,
      .original_width = (uint32_t)features.width,
      .original_height = (uint32_t)features.height,
      .components = alpha ? 4u : 3u,
      .has_alpha = alpha,
      .color = ColorFamily::RGB,
      .sample_type = SampleType::Integer,
      .bits = 8,
  };
  if (m_source.complete()) {
    read_metadata();
  }

  m_has_info = true;
  return true;
}

BaseDecoder::StepResult WebpDecoder::decode_impl(const DecodeOptions &opts) {

  if (!read_header()) {
    return {};
  }

  if (m_complete) {
    return {true, 0, {}};
  }

  if (m_row_stride == 0) {
    const size_t stride = layout_stride(info, opts);
    m_buffer.assign(checked_buffer_bytes(stride, info.original_height), 0);
    m_row_stride = stride;
  }

  return m_animated ? decode_animation(opts) : decode_still(opts);
}

// WebPAnimDecoderReset keeps the demuxed animation: the next frame is 0 again.
void WebpDecoder::rewind_codec() {
  if (!m_anim) {
    restart();
    return;
  }
  WebPAnimDecoderReset(m_anim);
  m_complete = false;
  m_prev_timestamp = 0;
  m_next_frame = 0;
  // The next decode may lay out under other options.
  m_row_stride = 0;
  m_srgb_ready = false;
}

/* Lossy crops start on even coordinates, and fancy upsampling replicates
 * chroma at crop edges, so a 2px margin keeps edge pixels identical. */
InvalidRect WebpDecoder::begin_region(const InvalidRect &want) {
  if (m_animated) {
    return BaseDecoder::begin_region(want);
  }
  if (m_scale > 1) {
    // want is whole blocks. The lossy margin is whole blocks too, and even,
    // so libwebp scales by exactly the sample and the crop starts even.
    const uint32_t m = m_lossy ? (m_scale % 2 ? 2 * m_scale : m_scale) : 0;
    const uint32_t x1 = want.x + want.width, y1 = want.y + want.height;
    const uint32_t x0 = want.x >= m ? want.x - m : want.x;
    const uint32_t y0 = want.y >= m ? want.y - m : want.y;
    const uint32_t x2 = info.original_width - x1 >= m ? x1 + m : x1;
    const uint32_t y2 = info.original_height - y1 >= m ? y1 + m : y1;
    m_region = InvalidRect{x0, y0, x2 - x0, y2 - y0};
    return m_region;
  }
  uint32_t x0 = want.x, y0 = want.y;
  uint32_t x1 = want.x + want.width, y1 = want.y + want.height;
  if (m_lossy) {
    x0 = (x0 & ~1u) >= 2 ? (x0 & ~1u) - 2 : 0;
    y0 = (y0 & ~1u) >= 2 ? (y0 & ~1u) - 2 : 0;
    x1 = std::min(x1 + 2, info.original_width);
    y1 = std::min(y1 + 2, info.original_height);
  }
  m_region = InvalidRect{x0, y0, x1 - x0, y1 - y0};
  return m_region;
}

/* libwebp crops, then area-averages the crop down to ceil(size / sample) as
 * rows come, so the region never exists at full size. A lossy crop must
 * start on even coordinates. */
uint32_t WebpDecoder::begin_scale(uint32_t sample, uint32_t aligned,
                                  const InvalidRect &want) {
  (void)aligned;
  if (m_animated || (m_lossy && ((want.x | want.y) & 1))) {
    return 1;
  }
  m_scale = sample;
  return m_scale;
}

void WebpDecoder::restart() {
  close();
  WebPInitDecoderConfig(&m_config);
  m_has_info = false;
  m_complete = false;
  m_prev_timestamp = 0;
  m_frame = 0;
  m_next_frame = 0;
  m_rows_done = 0;
  m_srgb_ready = false;
  m_srgb = SrgbTransform();
  m_row_stride = 0;
  m_duration_ms = 0;
  m_buffer.clear();
  m_exif.clear();
  m_metadata_read = false;
  m_source.consumed = 0;
}

void WebpDecoder::close() {
  if (m_idec) {
    WebPIDelete(m_idec);
    m_idec = nullptr;
  }
  if (m_anim) {
    WebPAnimDecoderDelete(m_anim);
    m_anim = nullptr;
  }
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
}

void WebpDecoder::read_metadata() {
  if (m_metadata_read) {
    return;
  }
  // Before completion only the ICC profile, which precedes the image, is read.
  m_metadata_read = m_source.complete();
  WebPData data = {m_source.data(), m_source.size()};
  WebPDemuxState state;
  WebPDemuxer *demux = WebPDemuxPartial(&data, &state);
  if (!demux) {
    return;
  }

  uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);

  WebPChunkIterator chunk;
  if ((flags & ICCP_FLAG) && WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
    if (chunk.chunk.bytes && chunk.chunk.size > 0) {
      if (m_src_profile) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      }
      m_src_profile = cmsOpenProfileFromMem(chunk.chunk.bytes,
                                            (cmsUInt32Number)chunk.chunk.size);
      if (m_src_profile && cmsGetColorSpace(m_src_profile) != cmsSigRgbData) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      }
    }
    WebPDemuxReleaseChunkIterator(&chunk);
  }

  if ((flags & EXIF_FLAG) && WebPDemuxGetChunk(demux, "EXIF", 1, &chunk)) {
    if (chunk.chunk.bytes && chunk.chunk.size > 0) {
      m_exif.assign(chunk.chunk.bytes, chunk.chunk.bytes + chunk.chunk.size);
    }
    WebPDemuxReleaseChunkIterator(&chunk);
  }

  WebPDemuxDelete(demux);

  info.orientation = (uint32_t)exif_orientation(m_exif.data(), m_exif.size());
}

BaseDecoder::StepResult WebpDecoder::decode_still(const DecodeOptions &opts) {
  DirtyRegion dirty;
  if (!m_idec) {
    m_config.output.colorspace = info.components == 4 ? MODE_RGBA : MODE_RGB;
    m_config.output.is_external_memory = 1;
    m_config.output.u.RGBA.rgba = m_buffer.data();
    m_config.output.u.RGBA.stride = (int)m_row_stride;
    m_config.output.u.RGBA.size = m_buffer.size();

    if (m_region.width > 0) {
      m_config.options.use_cropping = 1;
      m_config.options.crop_left = (int)m_region.x;
      m_config.options.crop_top = (int)m_region.y;
      m_config.options.crop_width = (int)m_region.width;
      m_config.options.crop_height = (int)m_region.height;
      if (m_scale > 1) {
        m_config.options.use_scaling = 1;
        m_config.options.scaled_width = (int)info.original_width;
        m_config.options.scaled_height = (int)info.original_height;
      }
    }
    m_idec = WebPIDecode(nullptr, 0, &m_config);
    if (!m_idec) {
      throw std::runtime_error("Failed to create WebP decoder");
    }
  }

  size_t available = m_source.available();
  VP8StatusCode status = VP8_STATUS_SUSPENDED;
  if (available > 0) {
    status =
        WebPIAppend(m_idec, m_source.data() + m_source.consumed, available);
    m_source.consumed += available;
  }

  if (status != VP8_STATUS_OK && status != VP8_STATUS_SUSPENDED) {
    throw std::runtime_error("Corrupt WebP");
  }

  int last_y = 0;
  int width = 0;
  int height = 0;
  int stride = 0;
  const uint8_t *rows_ptr =
      WebPIDecGetRGB(m_idec, &last_y, &width, &height, &stride);

  if (last_y < 0) {
    last_y = 0;
  }
  if (rows_ptr && last_y > 0) {
    if (width != (int)info.original_width ||
        height != (int)info.original_height) {
      throw std::runtime_error("WebP frame does not match its header");
    }
    if (stride < 0 || (size_t)stride != m_row_stride) {
      throw std::runtime_error("WebP stride does not match its buffer");
    }
  }
  if (last_y > (int)info.original_height) {
    last_y = (int)info.original_height;
  }

  const bool done = status == VP8_STATUS_OK;
  const uint32_t rows = done ? info.original_height : (uint32_t)last_y;

  // Rows above last_y are final, so each is converted once as it lands.
  if (rows > m_rows_done) {
    prepare_srgb(opts);
    const uint32_t n = rows - m_rows_done;
    m_srgb.apply(m_buffer.data(), m_row_stride, m_rows_done, n);
    finish_layout(m_buffer.data(), m_row_stride, info, opts, m_rows_done, n);
    dirty.add(InvalidRect{0, m_rows_done, info.original_width, n});
    m_rows_done = rows;
  }

  if (!done) {
    if (m_source.complete()) {
      throw std::runtime_error("Truncated WebP");
    }
    return {false, 0, dirty.rect(), m_row_stride};
  }

  read_metadata(); // Exif, for the orientation, trails the image.
  WebPIDelete(m_idec);
  m_idec = nullptr;
  m_complete = true;
  return {true, m_row_stride, {}};
}

void WebpDecoder::prepare_srgb(const DecodeOptions &opts) {
  if (!m_srgb_ready) {
    read_metadata();
    m_srgb = SrgbTransform(info, m_src_profile, opts.srgb_output);
    m_srgb_ready = true;
  }
}

BaseDecoder::StepResult
WebpDecoder::decode_animation(const DecodeOptions &opts) {
  if (!m_source.complete()) {
    return {};
  }

  if (!m_anim) {
    read_metadata();

    WebPAnimDecoderOptions anim_opts;
    if (!WebPAnimDecoderOptionsInit(&anim_opts)) {
      throw std::runtime_error("Failed to initialise WebP animation decoder");
    }
    anim_opts.color_mode = MODE_RGBA;
    anim_opts.use_threads = 0;

    WebPData data = {m_source.data(), m_source.size()};
    m_anim = WebPAnimDecoderNew(&data, &anim_opts);
    if (!m_anim) {
      throw std::runtime_error("Invalid animated WebP");
    }

    WebPAnimInfo anim_info;
    if (!WebPAnimDecoderGetInfo(m_anim, &anim_info)) {
      throw std::runtime_error("Failed to read WebP animation info");
    }
    if (anim_info.frame_count == 0) {
      throw std::runtime_error("WebP animation has no frames");
    }
    if (anim_info.canvas_width != info.original_width ||
        anim_info.canvas_height != info.original_height) {
      throw std::runtime_error(
          "WebP animation canvas does not match its header");
    }
    m_frame_count = checked_frame_count(anim_info.frame_count);
    m_loop_count = anim_info.loop_count;
    m_prev_timestamp = 0;
    m_next_frame = 0;
  }

  if (!WebPAnimDecoderHasMoreFrames(m_anim)) {
    m_complete = true;
    return {true, 0, {}};
  }
  prepare_srgb(opts);

  uint8_t *frame = nullptr;
  int timestamp = 0;
  if (!WebPAnimDecoderGetNext(m_anim, &frame, &timestamp)) {
    throw std::runtime_error("Corrupt animated WebP");
  }

  const size_t canvas_stride = (size_t)info.original_width * 4;
  // decode() compacted the last frame; every row is overwritten again.
  m_buffer.resize(m_row_stride * info.original_height);
  if (canvas_stride == m_row_stride) {
    memcpy(m_buffer.data(), frame, canvas_stride * info.original_height);
  } else {
    for (uint32_t y = 0; y < info.original_height; y++) {
      memcpy(m_buffer.data() + m_row_stride * y, frame + canvas_stride * y,
             canvas_stride);
    }
  }

  m_srgb.apply(m_buffer.data(), m_row_stride, 0, info.original_height);
  finish_layout(m_buffer.data(), m_row_stride, info, opts, 0,
                info.original_height);

  m_duration_ms =
      std::min<uint32_t>((uint32_t)std::max(0, timestamp - m_prev_timestamp),
                         MAX_FRAME_DURATION_MS);
  m_prev_timestamp = timestamp;
  m_frame = m_next_frame++;

  const bool last = !WebPAnimDecoderHasMoreFrames(m_anim);
  if (last) {
    m_complete = true;
  }
  return {last, m_row_stride, {}};
}

} // namespace imagedecoder
