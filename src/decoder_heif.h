#pragma once

#include "decoder_base.h"

#include <string.h>

#include <libheif/heif.h>
#include <libheif/heif_sequences.h>
#include <libheif/heif_tiling.h>

namespace imagedecoder {

class HeifDecoder : public BaseDecoder {
public:
  HeifDecoder() = default;
  ~HeifDecoder() override;

  bool read_header() override;
  bool supports_streaming() const override { return false; }
  StepResult decode_impl(const DecodeOptions &opts) override;

  uint32_t frame_count() const override { return m_frame_count; }
  uint32_t loop_count() const override { return m_loop_count; }
  cmsHPROFILE get_color_profile() override { return m_src_profile; }
  std::string get_name() override { return m_name; }
  std::vector<uint8_t> exif_data() override { return m_exif; }

  HdrKind hdr_kind() const override { return m_hdr_kind; }
  float hdr_headroom() const override { return m_hdr_headroom; }
  int hdr_primaries() const override { return m_primaries; }
  const GainmapData *gainmap() override;

  static bool is_heif(const uint8_t *data) {
    if (memcmp(data + 4, "ftyp", 4) != 0) {
      return false;
    }
    const char *brand = (const char *)data + 8;
    return memcmp(brand, "avif", 4) == 0 || memcmp(brand, "avis", 4) == 0 ||
           memcmp(brand, "heic", 4) == 0 || memcmp(brand, "heix", 4) == 0 ||
           memcmp(brand, "heim", 4) == 0 || memcmp(brand, "heis", 4) == 0 ||
           memcmp(brand, "hevc", 4) == 0 || memcmp(brand, "mif1", 4) == 0 ||
           memcmp(brand, "msf1", 4) == 0;
  }

protected:
  void restart() override;
  InvalidRect begin_region(const InvalidRect &want) override;

private:
  void close();
  void read_metadata(heif_image_handle *handle);
  void copy_planes(heif_image *image, const ImageInfo &dec);
  void find_gainmap();
  int copy_pixels(heif_image *image, const ImageInfo &dec, size_t work_stride,
                  uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy,
                  uint32_t w, uint32_t h);
  // A tile row at a time to [band] (y within the region, rows) when given.
  int decode_tiles(heif_colorspace space, heif_chroma chroma,
                   const ImageInfo &dec, size_t work_stride,
                   const std::function<void(uint32_t y, uint32_t rows,
                                            int bits)> &band = nullptr);
  void finish_rows(const ImageInfo &dec, const DecodeOptions &opts,
                   size_t work_stride, int decoded_bits, uint32_t y,
                   uint32_t rows);

  heif_context *m_ctx = nullptr;
  heif_image_handle *m_handle = nullptr;
  heif_track *m_track = nullptr;
  heif_image *m_lookahead = nullptr;
  uint32_t m_timescale = 0;
  heif_decoding_options *m_track_options = nullptr;
  uint32_t m_frame_count = 1;
  uint32_t m_loop_count = 0;
  bool m_advance_frame = false;
  bool m_complete = false;
  // A region decode assembling m_region from these tiles.
  bool m_tiles = false;
  heif_image_tiling m_tiling{};

  std::string m_name = "AVIF";
  HdrKind m_hdr_kind = HdrKind::None;
  float m_hdr_headroom = 0.0f;
  GainmapData m_gainmap;
  heif_item_id m_gainmap_item = 0;
  bool m_gainmap_read = false;
  int m_primaries = CICP_PRIMARIES_BT709;
  int m_luma_bits = 8;
  bool m_yuv = false;
  bool m_mono = false;
  heif_chroma m_chroma = heif_chroma_undefined;
  bool m_premultiplied = false;

  cmsHPROFILE m_src_profile = nullptr;
  // finish_rows()'s, made on first use in each decode_impl().
  std::unique_ptr<SrgbTransform> m_srgb_rows;
  std::vector<uint8_t> m_exif;
};

} // namespace imagedecoder
