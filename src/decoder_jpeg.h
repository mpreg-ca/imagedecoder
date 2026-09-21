#pragma once

#include "decoder_base.h"
#include "jpeglib.h"

#include <csetjmp>

#include <ultrahdr_api.h>

namespace imagedecoder {

struct JpegErrorMgr final {
  jpeg_error_mgr pub;
  jmp_buf jmpbuf;
  char message[JMSG_LENGTH_MAX];
};

struct JpegSourceMgr final {
  jpeg_source_mgr pub;
  size_t skip = 0;
};

class JpegDecoder : public BaseDecoder {
public:
  JpegDecoder();
  ~JpegDecoder() override;

  bool read_header() override;
  StepResult decode_impl(const DecodeOptions &opts) override;

  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "JPEG"; }

  std::vector<uint8_t> exif_data() override;

  HdrKind hdr_kind() const override { return m_hdr_kind; }
  float hdr_headroom() const override { return m_hdr_headroom; }
  const GainmapData *gainmap() override;

  static bool is_jpeg(const uint8_t *data) {
    return data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
  }

protected:
  void restart() override;

private:
  void create();
  void destroy();
  cmsHPROFILE read_icc_profile();

  void attach_source();
  void detach_source();

  uint32_t read_planar_rows(uint32_t y);
  void open_cmyk_transform();

  ImageInfo m_dec_layout;

  std::vector<uint8_t> exif_data_locked();
  void probe_gainmap();
  bool announces_gainmap() const;

  JpegErrorMgr m_jerr = JpegErrorMgr{};
  JpegSourceMgr m_src = JpegSourceMgr{};
  jpeg_decompress_struct m_jinfo = jpeg_decompress_struct{};
  bool m_created = false;

  bool m_progressive = false;

  HdrKind m_hdr_kind = HdrKind::None;
  float m_hdr_headroom = 0.0f;
  GainmapData m_gainmap;
  bool m_gainmap_read = false;
  // Announced by a stream's header, confirmed once the whole file is here.
  bool m_probe_pending = false;
  bool m_hdr_decided = false;
  cmsHPROFILE m_src_profile = nullptr;
  cmsHPROFILE m_cmyk_profile = nullptr;
  cmsHPROFILE m_cmyk_target_profile = nullptr;
  cmsHTRANSFORM m_cmyk_transform = nullptr;
  std::vector<uint8_t> m_cmyk_row;

  // Subsampled YCbCr handed over as planes, read an iMCU row at a time.
  bool m_planar = false;
  uint32_t m_plane_w[3] = {};
  uint32_t m_plane_h[3] = {};
  uint32_t m_raw_stride[3] = {};
  std::vector<uint8_t> m_raw;

  enum class Stage { Header, Start, Scanlines, Done };
  Stage m_stage = Stage::Header;

  size_t m_row_stride = 0;
  SrgbTransform m_srgb;
  uint32_t m_scan = 0;

  bool m_in_output = false;
  // Input is inside a scan; the header stops at the first SOS.
  bool m_in_scan = true;
};

} // namespace imagedecoder
