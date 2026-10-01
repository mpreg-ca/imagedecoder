#pragma once

#include "decoder_base.h"

#include <string.h>

#include <webp/decode.h>
#include <webp/demux.h>

namespace imagedecoder {

class WebpDecoder : public BaseDecoder {
public:
  WebpDecoder();
  ~WebpDecoder() override;

  bool read_header() override;
  StepResult decode_impl(const DecodeOptions &opts) override;

  uint32_t frame_count() const override { return m_frame_count; }
  uint32_t loop_count() const override { return m_loop_count; }
  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "WEBP"; }
  std::vector<uint8_t> exif_data() override { return m_exif; }

  static bool is_webp(const uint8_t *data) {
    return memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
  }

protected:
  void restart() override;
  void rewind_codec() override;

private:
  void close();
  void read_metadata();
  StepResult decode_still(const DecodeOptions &opts);
  StepResult decode_animation(const DecodeOptions &opts);
  void prepare_srgb(const DecodeOptions &opts);

  WebPIDecoder *m_idec = nullptr;
  WebPDecoderConfig m_config = WebPDecoderConfig{};
  WebPAnimDecoder *m_anim = nullptr;

  bool m_animated = false;
  uint32_t m_frame_count = 1;
  uint32_t m_loop_count = 0;
  int m_prev_timestamp = 0;
  bool m_complete = false;

  size_t m_row_stride = 0;
  uint32_t m_next_frame = 0;
  uint32_t m_rows_done = 0;
  bool m_metadata_read = false;
  bool m_srgb_ready = false;
  SrgbTransform m_srgb;

  cmsHPROFILE m_src_profile = nullptr;
  std::vector<uint8_t> m_exif;
};

} // namespace imagedecoder
