#include "decoder_gif.h"

#include <algorithm>
#include <string.h>

namespace imagedecoder {

struct GifReadState {
  const uint8_t *data;
  size_t size;
  size_t pos;
};

static int gif_read(GifFileType *gif, GifByteType *out, int len) {
  auto *s = (GifReadState *)gif->UserData;
  if (len <= 0 || s->pos >= s->size) {
    return 0;
  }
  size_t n = std::min((size_t)len, s->size - s->pos);
  memcpy(out, s->data + s->pos, n);
  s->pos += n;
  return (int)n;
}

// DGifSlurp holds every raster at once, so their total is bounded first.
static void check_gif_budget(const uint8_t *data, size_t size) {
  static const uint64_t MAX_RASTER_BYTES = 1ull << 30;
  size_t pos = 13;
  auto skip_table = [&](uint8_t flags) {
    if (flags & 0x80) {
      pos += (size_t)3 << ((flags & 7) + 1);
    }
  };
  auto skip_blocks = [&] {
    while (pos < size && data[pos] != 0) {
      pos += (size_t)data[pos] + 1;
    }
    pos++;
  };
  if (size < 13) {
    return;
  }
  skip_table(data[10]);
  const uint64_t canvas =
      (uint64_t)(data[6] | data[7] << 8) * (uint64_t)(data[8] | data[9] << 8);

  uint64_t frames = 0;
  uint64_t raster = 0;
  while (pos < size) {
    const uint8_t kind = data[pos++];
    if (kind == 0x21) {
      pos++; // label
      skip_blocks();
    } else if (kind == 0x2C) {
      if (pos + 9 > size) {
        return;
      }
      const uint32_t w = data[pos + 4] | (uint32_t)data[pos + 5] << 8;
      const uint32_t h = data[pos + 6] | (uint32_t)data[pos + 7] << 8;
      const uint8_t flags = data[pos + 8];
      pos += 9;
      skip_table(flags);
      pos++; // LZW minimum code size
      skip_blocks();
      raster += (uint64_t)w * h;
      if (++frames > MAX_FRAMES || frames * canvas > MAX_ANIMATION_PIXELS) {
        throw std::runtime_error("GIF has too many frames");
      }
      if (raster > MAX_RASTER_BYTES) {
        throw std::runtime_error("GIF frames are too large in total");
      }
    } else {
      return; // Trailer, or something giflib will reject itself.
    }
  }
}

GifDecoder::~GifDecoder() { close(); }

bool GifDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (!m_source.complete()) {
    return false;
  }

  close(); // A header that threw last time left its handles behind.
  check_gif_budget(m_source.data(), m_source.size());

  auto *state = new GifReadState{m_source.data(), m_source.size(), 0};
  m_read_state = state;

  int error = 0;
  m_gif = DGifOpen(state, gif_read, &error);
  if (!m_gif) {
    throw std::runtime_error("Invalid GIF");
  }

  // On failure giflib leaks a bounded amount internally; nothing to do here.
  if (DGifSlurp(m_gif) != GIF_OK) {
    throw std::runtime_error("Corrupt GIF");
  }

  check_dimensions(m_gif->SWidth < 0 ? 0 : (uint64_t)m_gif->SWidth,
                   m_gif->SHeight < 0 ? 0 : (uint64_t)m_gif->SHeight);
  if (m_gif->ImageCount <= 0) {
    throw std::runtime_error("GIF has no frames");
  }

  m_frame_count = checked_frame_count((uint64_t)m_gif->ImageCount);

  // No NETSCAPE block plays once; its count is repeats after the first play.
  m_loop_count = 1;
  for (int i = 0; i < m_gif->ImageCount; i++) {
    bool found = false;
    const SavedImage &img = m_gif->SavedImages[i];
    for (int e = 0; e + 1 < img.ExtensionBlockCount; e++) {
      const ExtensionBlock &b = img.ExtensionBlocks[e];
      if (b.Function != APPLICATION_EXT_FUNC_CODE || b.ByteCount < 11) {
        continue;
      }
      if (memcmp(b.Bytes, "NETSCAPE2.0", 11) != 0) {
        continue;
      }
      const ExtensionBlock &sub = img.ExtensionBlocks[e + 1];
      if (sub.ByteCount >= 3 && sub.Bytes[0] == 1) {
        const uint32_t repeats =
            (uint32_t)sub.Bytes[1] | ((uint32_t)sub.Bytes[2] << 8);
        m_loop_count = repeats == 0 ? 0 : repeats + 1;
      }
      found = true;
      break;
    }
    if (found) {
      break;
    }
  }

  info = {
      .width = (uint32_t)m_gif->SWidth,
      .height = (uint32_t)m_gif->SHeight,
      .original_width = (uint32_t)m_gif->SWidth,
      .original_height = (uint32_t)m_gif->SHeight,
      .components = 4,
      .has_alpha = true,
      .color = ColorFamily::RGB,
      .sample_type = SampleType::Integer,
      .bits = 8,
  };
  m_has_info = true;
  return true;
}

BaseDecoder::StepResult GifDecoder::decode_impl(const DecodeOptions &opts) {
  if (!read_header()) {
    return {};
  }

  if (m_complete) {
    return {true, 0, {}};
  }

  // Canvas stays 8-bit RGBA whatever was asked for; converted per step.
  if (m_canvas.empty()) {
    m_canvas_stride = (size_t)info.original_width * 4;
    checked_buffer_size(info);
    m_canvas.assign(m_canvas_stride * info.original_height, 0);
  }

  m_frame = m_next_frame;
  compose_frame(m_next_frame);
  m_next_frame++;

  const size_t work_stride = layout_stride(info, opts);
  if (sampling_rows()) {
    // Only the first frame: the canvas is not needed again.
    emit_frame(m_canvas.data(), m_canvas_stride, info, opts, nullptr,
               info.original_height);
    m_complete = true;
    return {true, work_stride, {}};
  }
  // Stale bytes only in row padding, which decode() compacts away.
  m_buffer.resize(checked_buffer_bytes(work_stride, info.original_height));
  if (work_stride == m_canvas_stride) {
    memcpy(m_buffer.data(), m_canvas.data(), m_canvas.size());
  } else {
    for (uint32_t y = 0; y < info.original_height; y++) {
      memcpy(m_buffer.data() + work_stride * y,
             m_canvas.data() + m_canvas_stride * y, m_canvas_stride);
    }
  }

  finish_layout(m_buffer.data(), work_stride, info, opts, 0,
                info.original_height);
  bool last = m_next_frame >= m_frame_count;
  if (last) {
    m_complete = true;
  }
  return {last, work_stride, {}};
}

InvalidRect GifDecoder::begin_region(const InvalidRect &want) {
  // DGifSlurp already paid for the LZW; only the region is composited.
  m_region = want;
  return m_region;
}

// The slurped rasters stay; only compositing starts over.
void GifDecoder::rewind_codec() {
  if (!m_has_info) {
    restart();
    return;
  }
  m_complete = false;
  m_next_frame = 0;
  m_prev_dispose = DISPOSAL_UNSPECIFIED;
  m_prev_w = 0;
  m_prev_h = 0;
  m_have_saved_canvas = false;
  std::vector<uint8_t>().swap(m_saved_canvas);
  m_frame = 0;
  m_duration_ms = 0;
  // Freed, not just emptied: a decoder may sit idle long after a rewind.
  std::vector<uint8_t>().swap(m_buffer);
  std::vector<uint8_t>().swap(m_canvas);
  m_canvas_stride = 0;
}

void GifDecoder::restart() {
  close();
  m_has_info = false;
  m_complete = false;
  m_next_frame = 0;
  m_frame_count = 1;
  m_loop_count = 0;
  m_prev_dispose = DISPOSAL_UNSPECIFIED;
  m_prev_w = 0;
  m_prev_h = 0;
  m_have_saved_canvas = false;
  m_saved_canvas.clear();
  m_frame = 0;
  m_duration_ms = 0;
  m_buffer.clear();
  m_canvas.clear();
  m_canvas_stride = 0;
}

void GifDecoder::close() {
  if (m_gif) {
    int error = 0;
    DGifCloseFile(m_gif, &error);
    m_gif = nullptr;
  }
  delete (GifReadState *)m_read_state;
  m_read_state = nullptr;
}

void GifDecoder::compose_frame(uint32_t index) {
  const SavedImage &image = m_gif->SavedImages[index];
  const GifImageDesc &desc = image.ImageDesc;

  if (m_have_saved_canvas) {
    m_canvas = m_saved_canvas;
    m_have_saved_canvas = false;
  } else if (m_prev_dispose == DISPOSE_BACKGROUND && m_prev_w > 0) {
    for (uint32_t r = 0; r < m_prev_h; r++) {
      uint32_t cy = m_prev_y + r;
      if (cy >= info.original_height) {
        break;
      }
      const size_t offset = (size_t)m_prev_x * 4;
      if (offset >= m_canvas_stride) {
        break;
      }
      uint8_t *at = m_canvas.data() + m_canvas_stride * cy + offset;
      const size_t room = m_canvas_stride - offset;
      memset(at, 0, std::min<size_t>((size_t)m_prev_w * 4, room));
    }
  }

  GraphicsControlBlock gcb;
  int transparent = NO_TRANSPARENT_COLOR;
  int dispose = DISPOSAL_UNSPECIFIED;
  uint32_t delay_ms = 0;
  if (DGifSavedExtensionToGCB(m_gif, (int)index, &gcb) == GIF_OK) {
    transparent = gcb.TransparentColor;
    dispose = gcb.DisposalMode;
    // Some encoders write restore-previous as 4, not the spec's 3.
    if (dispose == 4) {
      dispose = DISPOSE_PREVIOUS;
    } else if (dispose > 4) {
      dispose = DISPOSAL_UNSPECIFIED;
    }
    // Hundredths of a second.
    delay_ms = std::min<uint32_t>((uint32_t)std::max(0, gcb.DelayTime) * 10,
                                  MAX_FRAME_DURATION_MS);
  }

  if (dispose == DISPOSE_PREVIOUS) {
    m_saved_canvas = m_canvas;
    m_have_saved_canvas = true;
  }

  const ColorMapObject *map = desc.ColorMap ? desc.ColorMap : m_gif->SColorMap;

  if (!image.RasterBits) {
    throw std::runtime_error("GIF frame has no raster");
  }

  const int64_t left = desc.Left;
  const int64_t top = desc.Top;
  const int64_t fw = std::max(0, desc.Width);
  const int64_t fh = std::max(0, desc.Height);

  // The canvas covers m_region (the whole image outside a region decode).
  const int64_t rx = m_region.x;
  const int64_t ry = m_region.y;
  const int64_t x0 = std::max(left, rx);
  const int64_t y0 = std::max(top, ry);
  const int64_t x1 =
      std::max(x0, std::min(left + fw, rx + info.original_width));
  const int64_t y1 =
      std::max(y0, std::min(top + fh, ry + info.original_height));

  for (int64_t y = y0; y < y1; y++) {
    const GifByteType *src = image.RasterBits + (y - top) * fw;
    uint8_t *row = m_canvas.data() + m_canvas_stride * (size_t)(y - ry);
    for (int64_t x = x0; x < x1; x++) {
      int idx = src[x - left];
      if (idx == transparent) {
        continue;
      }
      if (!map || idx < 0 || idx >= map->ColorCount) {
        continue;
      }

      const GifColorType &c = map->Colors[idx];
      uint8_t *d = row + (size_t)(x - rx) * 4;
      d[0] = c.Red;
      d[1] = c.Green;
      d[2] = c.Blue;
      d[3] = 255;
    }
  }

  const uint32_t clip_x = (uint32_t)(x0 - rx);
  const uint32_t clip_y = (uint32_t)(y0 - ry);
  const uint32_t clip_w = (uint32_t)(x1 - x0);
  const uint32_t clip_h = (uint32_t)(y1 - y0);

  m_duration_ms = delay_ms;
  m_prev_dispose = dispose;
  m_prev_x = clip_x;
  m_prev_y = clip_y;
  m_prev_w = clip_w;
  m_prev_h = clip_h;
}

} // namespace imagedecoder
