#include "decoder_heif.h"

#include <libheif/heif_sequences.h>

#include <libheif/heif_items.h>

#include <algorithm>
#include <cmath>

namespace imagedecoder {

namespace {

// Big-endian reads over the ISO 21496-1 payload; false once it runs short.
struct IsoReader {
  const uint8_t *p;
  size_t size;
  size_t pos = 0;

  bool u8(uint32_t &v) {
    if (pos + 1 > size) {
      return false;
    }
    v = p[pos++];
    return true;
  }
  bool u16(uint32_t &v) {
    if (pos + 2 > size) {
      return false;
    }
    v = (uint32_t)p[pos] << 8 | p[pos + 1];
    pos += 2;
    return true;
  }
  bool u32(uint32_t &v) {
    if (pos + 4 > size) {
      return false;
    }
    v = (uint32_t)p[pos] << 24 | (uint32_t)p[pos + 1] << 16 |
        (uint32_t)p[pos + 2] << 8 | p[pos + 3];
    pos += 4;
    return true;
  }
  bool s32(int32_t &v) {
    uint32_t u;
    if (!u32(u)) {
      return false;
    }
    v = (int32_t)u;
    return true;
  }
};

/* A tmap item's data: a version byte, then ISO 21496-1 as libultrahdr reads
 * it for JPEG. Fills everything but the pixels; false if unusable. */
bool parse_iso_gainmap(const uint8_t *data, size_t size, GainmapData &out) {
  IsoReader r{data, size};
  uint32_t version, min_version, writer_version, flags;
  if (!r.u8(version) || version != 0 || !r.u16(min_version) ||
      min_version != 0 || !r.u16(writer_version) || !r.u8(flags)) {
    return false;
  }
  // Backward direction means an HDR base, which this path does not handle.
  if (flags & 4) {
    return false;
  }
  const int channels = (flags & 0x80) ? 3 : 1;
  const bool common = (flags & 8) != 0;

  uint32_t common_d = 1;
  if (common && !r.u32(common_d)) {
    return false;
  }
  auto frac = [&](bool is_signed, double &v) {
    int32_t n = 0;
    uint32_t un = 0;
    uint32_t d = common_d;
    if (is_signed ? !r.s32(n) : !r.u32(un)) {
      return false;
    }
    if (!common && !r.u32(d)) {
      return false;
    }
    if (d == 0) {
      return false;
    }
    v = (is_signed ? (double)n : (double)un) / d;
    return true;
  };

  double base_headroom, alt_headroom;
  if (!frac(false, base_headroom) || !frac(false, alt_headroom) ||
      base_headroom > 64 || alt_headroom > 64) {
    return false;
  }
  out.base_headroom = (float)base_headroom;
  out.alternate_headroom = (float)alt_headroom;
  for (int c = 0; c < channels; c++) {
    double min, max, gamma, base_offset, alt_offset;
    if (!frac(true, min) || !frac(true, max) || !frac(false, gamma) ||
        !frac(true, base_offset) || !frac(true, alt_offset)) {
      return false;
    }
    // Stops of gain, bounded so exp2 stays a finite float.
    if (min > max || min < -64 || max > 64 || !(gamma > 0) || gamma > 1e6 ||
        std::fabs(base_offset) > 1e6 || std::fabs(alt_offset) > 1e6) {
      return false;
    }
    out.min_content_boost[c] = (float)std::exp2(min);
    out.max_content_boost[c] = (float)std::exp2(max);
    out.gamma[c] = (float)gamma;
    out.offset_sdr[c] = (float)base_offset;
    out.offset_hdr[c] = (float)alt_offset;
  }
  for (int c = channels; c < 3; c++) {
    out.min_content_boost[c] = out.min_content_boost[0];
    out.max_content_boost[c] = out.max_content_boost[0];
    out.gamma[c] = out.gamma[0];
    out.offset_sdr[c] = out.offset_sdr[0];
    out.offset_hdr[c] = out.offset_hdr[0];
  }
  return true;
}

} // namespace

HeifDecoder::~HeifDecoder() { close(); }

bool HeifDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (!m_source.complete()) {
    return false;
  }

  close();

  m_ctx = heif_context_alloc();
  if (!m_ctx) {
    throw std::runtime_error("Failed to create HEIF context");
  }

  // A box header can claim an enormous image before a pixel is decoded.
  heif_context_set_maximum_image_size_limit(m_ctx, 32767);

  heif_error err = heif_context_read_from_memory_without_copy(
      m_ctx, m_source.data(), m_source.size(), nullptr);
  if (err.code != heif_error_Ok) {
    throw std::runtime_error(err.message ? err.message : "Invalid HEIF");
  }

  err = heif_context_get_primary_image_handle(m_ctx, &m_handle);
  if (err.code != heif_error_Ok || !m_handle) {
    throw std::runtime_error(err.message ? err.message : "HEIF has no image");
  }

  if (heif_context_has_sequence(m_ctx)) {
    m_track = heif_context_get_track(m_ctx, 0);
    if (m_track) {
      m_timescale = heif_track_get_timescale(m_track);
      uint32_t reps = heif_track_get_number_of_repetitions(m_track);
      m_loop_count = reps == heif_sequence_track_number_of_repetitions_infinite
                         ? 0
                     : reps == 0 ? 1
                                 : reps;

      m_track_options = heif_decoding_options_alloc();
      if (m_track_options) {
        m_track_options->ignore_sequence_editlist = 1;
      }
      if (m_timescale == 0) {
        m_timescale = heif_context_get_sequence_timescale(m_ctx);
      }
      // A sequence is animated; how many frames only decoding tells.
      m_frame_count = 2;
    }
  }

  int width = heif_image_handle_get_width(m_handle);
  int height = heif_image_handle_get_height(m_handle);

  if (m_track) {
    uint16_t tw = 0, th = 0;
    if (heif_track_get_image_resolution(m_track, &tw, &th).code ==
            heif_error_Ok &&
        tw > 0 && th > 0) {
      width = tw;
      height = th;
    }
  }
  check_dimensions(width < 0 ? 0 : (uint64_t)width,
                   height < 0 ? 0 : (uint64_t)height);

  bool alpha = heif_image_handle_has_alpha_channel(m_handle) != 0;
  m_premultiplied =
      alpha && heif_image_handle_is_premultiplied_alpha(m_handle) != 0;
  int luma_bits = heif_image_handle_get_luma_bits_per_pixel(m_handle);
  if (luma_bits <= 0) {
    luma_bits = 8;
  }
  m_luma_bits = luma_bits;
  uint32_t bits = luma_bits > 8 ? 16 : 8;

  m_name =
      is_heif(m_source.data()) && memcmp(m_source.data() + 8, "avi", 3) == 0
          ? "AVIF"
          : "HEIF";

  int matrix = -1;
  bool full_range = true;
  heif_color_profile_nclx *nclx = nullptr;
  if (heif_image_handle_get_nclx_color_profile(m_handle, &nclx).code ==
          heif_error_Ok &&
      nclx) {
    m_hdr_kind = hdr_kind_for_transfer((int)nclx->transfer_characteristics);
    m_primaries = (int)nclx->color_primaries; // For the F16 gamut step.
    matrix = (int)nclx->matrix_coefficients;
    full_range = nclx->full_range_flag != 0;

    if (m_hdr_kind == HdrKind::None &&
        (int)nclx->transfer_characteristics == CICP_TRANSFER_LINEAR &&
        bits > 8) {
      m_hdr_kind = HdrKind::Linear;
    }

    m_hdr_headroom = hdr_headroom_for(m_hdr_kind, 0.0f);
    heif_nclx_color_profile_free(nclx);
  }

  read_metadata(m_handle);
  if (!m_track && m_hdr_kind == HdrKind::None) {
    find_gainmap();
  }

  uint32_t sub_w = 0;
  uint32_t sub_h = 0;
  m_mono = false;
  m_yuv = false;
  m_chroma = heif_chroma_undefined;
  heif_colorspace native = heif_colorspace_undefined;
  heif_chroma chroma = heif_chroma_undefined;
  if (heif_image_handle_get_preferred_decoding_colorspace(m_handle, &native,
                                                          &chroma)
          .code == heif_error_Ok) {
    if (native == heif_colorspace_monochrome) {
      m_mono = true;
    } else if (native == heif_colorspace_YCbCr && matrix > 0 &&
               !m_premultiplied &&
               heif_image_handle_get_chroma_bits_per_pixel(m_handle) ==
                   luma_bits) {
      switch (chroma) {
      case heif_chroma_420:
        m_yuv = true;
        sub_w = 1;
        sub_h = 1;
        break;
      case heif_chroma_422:
        m_yuv = true;
        sub_w = 1;
        break;
      default:
        break;
      }
      if (m_yuv) {
        m_chroma = chroma;
      }
    }
  }

  // The data decides the output; a profile of the wrong family is dropped.
  if (m_src_profile &&
      (cmsGetColorSpace(m_src_profile) == cmsSigGrayData) != m_mono) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }

  info = {
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .original_width = (uint32_t)width,
      .original_height = (uint32_t)height,
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
      .full_range = full_range,
  };
  info.orientation = 1;

  m_has_info = true;
  return true;
}

BaseDecoder::StepResult HeifDecoder::decode_impl(const DecodeOptions &opts) {
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
  const size_t work_stride = layout_stride(dec, opts);

  const bool planar = dec.color == ColorFamily::YUV;
  const heif_channel single =
      m_mono ? heif_channel_Y : heif_channel_interleaved;
  heif_colorspace space = heif_colorspace_RGB;
  heif_chroma chroma;
  if (planar) {
    space = heif_colorspace_YCbCr;
    chroma = m_chroma;
  } else if (m_mono) {
    space = heif_colorspace_monochrome;
    chroma = heif_chroma_monochrome;
  } else if (info.bits == 16) {
    chroma = info.has_alpha ? heif_chroma_interleaved_RRGGBBAA_LE
                            : heif_chroma_interleaved_RRGGBB_LE;
  } else {
    chroma = info.has_alpha ? heif_chroma_interleaved_RGBA
                            : heif_chroma_interleaved_RGB;
  }

  heif_image *image = nullptr;
  heif_error err;
  if (m_track) {
    // Decoded one ahead (below), so the frame before the end knows it is last.
    if (m_lookahead) {
      image = m_lookahead;
      m_lookahead = nullptr;
      err = heif_error{heif_error_Ok, heif_suberror_Unspecified, nullptr};
    } else {
      err = heif_track_decode_next_image(m_track, &image, space, chroma,
                                         m_track_options);
    }
    if (err.code == heif_error_End_of_sequence) {
      m_complete = true;
      return {true, 0, {}};
    }
    if (err.code != heif_error_Ok || !image) {
      throw std::runtime_error(err.message ? err.message
                                           : "Failed to decode HEIF frame");
    }

    if (m_advance_frame) {
      m_advance_frame = false;
      m_frame++;
    }
  } else {
    err = heif_decode_image(m_handle, &image, space, chroma, nullptr);
    if (err.code != heif_error_Ok || !image) {
      throw std::runtime_error(err.message ? err.message
                                           : "Failed to decode HEIF");
    }
  }

  struct ImageGuard {
    heif_image *img;
    ~ImageGuard() {
      if (img) {
        heif_image_release(img);
      }
    }
  } image_guard{image};

  int decoded_bits = m_luma_bits;
  if (planar) {
    copy_planes(image, dec);
  } else {
    decoded_bits = heif_image_get_bits_per_pixel_range(image, single);
    int src_stride = 0;
    const uint8_t *src =
        heif_image_get_plane_readonly(image, single, &src_stride);
    if (!src || src_stride <= 0) {
      throw std::runtime_error("HEIF image has no pixels");
    }

    // Stale bytes only in row padding, which decode() compacts away.
    m_buffer.resize(checked_buffer_bytes(work_stride, info.original_height));

    if (heif_image_get_height(image, single) < (int)info.original_height ||
        heif_image_get_width(image, single) < (int)info.original_width) {
      throw std::runtime_error("HEIF frame is smaller than its declared size");
    }

    if (m_mono && dec.has_alpha) {
      /* libheif keeps alpha in a plane of its own, which may have its own
       * depth; it is brought to the luma's range as gray+alpha interleaves. */
      int alpha_stride = 0;
      const uint8_t *alpha = heif_image_get_plane_readonly(
          image, heif_channel_Alpha, &alpha_stride);
      const int alpha_bits =
          heif_image_get_bits_per_pixel_range(image, heif_channel_Alpha);
      const size_t sample = info.bits == 16 ? 2 : 1;
      const size_t alpha_sample = alpha_bits > 8 ? 2 : 1;
      if (!alpha || alpha_stride <= 0 || alpha_bits < 1 || alpha_bits > 16 ||
          decoded_bits < 1 || decoded_bits > 16 ||
          (size_t)alpha_stride < info.original_width * alpha_sample ||
          (size_t)src_stride < info.original_width * sample ||
          heif_image_get_width(image, heif_channel_Alpha) <
              (int)info.original_width ||
          heif_image_get_height(image, heif_channel_Alpha) <
              (int)info.original_height) {
        throw std::runtime_error("HEIF alpha plane is missing or too small");
      }
      const uint32_t alpha_max = (1u << alpha_bits) - 1;
      const uint32_t luma_max = (1u << decoded_bits) - 1;
      for (uint32_t y = 0; y < info.original_height; y++) {
        uint8_t *dst = m_buffer.data() + work_stride * y;
        const uint8_t *ys = src + (size_t)src_stride * y;
        const uint8_t *as = alpha + (size_t)alpha_stride * y;
        for (uint32_t x = 0; x < info.original_width; x++) {
          uint32_t a = alpha_sample == 2 ? ((const uint16_t *)as)[x] : as[x];
          a = std::min(a, alpha_max);
          if (alpha_max != luma_max) {
            a = (a * luma_max + alpha_max / 2) / alpha_max;
          }
          if (sample == 2) {
            ((uint16_t *)dst)[x * 2] = ((const uint16_t *)ys)[x];
            ((uint16_t *)dst)[x * 2 + 1] = (uint16_t)a;
          } else {
            dst[x * 2] = ys[x];
            dst[x * 2 + 1] = (uint8_t)a;
          }
        }
      }
    } else {
      if ((size_t)src_stride < row_bytes(dec)) {
        throw std::runtime_error("HEIF plane is narrower than its image");
      }
      const size_t copy = std::min((size_t)src_stride, row_bytes(dec));
      for (uint32_t y = 0; y < info.original_height; y++) {
        memcpy(m_buffer.data() + work_stride * y, src + (size_t)src_stride * y,
               copy);
      }
    }
  }

  if (m_track) {
    uint32_t ticks = heif_image_get_duration(image);
    const uint64_t ms =
        m_timescale ? (uint64_t)ticks * 1000 / m_timescale : ticks;
    m_duration_ms = (uint32_t)std::min<uint64_t>(ms, MAX_FRAME_DURATION_MS);
  }

  heif_image_release(image);
  image_guard.img = nullptr;

  if (!planar) {
    if (info.bits == 16 && decoded_bits > 0 && decoded_bits < 16) {
      const uint32_t in_max = (1u << decoded_bits) - 1;
      const size_t per_row = (size_t)info.original_width * dec.components;

      for (uint32_t y = 0; y < info.original_height; y++) {
        uint16_t *row = (uint16_t *)(m_buffer.data() + work_stride * y);
        for (size_t i = 0; i < per_row; i++) {
          uint32_t v = row[i];
          if (v > in_max) {
            v = in_max;
          }
          row[i] = (uint16_t)((v * 65535u + in_max / 2) / in_max);
        }
      }
    }

    const uint32_t n = dec.components;
    if (m_premultiplied && dec.has_alpha && n >= 2) {
      const uint32_t max = info.bits == 16 ? 65535u : 255u;
      for (uint32_t y = 0; y < dec.original_height; y++) {
        uint8_t *row = m_buffer.data() + work_stride * y;
        for (uint32_t x = 0; x < dec.original_width; x++) {
          if (info.bits == 16) {
            uint16_t *p = (uint16_t *)row + (size_t)x * n;
            const uint32_t a = p[n - 1];
            if (a == 0 || a >= max) {
              continue;
            }
            for (uint32_t c = 0; c + 1 < n; c++) {
              uint32_t v = ((uint32_t)p[c] * max + a / 2) / a;
              p[c] = (uint16_t)(v > max ? max : v);
            }
          } else {
            uint8_t *p = row + (size_t)x * n;
            const uint32_t a = p[n - 1];
            if (a == 0 || a >= max) {
              continue;
            }
            for (uint32_t c = 0; c + 1 < n; c++) {
              uint32_t v = ((uint32_t)p[c] * max + a / 2) / a;
              p[c] = (uint8_t)(v > max ? max : v);
            }
          }
        }
      }
    }

    if (opts.srgb_output) {
      SrgbTransform(dec, m_src_profile, true)
          .apply(m_buffer.data(), work_stride, 0, dec.original_height);
    }
    finish_layout(m_buffer.data(), work_stride, dec, opts, 0,
                  dec.original_height);
  }

  if (!m_track) {
    m_complete = true;
    return {true, work_stride, {}};
  }

  m_advance_frame = true;

  heif_image *next = nullptr;
  err = heif_track_decode_next_image(m_track, &next, space, chroma,
                                     m_track_options);
  // The lookahead settles the count: exact at the end, else one more.
  m_frame_count = checked_frame_count(
      err.code == heif_error_End_of_sequence
          ? (uint64_t)m_frame + 1
          : std::max<uint64_t>(m_frame_count, (uint64_t)m_frame + 2));
  if (err.code == heif_error_End_of_sequence) {
    m_complete = true;
    return {true, work_stride, {}};
  }
  if (err.code == heif_error_Ok && next) {
    m_lookahead = next;
  } else if (next) {
    heif_image_release(next);
  }
  // A bad next frame fails on the call that reaches it, not this one.
  return {false, work_stride, {}};
}

/* ISO 21496-1: a tmap item derives from (dimg) the primary image and the gain
 * map image, and carries the metadata as its data. */
void HeifDecoder::find_gainmap() {
  const heif_item_id primary = heif_image_handle_get_item_id(m_handle);
  const int count = heif_context_get_number_of_items(m_ctx);
  if (count <= 0) {
    return;
  }
  std::vector<heif_item_id> ids((size_t)count);
  const int n = heif_context_get_list_of_item_IDs(m_ctx, ids.data(), count);
  for (int i = 0; i < n; i++) {
    if (heif_item_get_item_type(m_ctx, ids[i]) !=
        heif_fourcc('t', 'm', 'a', 'p')) {
      continue;
    }
    heif_item_id map_id = 0;
    for (int ref = 0;; ref++) {
      uint32_t type = 0;
      heif_item_id *to = nullptr;
      const size_t refs =
          heif_context_get_item_references(m_ctx, ids[i], ref, &type, &to);
      if (refs >= 2 && type == heif_fourcc('d', 'i', 'm', 'g') &&
          to[0] == primary) {
        map_id = to[1];
      }
      // Allocated even for an empty entry, so released before stopping.
      heif_release_item_references(m_ctx, &to);
      if (refs == 0) {
        break;
      }
    }
    if (!map_id) {
      continue;
    }

    uint8_t *data = nullptr;
    size_t size = 0;
    if (heif_item_get_item_data(m_ctx, ids[i], nullptr, &data, &size).code !=
        heif_error_Ok) {
      continue;
    }
    GainmapData meta;
    const bool ok = data && parse_iso_gainmap(data, size, meta);
    heif_release_item_data(m_ctx, &data);
    if (!ok) {
      continue;
    }
    const float boost =
        std::max({meta.max_content_boost[0], meta.max_content_boost[1],
                  meta.max_content_boost[2]});
    if (!(boost > 1.0f) || !std::isfinite(boost)) {
      continue;
    }
    // The map must open, and no larger than the image it scales.
    heif_image_handle *map = nullptr;
    if (heif_context_get_image_handle(m_ctx, map_id, &map).code !=
            heif_error_Ok ||
        !map) {
      continue;
    }
    const int mw = heif_image_handle_get_width(map);
    const int mh = heif_image_handle_get_height(map);
    heif_image_handle_release(map);
    if (mw <= 0 || mh <= 0 || mw > heif_image_handle_get_width(m_handle) ||
        mh > heif_image_handle_get_height(m_handle)) {
      continue;
    }
    m_gainmap = std::move(meta);
    m_gainmap_item = map_id;
    m_hdr_kind = HdrKind::Gainmap;
    m_hdr_headroom = std::log2(boost);
    return;
  }
}

const GainmapData *HeifDecoder::gainmap() {
  if (m_hdr_kind != HdrKind::Gainmap || !m_ctx) {
    return nullptr;
  }
  if (m_gainmap_read) {
    return m_gainmap.empty() ? nullptr : &m_gainmap;
  }
  m_gainmap_read = true;

  heif_image_handle *handle = nullptr;
  if (heif_context_get_image_handle(m_ctx, m_gainmap_item, &handle).code !=
          heif_error_Ok ||
      !handle) {
    return nullptr;
  }
  heif_colorspace native = heif_colorspace_undefined;
  heif_chroma native_chroma = heif_chroma_undefined;
  heif_image_handle_get_preferred_decoding_colorspace(handle, &native,
                                                      &native_chroma);
  const bool mono = native == heif_colorspace_monochrome;
  // Deeper maps come back 16-bit, so narrowing to 8 rounds once, here.
  const bool deep = heif_image_handle_get_luma_bits_per_pixel(handle) > 8;
  heif_image *image = nullptr;
  heif_error err = heif_decode_image(
      handle, &image, mono ? heif_colorspace_monochrome : heif_colorspace_RGB,
      mono   ? heif_chroma_monochrome
      : deep ? heif_chroma_interleaved_RRGGBB_LE
             : heif_chroma_interleaved_RGB,
      nullptr);
  heif_image_handle_release(handle);
  if (err.code != heif_error_Ok || !image) {
    return nullptr;
  }

  const heif_channel channel = mono ? heif_channel_Y : heif_channel_interleaved;
  const uint32_t channels = mono ? 1 : 3;
  const int w = heif_image_get_width(image, channel);
  const int h = heif_image_get_height(image, channel);
  const int bits = heif_image_get_bits_per_pixel_range(image, channel);
  size_t stride = 0;
  const uint8_t *src = heif_image_get_plane_readonly2(image, channel, &stride);
  if (src && w > 0 && h > 0 && bits >= 8 && bits <= 16 &&
      (uint64_t)w * h <= MAX_IMAGE_PIXELS) {
    const size_t sample = bits > 8 ? 2 : 1;
    if (stride >= (size_t)w * channels * sample) {
      m_gainmap.width = (uint32_t)w;
      m_gainmap.height = (uint32_t)h;
      m_gainmap.channels = channels;
      m_gainmap.pixels.resize((size_t)w * h * channels);
      for (int y = 0; y < h; y++) {
        const uint8_t *row = src + stride * y;
        uint8_t *dst = m_gainmap.pixels.data() + (size_t)w * channels * y;
        if (sample == 1) {
          memcpy(dst, row, (size_t)w * channels);
          continue;
        }
        // Deeper maps are narrowed: the Kotlin side reads 8-bit gains.
        const uint32_t max = (1u << bits) - 1;
        for (size_t x = 0; x < (size_t)w * channels; x++) {
          const uint32_t v =
              std::min<uint32_t>(((const uint16_t *)row)[x], max);
          dst[x] = (uint8_t)((v * 255 + max / 2) / max);
        }
      }
    }
  }
  heif_image_release(image);
  return m_gainmap.empty() ? nullptr : &m_gainmap;
}

void HeifDecoder::restart() {
  close();
  m_has_info = false;
  m_complete = false;
  m_hdr_kind = HdrKind::None;
  m_hdr_headroom = 0.0f;
  m_gainmap = GainmapData{};
  m_gainmap_item = 0;
  m_gainmap_read = false;
  m_primaries = CICP_PRIMARIES_BT709;
  m_luma_bits = 8;
  m_frame = 0;
  m_frame_count = 1;
  m_loop_count = 0;
  m_advance_frame = false;
  m_premultiplied = false;
  m_duration_ms = 0;
  m_buffer.clear();
  m_exif.clear();
}

void HeifDecoder::close() {
  if (m_lookahead) {
    heif_image_release(m_lookahead);
    m_lookahead = nullptr;
  }
  if (m_track_options) {
    heif_decoding_options_free(m_track_options);
    m_track_options = nullptr;
  }
  if (m_track) {
    heif_track_release(m_track);
    m_track = nullptr;
  }
  m_timescale = 0;
  if (m_handle) {
    heif_image_handle_release(m_handle);
    m_handle = nullptr;
  }
  if (m_ctx) {
    heif_context_free(m_ctx);
    m_ctx = nullptr;
  }
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
}

void HeifDecoder::copy_planes(heif_image *image, const ImageInfo &dec) {
  static const heif_channel kChannels[4] = {
      heif_channel_Y, heif_channel_Cb, heif_channel_Cr, heif_channel_Alpha};
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

    if (p == 3 && !heif_image_has_channel(image, heif_channel_Alpha)) {
      memset(dst, 0xff, plane);
      dst += plane;
      continue;
    }

    size_t stride = 0;
    const uint8_t *src =
        heif_image_get_plane_readonly2(image, kChannels[p], &stride);
    if (!src) {
      throw std::runtime_error("HEIF image is missing a plane");
    }

    const int have_w = heif_image_get_width(image, kChannels[p]);
    const int have_h = heif_image_get_height(image, kChannels[p]);
    if (have_w <= 0 || have_h <= 0 || stride < (size_t)have_w * bytes) {
      throw std::runtime_error("HEIF plane is smaller than its image");
    }

    const uint32_t take_w = std::min((uint32_t)have_w, w);
    const uint32_t take_h = std::min((uint32_t)have_h, h);

    const int range = heif_image_get_bits_per_pixel_range(image, kChannels[p]);
    const uint32_t in_max =
        (bytes == 2 && range > 0 && range < 16) ? (1u << range) - 1u : 0u;

    for (uint32_t y = 0; y < h; y++) {
      uint8_t *row = dst + (size_t)w * bytes * y;
      memcpy(row, src + stride * std::min(y, take_h - 1),
             (size_t)take_w * bytes);

      for (uint32_t x = take_w; x < w; x++) {
        memcpy(row + (size_t)x * bytes, row + (size_t)(take_w - 1) * bytes,
               bytes);
      }

      if (in_max) {
        uint16_t *wide = (uint16_t *)row;
        for (uint32_t x = 0; x < w; x++) {
          uint32_t v = wide[x];
          if (v > in_max) {
            v = in_max;
          }
          wide[x] = (uint16_t)((v * 65535u + in_max / 2) / in_max);
        }
      }
    }
    dst += plane;
  }
}

void HeifDecoder::read_metadata(heif_image_handle *handle) {
  size_t icc_size = heif_image_handle_get_raw_color_profile_size(handle);
  if (icc_size > 0 && icc_size < (size_t)64 * 1024 * 1024) {
    std::vector<uint8_t> icc(icc_size);
    if (heif_image_handle_get_raw_color_profile(handle, icc.data()).code ==
        heif_error_Ok) {
      m_src_profile =
          cmsOpenProfileFromMem(icc.data(), (cmsUInt32Number)icc.size());
      const cmsColorSpaceSignature space =
          m_src_profile ? cmsGetColorSpace(m_src_profile) : cmsSigRgbData;
      if (m_src_profile && space != cmsSigRgbData && space != cmsSigGrayData) {
        cmsCloseProfile(m_src_profile);
        m_src_profile = nullptr;
      }
    }
  }

  heif_item_id exif_id = 0;
  if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &exif_id,
                                                       1) == 1) {
    size_t size = heif_image_handle_get_metadata_size(handle, exif_id);
    if (size > 4 && size < (size_t)64 * 1024 * 1024) {
      std::vector<uint8_t> block(size);
      if (heif_image_handle_get_metadata(handle, exif_id, block.data()).code ==
          heif_error_Ok) {
        uint32_t offset = ((uint32_t)block[0] << 24) |
                          ((uint32_t)block[1] << 16) |
                          ((uint32_t)block[2] << 8) | block[3];
        size_t start = (size_t)4 + offset;
        if (start < block.size()) {
          m_exif.assign(block.begin() + start, block.end());
        }
      }
    }
  }
}

} // namespace imagedecoder
