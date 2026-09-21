#pragma once

#include "decoder_base.h"

#include <gif_lib.h>

#include <string.h>

namespace imagedecoder {

// Output is always RGBA: a palette carries a transparent index.
class GifDecoder : public BaseDecoder {
public:
  GifDecoder() = default;
  ~GifDecoder() override;

  bool read_header() override;
  bool supports_streaming() const override { return false; }
  StepResult decode_impl(const DecodeOptions &opts) override;

  uint32_t frame_count() const override { return m_frame_count; }
  uint32_t loop_count() const override { return m_loop_count; }
  cmsHPROFILE get_color_profile() override { return nullptr; }
  std::string get_name() override { return "GIF"; }

  static bool is_gif(const uint8_t *data) {
    return memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0;
  }

protected:
  void restart() override;

private:
  void close();
  void compose_frame(uint32_t index);

  GifFileType *m_gif = nullptr;
  void *m_read_state = nullptr;
  uint32_t m_frame_count = 1;
  uint32_t m_loop_count = 0;
  uint32_t m_next_frame = 0;
  bool m_complete = false;

  int m_prev_dispose = DISPOSAL_UNSPECIFIED;
  uint32_t m_prev_x = 0;
  uint32_t m_prev_y = 0;
  uint32_t m_prev_w = 0;
  uint32_t m_prev_h = 0;
  std::vector<uint8_t> m_canvas;
  size_t m_canvas_stride = 0;
  std::vector<uint8_t> m_saved_canvas;
  bool m_have_saved_canvas = false;
};

} // namespace imagedecoder
