#pragma once

#include "decoder_base.h"

#include <string.h>

#include <openjpeg.h>

namespace imagedecoder {

class Jp2Decoder : public BaseDecoder {
public:
  Jp2Decoder() = default;
  ~Jp2Decoder() override;

  bool read_header() override;
  bool supports_streaming() const override { return false; }
  StepResult decode_impl(const DecodeOptions &opts) override;

  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "JP2"; }

  static bool is_jp2(const uint8_t *data) {
    static const uint8_t kJp2[12] = {0,   0,   0,    0x0c, 'j',  'P',
                                     ' ', ' ', 0x0d, 0x0a, 0x87, 0x0a};
    return memcmp(data, kJp2, 12) == 0 || (data[0] == 0xff && data[1] == 0x4f &&
                                           data[2] == 0xff && data[3] == 0x51);
  }

  size_t stream_read(void *buf, size_t size);
  int64_t stream_skip(int64_t bytes);
  bool stream_seek(int64_t pos);

protected:
  void restart() override;
  InvalidRect begin_region(const InvalidRect &want) override;
  uint32_t begin_scale(uint32_t sample, uint32_t aligned,
                       const InvalidRect &want) override;

private:
  void close();
  bool open();
  void decode_data();

  opj_image_t *m_image = nullptr;
  // Held from header to decode, so a region can be set between them.
  opj_codec_t *m_codec = nullptr;
  opj_stream_t *m_stream = nullptr;
  uint32_t m_comp_x0 = 0, m_comp_y0 = 0; // Full image's, in component pixels.
  bool m_complete = false;

  size_t m_read_pos = 0;

  cmsHPROFILE m_src_profile = nullptr;
};

} // namespace imagedecoder
