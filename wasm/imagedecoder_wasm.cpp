#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <malloc.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoder_factory.h"
#include "exif.h"

using emscripten::val;
using imagedecoder::BaseDecoder;
using imagedecoder::DecodeOptions;
using imagedecoder::DecodeProgress;
using imagedecoder::ImageInfo;

namespace {

// As the JNI layer: past these a caller's memory is the likelier problem.
constexpr uint64_t MAX_INPUT_BYTES = (uint64_t)1 << 30;
constexpr uint64_t MAX_OUTPUT_BYTES = (uint64_t)INT32_MAX;

[[noreturn]] void js_throw(const std::string &message) {
  val::global("Error").new_(message).throw_();
  __builtin_unreachable();
}

// The bytes of a Uint8Array, any other typed array or DataView, an
// ArrayBuffer, or a plain array of numbers, as a Uint8Array.
val as_bytes(const val &data) {
  if (data.isNull() || data.isUndefined()) {
    js_throw("No data given");
  }
  const val u8 = val::global("Uint8Array");
  if (data.instanceof(u8)) {
    return data;
  }
  if (data.instanceof(val::global("ArrayBuffer")) ||
      (val::global("SharedArrayBuffer").typeOf().as<std::string>() ==
           "function" &&
       data.instanceof(val::global("SharedArrayBuffer")))) {
    return u8.new_(data);
  }
  if (val::global("ArrayBuffer").call<bool>("isView", data)) {
    return u8.new_(data["buffer"], data["byteOffset"], data["byteLength"]);
  }
  if (val::global("Array").call<bool>("isArray", data)) {
    return u8.call<val>("from", data);
  }
  js_throw("Expected a Uint8Array, ArrayBuffer or array of bytes");
}

val js_bytes(const uint8_t *data, size_t size) {
  // One copy out: a view would dangle once wasm memory grows or is reused.
  return val(emscripten::typed_memory_view(size, data)).call<val>("slice");
}

val js_floats(const float *data) {
  val out = val::array();
  for (int i = 0; i < 3; i++) {
    out.call<void>("push", data[i]);
  }
  return out;
}

// sRGB for display, except HDR, where tone mapping is the caller's.
DecodeOptions js_options(const BaseDecoder &decoder) {
  DecodeOptions options;
  options.srgb_output = true;
  options.apply_orientation = true;
  const imagedecoder::HdrKind hdr = decoder.hdr_kind();
  options.output_mode = (hdr == imagedecoder::HdrKind::None ||
                         hdr == imagedecoder::HdrKind::Gainmap)
                            ? imagedecoder::OutputMode::Rgba8
                            : imagedecoder::OutputMode::RgbaF16;
  return options;
}

} // namespace

class ImageDecoder {
public:
  // All of the file (complete), or however much has arrived, the rest by
  // pushData() and markComplete(). Streamed, the format and header are read
  // as bytes come; until then width, height and format are empty and
  // decodeNext() asks for more. A format that can't stream waits for the end.
  static std::unique_ptr<ImageDecoder> open(const val &data, bool complete) {
    auto self = std::unique_ptr<ImageDecoder>(new ImageDecoder());
    self->guard([&] {
      self->push_js(data);
      if (complete) {
        self->mark_complete();
        if (!self->m_decoder) {
          js_throw("Not a supported image: too short");
        }
        if (!self->header()) {
          js_throw("Truncated image");
        }
      } else {
        self->header();
      }
    });
    return self;
  }

  void pushData(const val &data) {
    guard([&] {
      check_open();
      if (m_complete) {
        js_throw("markComplete() has already been called");
      }
      push_js(data);
    });
  }

  void markComplete() {
    guard([&] {
      check_open();
      mark_complete();
    });
  }

  // To the next whole frame, or the latest progressive step if the bytes run
  // out first. Throws "need more data" when there's nothing to show yet.
  val decodeNext() {
    val out = val::null();
    guard([&] {
      check_open();
      if (!m_more) {
        js_throw("No frames remain");
      }
      out = decode_step();
    });
    return out;
  }

  // Null if none; turned with the frames.
  val getGainmap() {
    val out = val::null();
    guard([&] {
      check_open();
      if (!m_complete || !header()) {
        js_throw("The image is not complete yet");
      }
      const imagedecoder::GainmapData *map = m_decoder->gainmap();
      if (!map || map->empty()) {
        return;
      }
      const uint64_t need = (uint64_t)map->width * map->height * map->channels;
      if (need == 0 || need > MAX_OUTPUT_BYTES || map->pixels.size() < need) {
        return;
      }
      const uint32_t orientation = m_decoder->info.orientation;
      ImageInfo layout;
      layout.original_width = map->width;
      layout.original_height = map->height;
      layout.components = map->channels;
      layout.bits = 8;
      uint32_t w = map->width;
      uint32_t h = map->height;
      if (imagedecoder::orientation_swaps_axes(orientation)) {
        std::swap(w, h);
      }
      const std::vector<uint8_t> turned = imagedecoder::orient_copy(
          map->pixels.data(), (size_t)map->width * map->channels, layout,
          orientation);
      out = val::object();
      out.set("pixels", js_bytes(turned.data(), turned.size()));
      out.set("width", w);
      out.set("height", h);
      out.set("channels", map->channels);
      out.set("gamma", js_floats(map->gamma));
      out.set("minContentBoost", js_floats(map->min_content_boost));
      out.set("maxContentBoost", js_floats(map->max_content_boost));
      out.set("offsetSdr", js_floats(map->offset_sdr));
      out.set("offsetHdr", js_floats(map->offset_hdr));
      out.set("baseHeadroom", map->base_headroom);
      out.set("alternateHeadroom", map->alternate_headroom);
    });
    return out;
  }

  val listTags() {
    val out = val::array();
    guard([&] {
      check_open();
      if (!header()) {
        return;
      }
      const std::vector<uint8_t> exif = m_decoder->exif_data();
      for (const std::string &name :
           imagedecoder::exif_list_tags(exif.data(), exif.size())) {
        out.call<void>("push", name);
      }
    });
    return out;
  }

  val getTag(const std::string &name) {
    val out = val::null();
    guard([&] {
      check_open();
      if (!header()) {
        return;
      }
      const std::vector<uint8_t> exif = m_decoder->exif_data();
      std::string value;
      if (imagedecoder::exif_get_tag(exif.data(), exif.size(), name.c_str(),
                                     &value)) {
        out = val(value);
      }
    });
    return out;
  }

  // Frees the decoder's memory now; delete() afterwards frees the rest.
  void close() {
    m_decoder.reset();
    std::vector<uint8_t>().swap(m_sniff);
    m_has_header = false;
    m_width = 0;
    m_height = 0;
    m_more = false;
    m_closed = true;
  }

  // Empty until the header is in.
  std::string format() const {
    return m_has_header ? m_decoder->get_name() : std::string();
  }
  uint32_t width() const { return m_width; }
  uint32_t height() const { return m_height; }
  bool hasNext() const { return m_more && !m_closed; }
  uint32_t page() const { return m_page; }
  uint32_t pages() const {
    return m_has_header ? std::max(1u, m_decoder->frame_count()) : 1;
  }
  uint32_t loopCount() const {
    return m_has_header ? m_decoder->loop_count() : 0;
  }
  bool isHdr() const {
    return m_has_header && js_options(*m_decoder).output_mode ==
                               imagedecoder::OutputMode::RgbaF16;
  }
  // As the Kotlin HdrKind: NONE, GAINMAP, PQ, HLG, LINEAR.
  uint32_t hdrKind() const {
    return m_has_header ? (uint32_t)m_decoder->hdr_kind() : 0;
  }
  float hdrHeadroom() const {
    return isHdr() ? m_decoder->hdr_headroom() : 0.0f;
  }

private:
  ImageDecoder() = default;

  // C++ exceptions become JS Errors with their message.
  template <typename F> void guard(F &&f) {
    try {
      f();
    } catch (const std::bad_alloc &) {
      js_throw("Out of memory");
    } catch (const std::exception &e) {
      js_throw(e.what());
    }
  }

  void check_open() const {
    if (m_closed) {
      js_throw("ImageDecoder has been closed");
    }
  }

  // Checked against the limit before any copy, then copied in slices so the
  // decoder's copy isn't briefly doubled.
  void push_js(const val &data) {
    const val bytes = as_bytes(data);
    const double length = bytes["length"].as<double>();
    if (!(length >= 0) || (double)m_total + length > (double)MAX_INPUT_BYTES) {
      js_throw("Image is too large to buffer");
    }
    constexpr size_t SLICE = (size_t)16 << 20;
    std::vector<uint8_t> chunk;
    for (size_t at = 0; at < (size_t)length; at += SLICE) {
      const size_t n = std::min(SLICE, (size_t)length - at);
      chunk.resize(n);
      val(emscripten::typed_memory_view(n, chunk.data()))
          .call<void>("set", bytes.call<val>("subarray", at, at + n));
      push(chunk.data(), n);
    }
  }

  void push(const uint8_t *data, size_t size) {
    m_total += size;
    if (m_decoder) {
      m_decoder->push_data(data, size);
      return;
    }
    m_sniff.insert(m_sniff.end(), data, data + size);
    if (m_sniff.size() < imagedecoder::SNIFF_BYTES) {
      return;
    }
    m_decoder = imagedecoder::create_decoder(m_sniff.data(), m_sniff.size());
    if (!m_decoder) {
      js_throw("Not a supported image format");
    }
    m_decoder->push_data(m_sniff.data(), m_sniff.size());
    std::vector<uint8_t>().swap(m_sniff);
  }

  void mark_complete() {
    m_complete = true;
    if (m_decoder) {
      m_decoder->mark_complete();
    }
  }

  // True once the header is in; errors in it are thrown.
  bool header() {
    if (m_has_header) {
      return true;
    }
    if (!m_decoder || (!m_complete && !m_decoder->supports_streaming()) ||
        !m_decoder->read_header()) {
      return false;
    }
    const ImageInfo &info = m_decoder->info;
    m_width = info.original_width;
    m_height = info.original_height;
    if (imagedecoder::orientation_swaps_axes(info.orientation)) {
      std::swap(m_width, m_height);
    }
    m_has_header = true;
    return true;
  }

  val decode_step() {
    if (!header()) {
      if (m_complete) {
        js_throw(m_decoder ? "Truncated image"
                           : "Not a supported image: too short");
      }
      js_throw("Need more data: push more before decoding again");
    }
    const DecodeOptions options = js_options(*m_decoder);

    // Steps between are never copied out; only the state the last leaves.
    DecodeProgress progress;
    DecodeProgress step;
    bool changed = false;
    imagedecoder::DirtyRegion dirty;
    for (;;) {
      try {
        progress = m_decoder->decode(options);
      } catch (...) {
        m_more = false; // Decoder errors are final.
        throw;
      }
      if (progress.changed) {
        changed = true;
        step = progress;
        dirty.add(progress.rect);
      }
      if (!progress.changed || progress.finished || progress.complete) {
        break;
      }
    }
    const bool whole = changed && step.finished;
    const DecodeProgress &at = changed ? step : progress;

    ImageInfo shown = at.info;
    if (changed && shown.components != 4) {
      js_throw("Image has no RGBA layout");
    }
    const uint32_t orientation =
        options.apply_orientation ? shown.orientation : 1;
    imagedecoder::InvalidRect rect = dirty.rect();

    const std::vector<uint8_t> &now = m_decoder->buffer();
    if (now.size() > MAX_OUTPUT_BYTES) {
      js_throw("Decoded image is too large");
    }

    val pixels = val::null();
    if (whole && !now.empty()) {
      pixels = js_bytes(now.data(), now.size());
      rect = {0, 0, shown.width, shown.height};
    } else if (changed && at.stride > 0 && shown.original_height > 0 &&
               (uint64_t)now.size() >=
                   (uint64_t)at.stride * (shown.original_height - 1) +
                       imagedecoder::row_bytes(shown)) {
      // No stride or orientation for a partial frame in JS either.
      const std::vector<uint8_t> packed =
          imagedecoder::orient_copy(now.data(), at.stride, shown, orientation);
      pixels = js_bytes(packed.data(), packed.size());
      if (orientation >= 2 && orientation <= 8) {
        if (imagedecoder::orientation_swaps_axes(orientation)) {
          std::swap(shown.width, shown.height);
        }
        rect = {0, 0, shown.width, shown.height};
      }
    }

    if (pixels.isNull()) {
      if (progress.complete) {
        m_more = false;
        js_throw("No frames remain");
      }
      js_throw("Need more data: push more before decoding again");
    }
    if (progress.complete) {
      m_more = false;
    }
    m_page = at.frame;

    val frame = val::object();
    frame.set("pixels", pixels);
    frame.set("width", shown.width);
    frame.set("height", shown.height);
    frame.set("changed", changed);
    frame.set("partial", !whole);
    frame.set("index", at.frame);
    frame.set("duration", at.duration_ms);
    frame.set("dirtyX", rect.x);
    frame.set("dirtyY", rect.y);
    frame.set("dirtyWidth", rect.width);
    frame.set("dirtyHeight", rect.height);
    frame.set("halfFloat",
              options.output_mode == imagedecoder::OutputMode::RgbaF16);
    return frame;
  }

  std::unique_ptr<BaseDecoder> m_decoder;
  std::vector<uint8_t> m_sniff;
  uint64_t m_total = 0;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_page = 0;
  bool m_more = true;
  bool m_closed = false;
  bool m_complete = false;
  bool m_has_header = false;
};

val supportedFormats() {
  val out = val::array();
  for (const std::string &f : imagedecoder::supported_formats()) {
    out.call<void>("push", f);
  }
  return out;
}

// Bytes the module's allocator has handed out: a decoder never delete()d
// shows up here.
double memoryInUse() { return (double)mallinfo().uordblks; }

EMSCRIPTEN_BINDINGS(imagedecoder) {
  emscripten::class_<ImageDecoder>("ImageDecoder")
      .class_function("open", &ImageDecoder::open)
      .function("pushData", &ImageDecoder::pushData)
      .function("markComplete", &ImageDecoder::markComplete)
      .function("decodeNext", &ImageDecoder::decodeNext)
      .function("getGainmap", &ImageDecoder::getGainmap)
      .function("listTags", &ImageDecoder::listTags)
      .function("getTag", &ImageDecoder::getTag)
      .function("close", &ImageDecoder::close)
      .property("format", &ImageDecoder::format)
      .property("width", &ImageDecoder::width)
      .property("height", &ImageDecoder::height)
      .property("hasNext", &ImageDecoder::hasNext)
      .property("page", &ImageDecoder::page)
      .property("pages", &ImageDecoder::pages)
      .property("loopCount", &ImageDecoder::loopCount)
      .property("isHdr", &ImageDecoder::isHdr)
      .property("hdrKind", &ImageDecoder::hdrKind)
      .property("hdrHeadroom", &ImageDecoder::hdrHeadroom);
  emscripten::function("supportedFormats", &supportedFormats);
  emscripten::function("memoryInUse", &memoryInUse);
}
