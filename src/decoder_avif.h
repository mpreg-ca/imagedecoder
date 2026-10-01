#pragma once

#include "decoder_base.h"

#include <string.h>

#include <avif/avif.h>

namespace imagedecoder {

// AVIF through libavif: streams, and shows progressive layers and grid cells
// as they arrive.
class AvifDecoder : public BaseDecoder {
public:
  AvifDecoder() = default;
  ~AvifDecoder() override;

  bool read_header() override;
  StepResult decode_impl(const DecodeOptions &opts) override;

  uint32_t frame_count() const override { return m_frame_count; }
  uint32_t loop_count() const override { return m_loop_count; }
  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return "AVIF"; }
  std::vector<uint8_t> exif_data() override { return m_exif; }

  HdrKind hdr_kind() const override { return m_hdr_kind; }
  float hdr_headroom() const override { return m_hdr_headroom; }
  int hdr_primaries() const override { return m_primaries; }
  const GainmapData *gainmap() override;

  // An AVIF brand as the major one, or among the compatible ones of a
  // generic (mif1, msf1) ftyp - as far as [size] holds it.
  static bool is_avif(const uint8_t *data, size_t size) {
    if (size < 12 || memcmp(data + 4, "ftyp", 4) != 0) {
      return false;
    }
    const auto avif = [](const uint8_t *b) {
      return memcmp(b, "avif", 4) == 0 || memcmp(b, "avis", 4) == 0;
    };
    if (avif(data + 8)) {
      return true;
    }
    if (memcmp(data + 8, "mif1", 4) != 0 && memcmp(data + 8, "msf1", 4) != 0) {
      return false;
    }
    const size_t box = ((size_t)data[0] << 24) | ((size_t)data[1] << 16) |
                       ((size_t)data[2] << 8) | data[3];
    const size_t end = std::min(box, size);
    for (size_t at = 16; at + 4 <= end; at += 4) {
      if (avif(data + at)) {
        return true;
      }
    }
    return false;
  }

protected:
  void restart() override;
  void rewind_codec() override;

private:
  static avifResult read(avifIO *io, uint32_t flags, uint64_t offset,
                         size_t size, avifROData *out);
  void close();
  void read_metadata();
  void find_gainmap();
  bool matches_header(const avifImage *img) const;
  void convert(const ImageInfo &dec, size_t work_stride, uint32_t rows);
  void copy_planes(const ImageInfo &dec);

  avifDecoder *m_dec = nullptr;
  avifIO m_io = {};
  uint32_t m_frame_count = 1;
  uint32_t m_loop_count = 0;
  // Layers of one still, shown as progressive steps rather than frames.
  bool m_progressive = false;
  bool m_complete = false;
  bool m_advance_frame = false;
  // Rows of a grid already shown while its cells arrive.
  uint32_t m_rows = 0;
  // Dropped to one, once, when a threaded decode fails before showing
  // anything: its threads may simply not have started (a full wasm pool).
  int m_threads = (int)MAX_DECODE_THREADS;
  bool m_emitted = false;

  // clap, irot and imir, which libavif leaves to the caller: a crop, then a
  // turn as an Exif orientation.
  avifCropRect m_crop = {};
  bool m_cropped = false;
  uint32_t m_turn = 1;
  std::vector<uint8_t> m_native;

  HdrKind m_hdr_kind = HdrKind::None;
  float m_hdr_headroom = 0.0f;
  GainmapData m_gainmap;
  bool m_gainmap_read = false;
  int m_primaries = CICP_PRIMARIES_BT709;
  bool m_yuv = false;
  bool m_mono = false;
  // The coded image as parsed, which every decoded one must match: libavif
  // passes on an AV1 frame that disagrees with its container.
  uint32_t m_coded_width = 0;
  uint32_t m_coded_height = 0;
  uint32_t m_depth = 8;
  avifPixelFormat m_format = AVIF_PIXEL_FORMAT_NONE;

  cmsHPROFILE m_src_profile = nullptr;
  std::vector<uint8_t> m_exif;
};

} // namespace imagedecoder
