#include "decoder_png.h"

#include <stdio.h>

#include "exif.h"

#ifndef PNG_READ_APNG_SUPPORTED
#error "libpng must be built with APNG support (PNG_READ_APNG_SUPPORTED)"
#endif
#ifndef PNG_eXIf_SUPPORTED
#error "libpng must be built with eXIf support (PNG_eXIf_SUPPORTED)"
#endif

#include "lcms2.h"
#include <algorithm>
#include <string.h>

namespace imagedecoder {

static const png_uint_32 MAX_DIMENSION = 32767;
static const png_alloc_size_t MAX_CHUNK_BYTES = 8 * 1024 * 1024;

static const uint32_t INVALIDATE_ROWS = 16;

static void PNGDoGammaCorrection(png_structp png, png_infop pinfo) {
  if (!png_get_valid(png, pinfo, PNG_INFO_gAMA)) {
    return;
  }

  double aGamma;

  if (png_get_gAMA(png, pinfo, &aGamma)) {
    if ((aGamma <= 0.0) || (aGamma > 21474.83)) {
      aGamma = 0.45455;
      png_set_gAMA(png, pinfo, aGamma);
    }
    png_set_gamma(png, 2.2, aGamma);
  } else {
    png_set_gamma(png, 2.2, 0.45455);
  }
}

PngDecoder::PngDecoder() { create(); }

PngDecoder::~PngDecoder() { destroy(); }

bool PngDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (setjmp(png_jmpbuf(m_png))) {
    throw std::runtime_error(m_message[0] ? m_message : "PNG decode failed");
  }

  while (!m_has_info && m_source.available() > 0) {
    size_t chunk = std::min<size_t>(m_source.available(), 64);
    const uint8_t *at = m_source.data() + m_source.consumed;
    m_source.consumed += chunk;
    png_process_data(m_png, m_pinfo, (png_bytep)at, chunk);
    // on_info pauses, so no row arrives before decode_impl lays out a buffer.
    m_source.consumed -= m_unconsumed;
    m_unconsumed = 0;
  }

  if (m_has_info) {
    return true;
  }
  if (m_source.complete()) {
    throw std::runtime_error("Truncated PNG: no header");
  }
  return false;
}

BaseDecoder::StepResult PngDecoder::decode_impl(const DecodeOptions &opts) {
  m_opts = opts;

  if (!read_header()) {
    return {};
  }

  if (setjmp(png_jmpbuf(m_png))) {
    throw std::runtime_error(m_message[0] ? m_message : "PNG decode failed");
  }

  if (m_row_stride == 0) {
    allocate();
  }

  if (m_done) {
    return {true, 0, {}};
  }

  m_frame_ready = false;

  while (!m_done && !m_frame_ready && m_source.available() > 0) {
    size_t chunk = m_source.available();
    const uint8_t *at = m_source.data() + m_source.consumed;
    m_source.consumed += chunk;
    png_process_data(m_png, m_pinfo, (png_bytep)at, chunk);

    m_source.consumed -= m_unconsumed;
    m_unconsumed = 0;
  }

  if (m_animated) {
    // Composited and converted by on_frame_end; decode() finishes it.
    if (m_frame_ready) {
      if (m_frames_shown >= m_frame_count) {
        m_done = true;
      }
      return {m_done, m_row_stride, {}};
    }
    if (m_done) {
      return {true, 0, {}};
    }
  } else if (m_done || (m_source.complete() && m_last_row)) {
    // Every row arrived; a missing IEND costs nothing already decoded.
    m_done = true;
    m_dirty.clear();
    return {true, m_row_stride, {}};
  }

  if (m_source.complete()) {
    throw std::runtime_error("Truncated PNG");
  }

  return {false, 0, take_dirty(), m_row_stride};
}

std::vector<uint8_t> PngDecoder::exif_data() {
  if (!m_has_info || !m_png) {
    return {};
  }

  png_uint_32 length = 0;
  png_bytep data = nullptr;
  if (png_get_eXIf_1(m_png, m_pinfo, &length, &data) && data && length > 0) {
    return std::vector<uint8_t>(data, data + length);
  }
  return {};
}

void PngDecoder::restart() {
  m_dirty.clear();
  destroy();
  m_message[0] = 0;
  m_has_info = false;
  m_done = false;
  m_in_band = false;
  m_current_pass = 0;
  m_frame_count = 1;
  m_animated = false;
  m_have_saved_canvas = false;
  m_saved_canvas.clear();
  m_dispose_op = 0;
  m_blend_op = 0;
  m_duration_ms = 0;
  m_frame = 0;
  m_last_row = false;
  m_frames_shown = 0;
  m_loop_count = 0;
  m_skip_frame = false;
  m_frame_x = m_frame_y = m_frame_w = m_frame_h = 0;
  m_frame_buf.clear();
  m_work.clear();
  m_frame_ready = false;
  m_unconsumed = 0;
  m_buffer.clear();
  m_canvas.clear();
  m_row_stride = 0;
  m_source.consumed = 0;
  create();
}

void PngDecoder::create() {
  auto errorFn = [](png_struct *p, png_const_charp msg) {
    auto *self = (PngDecoder *)png_get_error_ptr(p);
    if (self) {
      snprintf(self->m_message, sizeof self->m_message, "%s", msg ? msg : "");
    }
    png_longjmp(p, 1);
  };

  auto warnFn = [](png_struct *, png_const_charp) {};

  m_png = png_create_read_struct(PNG_LIBPNG_VER_STRING, (void *)this, errorFn,
                                 warnFn);
  if (!m_png) {
    throw std::runtime_error("Failed to create png read struct");
  }

  m_pinfo = png_create_info_struct(m_png);
  if (!m_pinfo) {
    png_destroy_read_struct(&m_png, NULL, NULL);
    m_png = nullptr;
    throw std::runtime_error("Failed to create png info struct");
  }

  png_set_user_limits(m_png, MAX_DIMENSION, MAX_DIMENSION);
  png_set_chunk_malloc_max(m_png, MAX_CHUNK_BYTES);

  png_set_option(m_png, PNG_MAXIMUM_INFLATE_WINDOW, PNG_OPTION_ON);
#ifdef PNG_SKIP_sRGB_CHECK_PROFILE
  png_set_option(m_png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);
#endif
  png_set_check_for_invalid_index(m_png, 0);

  png_set_progressive_read_fn(m_png, (void *)this, info_callback, row_callback,
                              end_callback);
  png_set_progressive_frame_fn(m_png, frame_info_callback, frame_end_callback);
}

void PngDecoder::destroy() {
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
  if (m_png) {
    png_destroy_read_struct(&m_png, m_pinfo ? &m_pinfo : nullptr, NULL);
  }
  m_png = nullptr;
  m_pinfo = nullptr;
}

bool PngDecoder::read_icc_profile() {
  if (png_get_valid(m_png, m_pinfo, PNG_INFO_iCCP)) {
    png_charp name;
    png_bytep icc_data;
    png_uint_32 icc_size;
    int comp_type;
    png_get_iCCP(m_png, m_pinfo, &name, &comp_type, &icc_data, &icc_size);

    m_src_profile = cmsOpenProfileFromMem(icc_data, icc_size);
    if (m_src_profile) {
      cmsColorSpaceSignature profileSpace = cmsGetColorSpace(m_src_profile);

      auto color_type = png_get_color_type(m_png, m_pinfo);

      bool rgb = color_type & PNG_COLOR_MASK_COLOR;

      if ((rgb && profileSpace != cmsSigRgbData) ||
          (!rgb && profileSpace != cmsSigGrayData)) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      } else {
        return true;
      }
    }
  }

  if (png_get_valid(m_png, m_pinfo, PNG_INFO_gAMA) &&
      png_get_valid(m_png, m_pinfo, PNG_INFO_cHRM)) {
    cmsCIExyYTRIPLE primaries = {
        {0.64, 0.33, 1}, {0.21, 0.71, 1}, {0.15, 0.06, 1}};
    cmsCIExyY whitepoint = {0.3127, 0.3290, 1.0};

    png_get_cHRM(m_png, m_pinfo, &whitepoint.x, &whitepoint.y, &primaries.Red.x,
                 &primaries.Red.y, &primaries.Green.x, &primaries.Green.y,
                 &primaries.Blue.x, &primaries.Blue.y);

    // on_info has libpng correct the samples to 2.2, so that is what they hold.
    cmsToneCurve *cmsgamma[3];
    cmsgamma[0] = cmsgamma[1] = cmsgamma[2] = cmsBuildGamma(NULL, 2.2);

    m_src_profile = cmsCreateRGBProfile(&whitepoint, &primaries, cmsgamma);

    cmsFreeToneCurve(cmsgamma[0]);
    return false;
  }

  if (png_get_valid(m_png, m_pinfo, PNG_INFO_sRGB)) {
    int intent;
    png_set_gray_to_rgb(m_png);
    png_get_sRGB(m_png, m_pinfo, &intent);
    m_src_profile = cmsCreate_sRGBProfile();
    cmsSetHeaderRenderingIntent(m_src_profile, intent);
    return true;
  }

  return false;
}

template <typename Body>
static void in_callback(png_structp png, const char *whose, Body body) {
  char msg[160];
  try {
    body();
    return;
  } catch (const std::exception &e) {
    snprintf(msg, sizeof msg, "%s", e.what());
  } catch (...) {
    snprintf(msg, sizeof msg, "%s", whose);
  }
  png_error(png, msg);
}

void PngDecoder::info_callback(png_structp png, png_infop) {
  auto *self = (PngDecoder *)png_get_progressive_ptr(png);
  in_callback(png, "PNG header rejected", [&] { self->on_info(); });
}

void PngDecoder::row_callback(png_structp png, png_bytep row, png_uint_32 y,
                              int pass) {
  auto *self = (PngDecoder *)png_get_progressive_ptr(png);
  in_callback(png, "PNG row rejected", [&] { self->on_row(row, y, pass); });
}

void PngDecoder::end_callback(png_structp png, png_infop) {
  ((PngDecoder *)png_get_progressive_ptr(png))->m_done = true;
}

void PngDecoder::frame_info_callback(png_structp png, png_uint_32 frame) {
  auto *self = (PngDecoder *)png_get_progressive_ptr(png);
  in_callback(png, "PNG frame rejected", [&] { self->on_frame_info(frame); });
}

void PngDecoder::frame_end_callback(png_structp png, png_uint_32) {
  auto *self = (PngDecoder *)png_get_progressive_ptr(png);
  in_callback(png, "PNG frame rejected", [&] { self->on_frame_end(); });
}

void PngDecoder::load_frame() {
  m_frame_x = png_get_next_frame_x_offset(m_png, m_pinfo);
  m_frame_y = png_get_next_frame_y_offset(m_png, m_pinfo);
  m_frame_w = png_get_next_frame_width(m_png, m_pinfo);
  m_frame_h = png_get_next_frame_height(m_png, m_pinfo);
  m_dispose_op = png_get_next_frame_dispose_op(m_png, m_pinfo);
  m_blend_op = png_get_next_frame_blend_op(m_png, m_pinfo);
  // libpng checks the rect against the canvas; this keeps the copies safe.
  if (m_frame_x > info.original_width || m_frame_y > info.original_height ||
      m_frame_w > info.original_width - m_frame_x ||
      m_frame_h > info.original_height - m_frame_y) {
    throw std::runtime_error("APNG frame outside the canvas");
  }

  png_uint_16 num = png_get_next_frame_delay_num(m_png, m_pinfo);
  png_uint_16 den = png_get_next_frame_delay_den(m_png, m_pinfo);
  if (den == 0) {
    den = 100;
  }
  m_duration_ms = (uint32_t)std::min<uint64_t>((uint64_t)num * 1000 / den,
                                               MAX_FRAME_DURATION_MS);

  const size_t pixel = (size_t)info.components * ((info.bits + 7) / 8);
  m_frame_buf.assign((size_t)m_frame_w * pixel * m_frame_h, 0);

  if (m_dispose_op == PNG_fcTL_DISPOSE_OP_PREVIOUS) {
    if (m_frames_shown == 0) {
      // There is no previous frame to return to; the spec says clear instead.
      m_dispose_op = PNG_fcTL_DISPOSE_OP_BACKGROUND;
    } else {
      m_saved_canvas = m_canvas;
      m_have_saved_canvas = true;
    }
  }
}

void PngDecoder::on_frame_info(uint32_t) {
  if (!m_animated) {
    return;
  }

  const size_t pixel = (size_t)info.components * ((info.bits + 7) / 8);
  if (m_have_saved_canvas) {
    m_canvas.swap(m_saved_canvas);
    m_have_saved_canvas = false;
  } else if (m_dispose_op == PNG_fcTL_DISPOSE_OP_BACKGROUND) {
    for (uint32_t r = 0; r < m_frame_h; r++) {
      memset(m_canvas.data() + m_row_stride * (m_frame_y + r) +
                 (size_t)m_frame_x * pixel,
             0, (size_t)m_frame_w * pixel);
    }
  }

  load_frame();
}

template <typename T>
static void blend_over(T *d, const T *s, uint32_t count, uint32_t comps) {
  const uint64_t max = (T) ~(T)0;
  const uint32_t a = comps - 1;
  for (uint32_t i = 0; i < count; i++, d += comps, s += comps) {
    const uint64_t sa = s[a];
    if (sa == max) {
      memcpy(d, s, comps * sizeof(T));
    } else if (sa != 0) {
      const uint64_t da = d[a];
      const uint64_t out_a = sa + da * (max - sa) / max;
      for (uint32_t c = 0; c < a; c++) {
        const uint64_t v = s[c] * sa + d[c] * da * (max - sa) / max;
        d[c] = (T)(out_a ? v / out_a : 0);
      }
      d[a] = (T)out_a;
    }
  }
}

void PngDecoder::on_frame_end() {
  if (!m_animated) {
    return;
  }
  if (m_skip_frame) {
    // The hidden default image: not part of the animation.
    m_skip_frame = false;
    return;
  }

  const size_t pixel = (size_t)info.components * ((info.bits + 7) / 8);
  const size_t span = (size_t)m_frame_w * pixel;
  const bool over = m_blend_op == PNG_fcTL_BLEND_OP_OVER && info.has_alpha;
  for (uint32_t r = 0; r < m_frame_h; r++) {
    uint8_t *d = m_canvas.data() + m_row_stride * (m_frame_y + r) +
                 (size_t)m_frame_x * pixel;
    const uint8_t *src = m_frame_buf.data() + span * r;
    if (!over) {
      memcpy(d, src, span);
    } else if (info.bits == 16) {
      blend_over((uint16_t *)d, (const uint16_t *)src, m_frame_w,
                 info.components);
    } else {
      blend_over(d, src, m_frame_w, info.components);
    }
  }

  // Converted here; decode() compacts and turns it.
  m_buffer = m_canvas;
  finish_rows(0, info.original_height);
  m_frame = m_frames_shown++;
  m_frame_ready = true;

  m_unconsumed = png_process_data_pause(m_png, 0);
}

void PngDecoder::on_info() {
  auto color_type = png_get_color_type(m_png, m_pinfo);
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(m_png);
  }

  auto depth = png_get_bit_depth(m_png, m_pinfo);
  if (color_type == PNG_COLOR_TYPE_GRAY && depth < 8) {
    png_set_expand_gray_1_2_4_to_8(m_png);
  }

  if (png_get_valid(m_png, m_pinfo, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(m_png);
  }

  png_set_swap(m_png);

  if (!read_icc_profile()) {
    if (png_get_valid(m_png, m_pinfo, PNG_INFO_gAMA) &&
        png_get_valid(m_png, m_pinfo, PNG_INFO_cHRM)) {
      png_set_gray_to_rgb(m_png);
      PNGDoGammaCorrection(m_png, m_pinfo);
    }
  }

  m_passes = png_set_interlace_handling(m_png);

  png_read_update_info(m_png, m_pinfo);

  check_dimensions(png_get_image_width(m_png, m_pinfo),
                   png_get_image_height(m_png, m_pinfo));

  m_channels = png_get_channels(m_png, m_pinfo);
  m_bit_depth = png_get_bit_depth(m_png, m_pinfo);
  auto final_type = png_get_color_type(m_png, m_pinfo);

  const uint32_t width = png_get_image_width(m_png, m_pinfo);
  const uint32_t height = png_get_image_height(m_png, m_pinfo);
  info = {
      .width = width,
      .height = height,
      .original_width = width,
      .original_height = height,
      .components = m_channels,
      .has_alpha = (final_type & PNG_COLOR_MASK_ALPHA) != 0,
      .color = (final_type & PNG_COLOR_MASK_COLOR) ? ColorFamily::RGB
                                                   : ColorFamily::Gray,
      .sample_type = SampleType::Integer,
      .bits = m_bit_depth,
  };
  if (png_get_valid(m_png, m_pinfo, PNG_INFO_acTL)) {
    png_uint_32 frames = 0;
    png_uint_32 plays = 0;
    if (png_get_acTL(m_png, m_pinfo, &frames, &plays) && frames > 0) {
      m_animated = true;
      // libpng counts a hidden default image among the frames; it is not one.
      const bool hidden = png_get_first_frame_is_hidden(m_png, m_pinfo) != 0;
      if (hidden && frames > 1) {
        frames--;
      }
      if ((uint64_t)frames * width * height > MAX_ANIMATION_PIXELS) {
        throw std::runtime_error("APNG has too many frames for its size");
      }
      m_frame_count = checked_frame_count(frames);
      m_loop_count = plays;
    }
  }

  m_has_info = true;

  {
    const std::vector<uint8_t> exif = exif_data();
    if (!exif.empty()) {
      int tag = exif_orientation(exif.data(), exif.size());
      if (tag >= 1 && tag <= 8) {
        info.orientation = (uint32_t)tag;
      }
    }
  }

  if (m_animated) {
    // Frame 0's fcTL precedes IDAT, where no frame callback reports it.
    m_skip_frame = png_get_first_frame_is_hidden(m_png, m_pinfo) != 0;
    if (!m_skip_frame) {
      load_frame();
    }
  }

  // Rows wait for decode_impl, which knows the options the buffer is for.
  m_unconsumed = png_process_data_pause(m_png, 0);
}

void PngDecoder::allocate() {
  const size_t stride = layout_stride(info, m_opts);
  const size_t row_bytes = png_get_rowbytes(m_png, m_pinfo);
  if (row_bytes > stride) {
    png_error(m_png, "PNG row is larger than the decode buffer");
  }

  const size_t bytes = checked_buffer_bytes(stride, info.original_height);
  if (m_animated) {
    m_canvas.assign(bytes, 0);
  } else if (m_passes > 1) {
    // Passes combine into the raw rows, so conversion needs its own copy.
    m_work.assign(checked_buffer_bytes(row_bytes, info.original_height), 0);
  }
  m_srgb = SrgbTransform(info, m_src_profile, m_opts.srgb_output);
  m_buffer.assign(bytes, 0);
  m_row_stride = stride;
}

void PngDecoder::finish_rows(uint32_t y, uint32_t height) {
  if (height == 0) {
    return;
  }
  m_srgb.apply(m_buffer.data(), m_row_stride, y, height);
  finish_layout(m_buffer.data(), m_row_stride, info, m_opts, y, height);
}

void PngDecoder::on_row(png_bytep row, uint32_t y, int pass) {
  if (!row || m_skip_frame || m_row_stride == 0) {
    return;
  }

  if (m_animated) {
    // Composited on frame end, when every pass has landed.
    if (y >= m_frame_h) {
      return;
    }
    const size_t span = m_frame_buf.size() / (m_frame_h ? m_frame_h : 1);
    uint8_t *dst = m_frame_buf.data() + span * y;
    if (m_passes > 1) {
      png_progressive_combine_row(m_png, dst, row);
    } else {
      memcpy(dst, row,
             std::min(span, (size_t)png_get_rowbytes(m_png, m_pinfo)));
    }
    return;
  }

  if (y >= info.original_height) {
    return;
  }
  const size_t bytes = png_get_rowbytes(m_png, m_pinfo);
  uint8_t *dst = m_buffer.data() + m_row_stride * y;

  if (m_passes > 1) {
    // Converted per row from the combined raw one, so every pass shows.
    uint8_t *raw = m_work.data() + bytes * y;
    png_progressive_combine_row(m_png, raw, row);
    memcpy(dst, raw, bytes);
    finish_rows(y, 1);
    m_last_row = pass == 6 && y + 1 == info.original_height;
    m_dirty.add(InvalidRect{0, 0, info.original_width, info.original_height});
    return;
  }

  memcpy(dst, row, bytes);
  m_last_row = y + 1 == info.original_height;

  if (pass != m_current_pass) {
    m_current_pass = pass;
    m_in_band = false;
  }
  if (!m_in_band) {
    m_band_first = y;
    m_in_band = true;
  }
  m_band_last = y;

  if (m_band_last - m_band_first + 1 < INVALIDATE_ROWS &&
      m_band_last + 1 < info.original_height) {
    return;
  }

  const uint32_t first = m_band_first;
  const uint32_t height = m_band_last - first + 1;
  m_in_band = false;
  finish_rows(first, height);
  m_dirty.add(InvalidRect{0, first, info.original_width, height});
}

} // namespace imagedecoder
