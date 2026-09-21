#pragma once

#include "decoder_base.h"
#include "png.h"

#include <csetjmp>

namespace imagedecoder {

class PngDecoder : public BaseDecoder {
public:
  PngDecoder();
  ~PngDecoder() override;

  bool read_header() override;
  StepResult decode_impl(const DecodeOptions &opts) override;

  uint32_t frame_count() const override { return m_frame_count; }
  uint32_t loop_count() const override { return m_loop_count; }
  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "PNG"; }

  std::vector<uint8_t> exif_data() override;

  static bool is_png(const uint8_t *data) {
    return data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
           data[3] == 'G';
  }

protected:
  void restart() override;

private:
  void create();
  void destroy();
  bool read_icc_profile();

  static void info_callback(png_structp png, png_infop pinfo);
  static void row_callback(png_structp png, png_bytep row, png_uint_32 y,
                           int pass);
  static void end_callback(png_structp png, png_infop pinfo);
  static void frame_info_callback(png_structp png, png_uint_32 frame);
  static void frame_end_callback(png_structp png, png_uint_32 frame);
  void on_frame_info(uint32_t frame);
  void load_frame();
  void on_frame_end();

  void on_info();
  void allocate();
  void finish_rows(uint32_t y, uint32_t height);
  void on_row(png_bytep row, uint32_t y, int pass);

  png_struct *m_png = nullptr;
  png_info *m_pinfo = nullptr;
  char m_message[256] = {};

  uint32_t m_channels = 0;
  uint32_t m_bit_depth = 0;
  /* libpng's callbacks reach these but not decode_impl's locals. */
  DecodeOptions m_opts;

  DirtyRegion m_dirty;
  InvalidRect take_dirty() {
    InvalidRect r = m_dirty.rect();
    m_dirty.clear();
    return r;
  }

  int m_passes = 1;

  cmsHPROFILE m_src_profile = nullptr;

  size_t m_row_stride = 0;
  // Colour transform then widening, in that order.
  SrgbTransform m_srgb;

  uint32_t m_band_first = 0;
  uint32_t m_band_last = 0;
  bool m_in_band = false;
  int m_current_pass = 0;

  bool m_done = false;

  uint32_t m_frame_count = 1;
  // The hidden default image decodes but is not a frame.
  bool m_skip_frame = false;
  // The final row of a still arrived, so a missing IEND is not truncation.
  bool m_last_row = false;
  uint32_t m_frames_shown = 0;
  // One frame's rows, composited into the canvas when it ends.
  std::vector<uint8_t> m_frame_buf;
  // Interlaced passes combine here; m_buffer holds the converted rows.
  std::vector<uint8_t> m_work;
  uint32_t m_frame_x = 0;
  uint32_t m_frame_y = 0;
  uint32_t m_frame_w = 0;
  uint32_t m_frame_h = 0;
  uint8_t m_dispose_op = 0;
  uint8_t m_blend_op = 0;
  std::vector<uint8_t> m_canvas;
  std::vector<uint8_t> m_saved_canvas;

  bool m_have_saved_canvas = false;
  bool m_animated = false;
  uint32_t m_loop_count = 0;

  bool m_frame_ready = false;

  size_t m_unconsumed = 0;
};

} // namespace imagedecoder
