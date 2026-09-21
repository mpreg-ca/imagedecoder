#pragma once

#include "decoder_avif.h"
#include "decoder_base.h"
#include "decoder_gif.h"
#include "decoder_heif.h"
#include "decoder_jp2.h"
#include "decoder_jpeg.h"
#include "decoder_jxl.h"
#include "decoder_png.h"
#include "decoder_tiff.h"
#include "decoder_webp.h"

#include <memory>
#include <string>
#include <vector>

namespace imagedecoder {

inline constexpr size_t SNIFF_BYTES = 12;

inline std::vector<std::string> supported_formats() {
  return {"PNG", "JPEG", "TIFF", "WEBP", "GIF", "AVIF", "HEIF", "JXL", "JP2"};
}

inline std::unique_ptr<BaseDecoder> create_decoder(const uint8_t *data,
                                                   size_t size) {
  if (!data || size < SNIFF_BYTES) {
    return nullptr;
  }

  if (PngDecoder::is_png(data)) {
    return std::make_unique<PngDecoder>();
  }

  if (TiffDecoder::is_tiff(data)) {
    return std::make_unique<TiffDecoder>();
  }

  if (WebpDecoder::is_webp(data)) {
    return std::make_unique<WebpDecoder>();
  }

  if (GifDecoder::is_gif(data)) {
    return std::make_unique<GifDecoder>();
  }

  if (AvifDecoder::is_avif(data, size)) {
    return std::make_unique<AvifDecoder>();
  }

  if (HeifDecoder::is_heif(data)) {
    return std::make_unique<HeifDecoder>();
  }

  if (JpegXlDecoder::is_jxl(data)) {
    return std::make_unique<JpegXlDecoder>();
  }

  if (Jp2Decoder::is_jp2(data)) {
    return std::make_unique<Jp2Decoder>();
  }

  if (JpegDecoder::is_jpeg(data)) {
    return std::make_unique<JpegDecoder>();
  }

  return nullptr;
}

} // namespace imagedecoder
