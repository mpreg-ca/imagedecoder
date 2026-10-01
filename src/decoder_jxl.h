#pragma once

#include "decoder_base.h"

#include <string.h>

#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>

namespace imagedecoder {

class JpegXlDecoder : public BaseDecoder {
public:
  JpegXlDecoder();
  ~JpegXlDecoder() override;

  bool read_header() override;
  StepResult decode_impl(const DecodeOptions &opts) override;

private:
  StepResult decode_threaded(const DecodeOptions &opts);

public:
  uint32_t frame_count() const override { return m_frame_count; }
  uint32_t loop_count() const override {
    return m_basic.have_animation ? m_basic.animation.num_loops : 0;
  }
  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "JXL"; }
  std::vector<uint8_t> exif_data() override { return m_exif; }

  HdrKind hdr_kind() const override { return m_hdr_kind; }
  float hdr_headroom() const override { return m_hdr_headroom; }
  int hdr_primaries() const override { return m_primaries; }

  static bool is_jxl(const uint8_t *data) {
    static const uint8_t kContainer[12] = {0,   0,   0,    0x0c, 'J',  'X',
                                           'L', ' ', 0x0d, 0x0a, 0x87, 0x0a};
    return (data[0] == 0xff && data[1] == 0x0a) ||
           memcmp(data, kContainer, 12) == 0;
  }

protected:
  void restart() override;
  void rewind_codec() override;

private:
  void create();
  void close();
  void refresh_input();
  void read_basic_info();
  void read_colour();
  void present(const DecodeOptions &opts);

  ::JxlDecoder *m_dec = nullptr;
  void *m_runner = nullptr;
  bool m_retried_unthreaded = false;
  JxlBasicInfo m_basic = {};
  bool m_complete = false;
  bool m_colour_read = false;
  bool m_last_frame = false;
  // Something was shown, so a threaded failure can no longer be retried.
  bool m_emitted = false;
  std::vector<uint8_t> m_raw;
  bool m_frame_ready = false;
  bool m_advance_frame = false;
  bool m_attached = false;
  bool m_closed = false;
  size_t m_attached_end = 0;

  uint32_t m_frame_count = 1;
  size_t m_row_stride = 0;
  SrgbTransform m_srgb;

  HdrKind m_hdr_kind = HdrKind::None;
  float m_hdr_headroom = 0.0f;
  int m_primaries = CICP_PRIMARIES_BT709;

  cmsHPROFILE m_src_profile = nullptr;
  std::vector<uint8_t> m_exif;
};

} // namespace imagedecoder
