#pragma once

#include "decoder_base.h"

#include <tiffio.h>

namespace imagedecoder {

class TiffDecoder : public BaseDecoder {
public:
  TiffDecoder();
  ~TiffDecoder() override;

  bool read_header() override;
  bool supports_streaming() const override { return false; }
  StepResult decode_impl(const DecodeOptions &opts) override;

  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "TIFF"; }

  std::vector<uint8_t> exif_data() override {
    if (!m_source.complete()) {
      return {};
    }
    return std::vector<uint8_t>(m_source.data(),
                                m_source.data() + m_source.size());
  }

  static bool is_tiff(const uint8_t *data) {
    return (data[0] == 'I' && data[1] == 'I' && data[2] == 42 &&
            data[3] == 0) ||
           (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42);
  }

protected:
  void restart() override;
  InvalidRect begin_region(const InvalidRect &want) override;

private:
  void close();
  bool open();
  // A band at a time to `band` (rows y.. of the image, from m_buffer's top)
  // when given; else the whole frame into m_buffer.
  void read_native(
      const DecodeOptions &opts,
      const std::function<void(uint32_t y, uint32_t rows)> &band = nullptr);
  void read_rgba(const DecodeOptions &opts);

  TIFF *m_tiff = nullptr;
  size_t m_read_pos = 0;
  uint32_t m_width = 0; // full image; info shrinks during a region
  uint32_t m_height = 0;

  uint16_t m_bits_per_sample = 8;
  uint16_t m_samples_per_pixel = 1;
  bool m_associated_alpha = false;
  uint16_t m_photometric = 0;
  uint16_t m_planar_config = 1;
  bool m_native_layout = false;
  bool m_complete = false;

  cmsHPROFILE m_src_profile = nullptr;
};

} // namespace imagedecoder
