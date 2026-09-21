#include "decoder_jpeg.h"
#include "cmyk.h"
#include "exif.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <string.h>
#include <string_view>

namespace imagedecoder {

// Ceiling on libjpeg's own allocations, so a header can't size the working set.
static const long MAX_LIBJPEG_MEMORY = 512L * 1024 * 1024;
static const int MAX_SCANS = 1000;

// The layout libjpeg writes: YCbCr stays planar only when asked for as-is.
static ImageInfo decoded_layout(const ImageInfo &info,
                                const DecodeOptions &opts) {
  ImageInfo dec = info;
  if (dec.color == ColorFamily::YUV &&
      opts.output_mode != OutputMode::Original) {
    dec.color = ColorFamily::RGB;
    dec.components = dec.has_alpha ? 4 : 3;
    dec.subsampling_w = 0;
    dec.subsampling_h = 0;
    dec.full_range = true;
  }
  return dec;
}

static bool all_components_seen(jpeg_decompress_struct *info) {
  if (!info->coef_bits) {
    return jpeg_input_complete(info);
  }

  for (int c = 0; c < info->num_components; c++) {
    if (info->coef_bits[c][0] == -1) {
      return false;
    }
  }
  return true;
}

static cmsHPROFILE open_builtin_cmyk_profile() {
  std::vector<uint8_t> icc(CMYK_USWebCoatedSWOP_icc_len);
  uLongf len = (uLongf)icc.size();
  if (uncompress(icc.data(), &len, CMYK_USWebCoatedSWOP_icc_z,
                 (uLong)CMYK_USWebCoatedSWOP_icc_z_len) != Z_OK ||
      len != icc.size()) {
    return nullptr;
  }
  return cmsOpenProfileFromMem(icc.data(), (cmsUInt32Number)icc.size());
}

JpegDecoder::JpegDecoder() { create(); }

JpegDecoder::~JpegDecoder() {
  destroy();
  if (m_cmyk_transform) {
    cmsDeleteTransform(m_cmyk_transform);
  }
  if (m_cmyk_profile) {
    cmsCloseProfile(m_cmyk_profile);
  }
  if (m_cmyk_target_profile) {
    cmsCloseProfile(m_cmyk_target_profile);
  }
}

bool JpegDecoder::read_header() {
  if (m_has_info) {
    return true;
  }

  if (setjmp(m_jerr.jmpbuf)) {
    detach_source();
    throw std::runtime_error(m_jerr.message[0] ? m_jerr.message
                                               : "Invalid JPEG");
  }

  attach_source();
  int rc = jpeg_read_header(&m_jinfo, TRUE);
  detach_source();

  if (rc == JPEG_SUSPENDED) {
    if (m_source.complete()) {
      throw std::runtime_error("Truncated JPEG: no header");
    }
    return false;
  }

  switch (m_jinfo.jpeg_color_space) {
  case JCS_GRAYSCALE:
  case JCS_RGB:
  case JCS_YCbCr:
  case JCS_CMYK:
  case JCS_YCCK:
    break;
  default:
    throw std::runtime_error("Unsupported JPEG color space");
  }

  auto jcs = m_jinfo.jpeg_color_space;
  m_progressive = jpeg_has_multiple_scans(&m_jinfo);

  if (jcs == JCS_CMYK || jcs == JCS_YCCK) {
    m_src_profile = cmsCreate_sRGBProfile();
  } else {
    m_src_profile = read_icc_profile();
  }

  auto color = jcs == JCS_RGB         ? ColorFamily::RGB
               : jcs == JCS_YCbCr     ? ColorFamily::YUV
               : jcs == JCS_GRAYSCALE ? ColorFamily::Gray
                                      : ColorFamily::RGB;

  // Zero (no planes) unless the ratio is a power of two: decoded as RGB then.
  uint32_t subsampling_w = 0;
  uint32_t subsampling_h = 0;
  if (color == ColorFamily::YUV) {
    if (m_jinfo.num_components < 3) {
      throw std::runtime_error("YCbCr JPEG without three components");
    }
    const jpeg_component_info *y = &m_jinfo.comp_info[0];
    const jpeg_component_info *cb = &m_jinfo.comp_info[1];
    const jpeg_component_info *cr = &m_jinfo.comp_info[2];
    // read_planar_rows needs full-factor luma and one chroma row per iMCU.
    if (m_jinfo.num_components == 3 &&
        y->h_samp_factor == m_jinfo.max_h_samp_factor &&
        y->v_samp_factor == m_jinfo.max_v_samp_factor &&
        y->v_samp_factor <= 2 && cb->v_samp_factor == 1 &&
        cb->h_samp_factor > 0 && cb->v_samp_factor > 0 &&
        cb->h_samp_factor == cr->h_samp_factor &&
        cb->v_samp_factor == cr->v_samp_factor &&
        m_jinfo.max_h_samp_factor % cb->h_samp_factor == 0 &&
        m_jinfo.max_v_samp_factor % cb->v_samp_factor == 0) {
      const int rw = m_jinfo.max_h_samp_factor / cb->h_samp_factor;
      const int rh = m_jinfo.max_v_samp_factor / cb->v_samp_factor;
      if ((rw & (rw - 1)) == 0 && (rh & (rh - 1)) == 0) {
        subsampling_w = (uint32_t)rw >> 1;
        subsampling_h = (uint32_t)rh >> 1;
      }
    }
  }

  uint32_t components;
  uint32_t bits;
  if (jcs == JCS_CMYK || jcs == JCS_YCCK) {
    components = 3;
    bits = 16;
  } else {
    components = (uint32_t)m_jinfo.num_components;
    bits = 8;
  }

  check_dimensions(m_jinfo.image_width, m_jinfo.image_height);

  info = {
      .width = (uint32_t)m_jinfo.image_width,
      .height = (uint32_t)m_jinfo.image_height,
      .original_width = (uint32_t)m_jinfo.image_width,
      .original_height = (uint32_t)m_jinfo.image_height,
      .components = components,
      .color = color,
      .sample_type = SampleType::Integer,
      .bits = bits,
      .subsampling_w = subsampling_w,
      .subsampling_h = subsampling_h,
      .yuv_matrix = 5,
  };
  {
    std::vector<uint8_t> exif = exif_data_locked();
    info.orientation = (uint32_t)exif_orientation(exif.data(), exif.size());
  }

  m_has_info = true;

  /* The gain map trails the image, but the header's markers announce it.
   * Decided once: a restart for new options must not change the verdict. */
  if (!m_hdr_decided) {
    m_hdr_decided = true;
    if (m_source.complete()) {
      probe_gainmap();
    } else if (announces_gainmap()) {
      m_hdr_kind = HdrKind::Gainmap;
      m_probe_pending = true;
    }
  }
  return true;
}

BaseDecoder::StepResult JpegDecoder::decode_impl(const DecodeOptions &opts) {
  DirtyRegion dirty;
  if (!read_header()) {
    return {};
  }

  if (m_probe_pending && m_source.complete()) {
    probe_gainmap();
  }

  if (m_stage == Stage::Done) {
    return {true, 0, {}};
  }

  auto *dinfo = &m_jinfo;
  const auto jcs = dinfo->jpeg_color_space;
  const bool cmyk = jcs == JCS_CMYK || jcs == JCS_YCCK;

  // Armed first: the setup below reads the ICC profile through libjpeg.
  if (setjmp(m_jerr.jmpbuf)) {
    detach_source();
    if (m_stage == Stage::Done) {
      // Trailing garbage after the last row costs nothing already decoded.
      return {true, m_row_stride, {}};
    }
    throw std::runtime_error(m_jerr.message[0] ? m_jerr.message
                                               : "JPEG decode failed");
  }

  if (m_row_stride == 0) {
    m_dec_layout = decoded_layout(info, opts);
    m_planar = m_dec_layout.color == ColorFamily::YUV &&
               (m_dec_layout.subsampling_w || m_dec_layout.subsampling_h);
    // Everything that can fail comes before the stride marks setup as done.
    size_t stride;
    size_t bytes;
    if (m_planar) {
      const uint32_t pw = padded_width(m_dec_layout);
      const uint32_t ph = padded_height(m_dec_layout);
      size_t planes = 0;
      size_t raw = 0;
      for (int i = 0; i < 3; i++) {
        const jpeg_component_info *c = &dinfo->comp_info[i];
        m_plane_w[i] = pw * c->h_samp_factor / dinfo->max_h_samp_factor;
        m_plane_h[i] = ph * c->v_samp_factor / dinfo->max_v_samp_factor;
        m_raw_stride[i] = c->width_in_blocks * DCTSIZE;
        planes += (size_t)m_plane_w[i] * m_plane_h[i];
        raw += (size_t)m_raw_stride[i] * DCTSIZE * c->v_samp_factor;
      }
      stride = output_row_bytes(m_dec_layout, opts);
      bytes = checked_buffer_bytes(planes, 1);
      m_raw.assign(raw, 0);
    } else {
      stride = layout_stride(m_dec_layout, opts);
      bytes = checked_buffer_bytes(stride, info.original_height);
    }
    if (cmyk) {
      open_cmyk_transform();
      m_cmyk_row.assign((size_t)info.original_width * 4, 0);
    }
    // CMYK comes out of its own transform already in sRGB.
    m_srgb =
        SrgbTransform(m_dec_layout, m_src_profile, opts.srgb_output && !cmyk);
    m_buffer.assign(bytes, 0);
    m_row_stride = stride;
  }

  attach_source();

  if (m_stage == Stage::Header) {
    dinfo->out_color_space =
        cmyk                                      ? JCS_CMYK
        : m_dec_layout.color == ColorFamily::Gray ? JCS_GRAYSCALE
        : m_dec_layout.color == ColorFamily::YUV  ? JCS_YCbCr
                                                  : JCS_RGB;
    dinfo->raw_data_out = m_planar;
    dinfo->do_fancy_upsampling = TRUE;
    dinfo->dct_method = JDCT_ISLOW;
    // Worth much more here than in a plain decode: shows the early scans.
    dinfo->do_block_smoothing = TRUE;

    dinfo->buffered_image = m_progressive;
    m_stage = Stage::Start;
  }

  if (m_stage == Stage::Start) {
    if (!jpeg_start_decompress(dinfo)) {
      detach_source();
      if (m_source.complete()) {
        throw std::runtime_error("Truncated JPEG");
      }
      return {};
    }
    const uint32_t components = cmyk ? 4 : m_dec_layout.components;
    if (dinfo->output_width != info.original_width ||
        dinfo->output_height != info.original_height ||
        (uint32_t)dinfo->output_components > components) {
      detach_source();
      throw std::runtime_error("JPEG output does not match its header");
    }
    m_stage = Stage::Scanlines;
  }

  bool finished = false;

  while (m_stage == Stage::Scanlines && !finished) {
    if (dinfo->buffered_image && !m_in_output) {
      int status;
      do {
        status = jpeg_consume_input(dinfo);
        if (status == JPEG_REACHED_SOS) {
          m_in_scan = true;
        } else if (status == JPEG_SCAN_COMPLETED) {
          m_in_scan = false;
        }
      } while (status != JPEG_SUSPENDED && status != JPEG_REACHED_EOI);

      // Each scan walks every block: thousands of them cost, not refine.
      if (dinfo->input_scan_number > MAX_SCANS) {
        detach_source();
        throw std::runtime_error("JPEG has too many scans");
      }

      if (jpeg_input_complete(dinfo) &&
          dinfo->input_scan_number == dinfo->output_scan_number) {
        finished = true;
        break;
      }

      // Show the last whole scan: output never waits on input, so each pass
      // covers the image, where the scan still arriving would sweep down it.
      // The coefficients hold that scan's rows so far either way.
      const int shown = jpeg_input_complete(dinfo) || !m_in_scan
                            ? dinfo->input_scan_number
                            : dinfo->input_scan_number - 1;
      if (shown <= dinfo->output_scan_number) {
        break;
      }

      if (!all_components_seen(dinfo) && !jpeg_input_complete(dinfo)) {
        break;
      }

      if (!jpeg_start_output(dinfo, shown)) {
        break;
      }
      m_in_output = true;
    }

    while (dinfo->output_scanline < dinfo->output_height) {
      const uint32_t y = dinfo->output_scanline;
      if (m_planar) {
        const uint32_t rows = read_planar_rows(y);
        if (rows == 0) {
          break;
        }
        dirty.add(InvalidRect{0, y, info.original_width,
                              std::min(rows, info.original_height - y)});
        continue;
      }

      uint8_t *row = m_buffer.data() + m_row_stride * y;
      uint8_t *dst = cmyk ? m_cmyk_row.data() : row;
      if (jpeg_read_scanlines(dinfo, &dst, 1) != 1) {
        break;
      }
      if (cmyk) {
        cmsDoTransform(m_cmyk_transform, dst, row, info.original_width);
      }

      m_srgb.apply(m_buffer.data(), m_row_stride, y, 1);
      finish_layout(m_buffer.data(), m_row_stride, m_dec_layout, opts, y, 1);
      dirty.add(InvalidRect{0, y, info.original_width, 1});
    }

    if (dinfo->output_scanline < dinfo->output_height) {
      break;
    }

    if (!dinfo->buffered_image) {
      finished = true;
      break;
    }

    if (!jpeg_finish_output(dinfo)) {
      break;
    }

    m_in_output = false;
    m_scan++;
  }

  if (finished) {
    m_stage = Stage::Done;
    jpeg_finish_decompress(dinfo);
  }

  detach_source();

  if (finished) {
    return {true, m_row_stride, {}};
  }
  if (m_source.complete()) {
    throw std::runtime_error("Truncated JPEG");
  }
  // Planes have no rows to pack, so a planar partial gives no stride.
  return {false, 0, dirty.rect(), m_planar ? 0 : m_row_stride};
}

/* One iMCU row at luma row y into the planes; returns its height, 0 on
 * suspension. Via block-wide scratch, since libjpeg writes whole blocks. */
uint32_t JpegDecoder::read_planar_rows(uint32_t y) {
  auto *dinfo = &m_jinfo;
  const uint32_t rows = DCTSIZE * dinfo->comp_info[0].v_samp_factor;
  const uint32_t luma_bytes = rows * m_raw_stride[0];
  const uint32_t chroma_bytes = DCTSIZE * m_raw_stride[1];

  JSAMPROW rowptrs[2 * DCTSIZE + DCTSIZE + DCTSIZE];
  for (uint32_t i = 0; i < rows; i++) {
    rowptrs[i] = m_raw.data() + (size_t)m_raw_stride[0] * i;
  }
  for (uint32_t i = 0; i < DCTSIZE; i++) {
    rowptrs[2 * DCTSIZE + i] =
        m_raw.data() + luma_bytes + (size_t)m_raw_stride[1] * i;
    rowptrs[3 * DCTSIZE + i] =
        m_raw.data() + luma_bytes + chroma_bytes + (size_t)m_raw_stride[2] * i;
  }

  JSAMPARRAY planes[3] = {&rowptrs[0], &rowptrs[2 * DCTSIZE],
                          &rowptrs[3 * DCTSIZE]};
  if (jpeg_read_raw_data(dinfo, planes, rows) == 0) {
    return 0;
  }

  uint8_t *plane = m_buffer.data();
  for (int c = 0; c < 3; c++) {
    const uint32_t n = c == 0 ? rows : DCTSIZE;
    const uint32_t first = y / rows * n;
    JSAMPARRAY src = planes[c];
    for (uint32_t i = 0; i < n && first + i < m_plane_h[c]; i++) {
      memcpy(plane + (size_t)m_plane_w[c] * (first + i), src[i], m_plane_w[c]);
    }
    plane += (size_t)m_plane_w[c] * m_plane_h[c];
  }
  return rows;
}

void JpegDecoder::open_cmyk_transform() {
  if (m_cmyk_transform) {
    return;
  }
  if (!m_cmyk_profile) {
    m_cmyk_profile = read_icc_profile();
  }
  if (!m_cmyk_profile) {
    m_cmyk_profile = open_builtin_cmyk_profile();
  }
  if (!m_cmyk_profile) {
    throw std::runtime_error("Failed to load CMYK profile");
  }
  if (!m_cmyk_target_profile) {
    m_cmyk_target_profile = cmsCreate_sRGBProfile();
  }
  const cmsUInt32Number in_type =
      m_jinfo.saw_Adobe_marker ? TYPE_CMYK_8_REV : TYPE_CMYK_8;
  m_cmyk_transform = cmsCreateTransform(
      m_cmyk_profile, in_type, m_cmyk_target_profile, TYPE_RGB_16,
      cmsGetHeaderRenderingIntent(m_cmyk_profile), 0);
  if (!m_cmyk_transform) {
    throw std::runtime_error("Failed to create CMYK <-> RGB transform");
  }
}

std::vector<uint8_t> JpegDecoder::exif_data() {
  if (!m_has_info) {
    return {};
  }
  return exif_data_locked();
}

const GainmapData *JpegDecoder::gainmap() {
  if (m_probe_pending && m_source.complete()) {
    probe_gainmap();
  }
  if (m_hdr_kind != HdrKind::Gainmap || !m_source.complete()) {
    return nullptr;
  }
  if (m_gainmap_read) {
    return m_gainmap.empty() ? nullptr : &m_gainmap;
  }

  m_gainmap_read = true;

  uhdr_codec_private_t *dec = uhdr_create_decoder();
  if (!dec) {
    return nullptr;
  }

  uhdr_compressed_image_t img = {};
  img.data = (void *)m_source.data();
  img.data_sz = m_source.size();
  img.capacity = m_source.size();
  img.cg = UHDR_CG_UNSPECIFIED;
  img.ct = UHDR_CT_UNSPECIFIED;
  img.range = UHDR_CR_UNSPECIFIED;

  // SDR output returns before applying the map, which is all that is wanted.
  if (uhdr_dec_set_image(dec, &img).error_code == UHDR_CODEC_OK &&
      uhdr_dec_set_out_color_transfer(dec, UHDR_CT_SRGB).error_code ==
          UHDR_CODEC_OK &&
      uhdr_dec_set_out_img_format(dec, UHDR_IMG_FMT_32bppRGBA8888).error_code ==
          UHDR_CODEC_OK &&
      uhdr_dec_probe(dec).error_code == UHDR_CODEC_OK &&
      uhdr_decode(dec).error_code == UHDR_CODEC_OK) {
    uhdr_raw_image_t *map = uhdr_get_decoded_gainmap_image(dec);
    if (map && map->w > 0 && map->h > 0) {
      // libultrahdr pads colour maps to RGBA; kept as RGB, like HEIF's.
      const uint32_t in = map->fmt == UHDR_IMG_FMT_8bppYCbCr400 ? 1 : 4;
      const uint32_t channels = in == 1 ? 1 : 3;
      const uint8_t *src = (const uint8_t *)map->planes[UHDR_PLANE_Y];
      size_t stride = (size_t)map->stride[UHDR_PLANE_Y] * in;

      if (src && stride >= (size_t)map->w * in) {
        m_gainmap.width = map->w;
        m_gainmap.height = map->h;
        m_gainmap.channels = channels;
        m_gainmap.pixels.resize((size_t)map->w * map->h * channels);
        for (uint32_t y = 0; y < map->h; y++) {
          const uint8_t *row = src + stride * y;
          uint8_t *dst =
              m_gainmap.pixels.data() + (size_t)y * map->w * channels;
          if (in == channels) {
            memcpy(dst, row, (size_t)map->w * channels);
            continue;
          }
          for (uint32_t x = 0; x < map->w; x++) {
            memcpy(dst + (size_t)x * 3, row + (size_t)x * 4, 3);
          }
        }
      }
    }
  }

  uhdr_release_decoder(dec);
  return m_gainmap.empty() ? nullptr : &m_gainmap;
}

void JpegDecoder::restart() {
  destroy();
  m_jerr.message[0] = 0;
  m_src = JpegSourceMgr{};
  m_jinfo = jpeg_decompress_struct{};
  m_has_info = false;
  m_stage = Stage::Header;
  m_scan = 0;
  m_in_output = false;
  m_in_scan = true;
  m_source.consumed = 0;
  m_buffer.clear();
  m_row_stride = 0;
  m_dec_layout = ImageInfo{};
  m_planar = false;
  create();
}

void JpegDecoder::create() {
  m_jinfo.err = jpeg_std_error(&m_jerr.pub);
  m_jerr.pub.error_exit = [](j_common_ptr info) {
    auto *err = (JpegErrorMgr *)info->err;
    (*info->err->format_message)(info, err->message);
    longjmp(err->jmpbuf, 1);
  };
  m_jerr.pub.output_message = [](j_common_ptr) {};

  if (setjmp(m_jerr.jmpbuf)) {
    throw std::runtime_error(
        m_jerr.message[0] ? m_jerr.message : "Failed to create JPEG decoder");
  }

  jpeg_create_decompress(&m_jinfo);
  m_created = true;

  m_jinfo.mem->max_memory_to_use = MAX_LIBJPEG_MEMORY;

  m_src.pub.init_source = [](j_decompress_ptr) {};
  m_src.pub.fill_input_buffer = [](j_decompress_ptr) -> boolean {
    return FALSE;
  };
  m_src.pub.skip_input_data = [](j_decompress_ptr cinfo, long num_bytes) {
    auto *src = (JpegSourceMgr *)cinfo->src;
    if (num_bytes <= 0) {
      return;
    }
    if ((size_t)num_bytes > src->pub.bytes_in_buffer) {
      src->skip = (size_t)num_bytes - src->pub.bytes_in_buffer;
      src->pub.next_input_byte += src->pub.bytes_in_buffer;
      src->pub.bytes_in_buffer = 0;
    } else {
      src->pub.next_input_byte += num_bytes;
      src->pub.bytes_in_buffer -= num_bytes;
    }
  };
  m_src.pub.resync_to_restart = jpeg_resync_to_restart;
  m_src.pub.term_source = [](j_decompress_ptr) {};
  m_src.pub.bytes_in_buffer = 0;
  m_src.pub.next_input_byte = nullptr;

  m_jinfo.src = (jpeg_source_mgr *)&m_src;

  jpeg_save_markers(&m_jinfo, JPEG_APP0 + 1, 0xFFFF);
  jpeg_save_markers(&m_jinfo, JPEG_APP0 + 2, 0xFFFF);
}

void JpegDecoder::destroy() {
  if (m_src_profile) {
    cmsCloseProfile(m_src_profile);
    m_src_profile = nullptr;
  }
  if (m_created) {
    jpeg_destroy_decompress(&m_jinfo);
  }
  m_created = false;
}

cmsHPROFILE JpegDecoder::read_icc_profile() {
  JOCTET *icc_data;
  unsigned int icc_size;
  if (!jpeg_read_icc_profile(&m_jinfo, &icc_data, &icc_size)) {
    return nullptr;
  }
  cmsHPROFILE src_profile = cmsOpenProfileFromMem(icc_data, icc_size);
  free(icc_data);
  if (!src_profile) {
    return nullptr;
  }

  cmsColorSpaceSignature profileSpace = cmsGetColorSpace(src_profile);

  auto colorspace = m_jinfo.jpeg_color_space;

  if ((colorspace == JCS_GRAYSCALE && profileSpace != cmsSigGrayData) ||
      (colorspace == JCS_CMYK && profileSpace != cmsSigCmykData) ||
      (colorspace == JCS_YCCK && profileSpace != cmsSigCmykData) ||
      (colorspace == JCS_RGB && profileSpace != cmsSigRgbData) ||
      (colorspace == JCS_YCbCr && profileSpace != cmsSigRgbData)) {
    cmsCloseProfile(src_profile);
    return nullptr;
  }

  return src_profile;
}

void JpegDecoder::attach_source() {
  size_t avail = m_source.available();
  if (m_src.skip > 0) {
    size_t take = std::min(m_src.skip, avail);
    m_source.consumed += take;
    m_src.skip -= take;
    avail -= take;
  }

  m_src.pub.next_input_byte = m_source.data() + m_source.consumed;
  m_src.pub.bytes_in_buffer = avail;
}

void JpegDecoder::detach_source() {
  m_source.consumed = m_source.size() - m_src.pub.bytes_in_buffer;
}

std::vector<uint8_t> JpegDecoder::exif_data_locked() {
  for (jpeg_saved_marker_ptr m = m_jinfo.marker_list; m; m = m->next) {
    if (m->marker != JPEG_APP0 + 1 || m->data_length < 6) {
      continue;
    }
    if (memcmp(m->data, "Exif\0\0", 6) != 0) {
      continue;
    }
    return std::vector<uint8_t>(m->data, m->data + m->data_length);
  }
  return {};
}

bool JpegDecoder::announces_gainmap() const {
  static const char XMP[] = "http://ns.adobe.com/xap/1.0/";
  static const char ISO[] = "urn:iso:std:iso:ts:21496:-1";
  for (jpeg_saved_marker_ptr m = m_jinfo.marker_list; m; m = m->next) {
    const char *data = (const char *)m->data;
    const size_t size = m->data_length;
    if (m->marker == JPEG_APP0 + 1 && size > sizeof XMP &&
        memcmp(data, XMP, sizeof XMP) == 0 &&
        std::string_view(data, size).find("hdrgm:") != std::string_view::npos) {
      return true;
    }
    if (m->marker == JPEG_APP0 + 2 && size >= sizeof ISO &&
        memcmp(data, ISO, sizeof ISO) == 0) {
      return true;
    }
  }
  return false;
}

void JpegDecoder::probe_gainmap() {
  if (!m_source.complete()) {
    return;
  }
  // A stream keeps the verdict its layout used; a whole file is settled here.
  const bool announced = m_probe_pending;
  m_probe_pending = false;
  if (!announced) {
    m_hdr_kind = HdrKind::None;
  }

  uhdr_codec_private_t *dec = uhdr_create_decoder();
  if (!dec) {
    return;
  }

  uhdr_compressed_image_t img = {};
  img.data = (void *)m_source.data();
  img.data_sz = m_source.size();
  img.capacity = m_source.size();
  img.cg = UHDR_CG_UNSPECIFIED;
  img.ct = UHDR_CT_UNSPECIFIED;
  img.range = UHDR_CR_UNSPECIFIED;

  if (uhdr_dec_set_image(dec, &img).error_code == UHDR_CODEC_OK &&
      uhdr_dec_probe(dec).error_code == UHDR_CODEC_OK) {
    uhdr_gainmap_metadata_t *meta = uhdr_dec_get_gainmap_metadata(dec);
    if (meta) {
      m_hdr_kind = HdrKind::Gainmap;

      for (int i = 0; i < 3; i++) {
        m_gainmap.gamma[i] = meta->gamma[i];
        m_gainmap.min_content_boost[i] = meta->min_content_boost[i];
        m_gainmap.max_content_boost[i] = meta->max_content_boost[i];
        m_gainmap.offset_sdr[i] = meta->offset_sdr[i];
        m_gainmap.offset_hdr[i] = meta->offset_hdr[i];
      }
      // libultrahdr gives the capacities as linear boosts.
      m_gainmap.base_headroom = meta->hdr_capacity_min > 1.0f
                                    ? std::log2(meta->hdr_capacity_min)
                                    : 0.0f;
      m_gainmap.alternate_headroom = meta->hdr_capacity_max > 1.0f
                                         ? std::log2(meta->hdr_capacity_max)
                                         : 0.0f;

      float boost =
          std::max({meta->max_content_boost[0], meta->max_content_boost[1],
                    meta->max_content_boost[2]});
      m_hdr_headroom = boost > 1.0f ? std::log2(boost) : 0.0f;
      if (m_hdr_headroom <= 0.0f && !announced) {
        m_hdr_kind = HdrKind::None;
      }
    }
  }

  uhdr_release_decoder(dec);
}

} // namespace imagedecoder
