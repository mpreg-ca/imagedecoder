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

private:
  void close();
  bool open();

  opj_image_t *m_image = nullptr;
  bool m_complete = false;

  size_t m_read_pos = 0;

  cmsHPROFILE m_src_profile = nullptr;
};

} // namespace imagedecoder
