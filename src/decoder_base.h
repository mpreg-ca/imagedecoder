#pragma once

#include "hdr.h"
#include "lcms2.h"
#include <functional>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace imagedecoder {

// Values match VapourSynth's VSColorFamily/VSSampleType.
enum class ColorFamily : uint32_t {
  Undefined = 0,
  Gray = 1,
  RGB = 2,
  YUV = 3,
};

enum class SampleType : uint32_t {
  Integer = 0,
  Float = 1,
};

struct ImageInfo final {
  // What is in the buffer: the stored size until a turn, the turned size after.
  uint32_t width = 0;
  uint32_t height = 0;
  // As stored in the file, before any turn.
  uint32_t original_width = 0;
  uint32_t original_height = 0;
  uint32_t components = 0;
  bool has_alpha = false;
  ColorFamily color = ColorFamily::Undefined;
  SampleType sample_type = SampleType::Integer;
  uint32_t bits = 0;
  uint32_t subsampling_w = 0;
  uint32_t subsampling_h = 0;
  int yuv_matrix = 1;

  bool full_range = true;

  uint32_t orientation = 1;
};

inline bool orientation_swaps_axes(uint32_t orientation) {
  return orientation >= 5 && orientation <= 8;
}

inline uint32_t padded_width(const ImageInfo &info) {
  if (info.subsampling_w >= 8) {
    return info.original_width;
  }
  uint32_t mask = (1u << info.subsampling_w) - 1;
  return (info.original_width + mask) & ~mask;
}

inline uint32_t padded_height(const ImageInfo &info) {
  if (info.subsampling_h >= 8) {
    return info.original_height;
  }
  uint32_t mask = (1u << info.subsampling_h) - 1;
  return (info.original_height + mask) & ~mask;
}

// A rect, since progress comes as whole-frame passes, row bands or tiles.
struct InvalidRect final {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool empty() const { return width == 0 || height == 0; }
};

class DirtyRegion final {
public:
  void add(const InvalidRect &r) {
    if (r.empty()) {
      return;
    }
    if (m_rect.empty()) {
      m_rect = r;
      return;
    }
    uint32_t left = std::min(m_rect.x, r.x);
    uint32_t top = std::min(m_rect.y, r.y);
    uint32_t right = std::max(m_rect.x + m_rect.width, r.x + r.width);
    uint32_t bottom = std::max(m_rect.y + m_rect.height, r.y + r.height);
    m_rect = InvalidRect{left, top, right - left, bottom - top};
  }

  void replace(const InvalidRect &r) { m_rect = r; }

  void clear() { m_rect = InvalidRect{}; }
  const InvalidRect &rect() const { return m_rect; }

private:
  InvalidRect m_rect;
};

struct DecodeProgress final {
  bool changed = false;

  bool complete = false;

  InvalidRect rect;

  uint32_t frame = 0;

  uint32_t duration_ms = 0;

  // buffer()'s layout: partial frames are unturned at the decoder's stride.
  ImageInfo info;
  size_t stride = 0;
  // buffer() holds a whole frame, packed and turned; otherwise it is partial.
  bool finished = false;
};

// Owns the input rather than borrowing it, so a file can still be arriving.
class SourceBuffer final {
public:
  void push(const uint8_t *data, size_t size) {
    if (m_complete) {
      throw std::runtime_error("Data pushed after the source was completed");
    }
    m_data.insert(m_data.end(), data, data + size);
  }

  void mark_complete() { m_complete = true; }

  bool complete() const { return m_complete; }
  const uint8_t *data() const { return m_data.data(); }
  size_t size() const { return m_data.size(); }

  size_t consumed = 0;

  size_t available() const {
    return consumed < m_data.size() ? m_data.size() - consumed : 0;
  }

private:
  std::vector<uint8_t> m_data;
  bool m_complete = false;
};

enum class OutputMode : uint32_t {
  Original = 0,

  YUVtoRGB = 1,

  Rgba8 = 2,

  RgbaF16 = 3,
};

inline bool expands_to_rgba(OutputMode mode) {
  return mode == OutputMode::Rgba8 || mode == OutputMode::RgbaF16;
}

// Values match the Kotlin enum's ordinals.
enum class HdrKind : uint32_t {
  None = 0,
  Gainmap = 1, // SDR base picture plus an unapplied gain map - UltraHDR.
  PQ = 2,
  HLG = 3,
  Linear = 4,
};

enum : int {
  CICP_TRANSFER_BT709 = 1,
  CICP_TRANSFER_LINEAR = 8,
  CICP_TRANSFER_SRGB = 13,
  CICP_TRANSFER_PQ = 16,
  CICP_TRANSFER_HLG = 18,
};

HdrKind hdr_kind_for_transfer(int transfer);

// A container's stated peak is believed; otherwise the format's nominal one.
float hdr_headroom_for(HdrKind kind, float peak_nits);

struct GainmapData final {
  std::vector<uint8_t>
      pixels; // Tightly packed 8-bit, usually smaller than the base image.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;

  float gamma[3] = {1.0f, 1.0f, 1.0f};
  float min_content_boost[3] = {1.0f, 1.0f, 1.0f};
  float max_content_boost[3] = {1.0f, 1.0f, 1.0f};
  float offset_sdr[3] = {0.0f, 0.0f, 0.0f};
  float offset_hdr[3] = {0.0f, 0.0f, 0.0f};
  // ISO 21496-1 weighting, in stops: none of the map applies at or below the
  // base headroom, all of it at or above the alternate one.
  float base_headroom = 0.0f;
  float alternate_headroom = 0.0f;

  bool empty() const { return pixels.empty() || width == 0 || height == 0; }
};

struct DecodeOptions final {
  bool srgb_output = false;

  OutputMode output_mode = OutputMode::YUVtoRGB;

  bool apply_orientation = false;

  bool operator==(const DecodeOptions &) const = default;
};

cmsUInt32Number cms_format(const ImageInfo &info);

inline bool can_expand_rgba(const ImageInfo &info) {
  return cms_format(info) != 0;
}

// Stored-orientation layouts: decoders fill the buffer before any turn.
inline size_t row_bytes(const ImageInfo &info) {
  return (size_t)info.original_width * info.components * ((info.bits + 7) / 8);
}

// An Rgba mode widens to four channels; otherwise the row is the decoded one.
inline size_t output_row_bytes(const ImageInfo &dec,
                               const DecodeOptions &opts) {
  if (!expands_to_rgba(opts.output_mode) || !can_expand_rgba(dec)) {
    return row_bytes(dec);
  }
  return (size_t)dec.original_width * 4 *
         (opts.output_mode == OutputMode::RgbaF16 ? 2 : 1);
}

// Converted where rows lie, so the buffer must fit both.
inline size_t layout_stride(const ImageInfo &dec, const DecodeOptions &opts) {
  return std::max(row_bytes(dec), output_row_bytes(dec, opts));
}

// 268M pixels: past any real photograph.
inline constexpr uint64_t MAX_IMAGE_PIXELS = (uint64_t)1 << 28;

inline constexpr uint64_t MAX_IMAGE_BYTES = (uint64_t)INT32_MAX;

// A minute a frame is already absurd; past that a player just hangs.
inline constexpr uint32_t MAX_FRAME_DURATION_MS = 60000;

inline constexpr uint32_t MAX_FRAMES = 100000;

// Frames x canvas pixels: each frame re-composites and converts the canvas.
inline constexpr uint64_t MAX_ANIMATION_PIXELS = (uint64_t)1 << 34;

// libjxl threads: two get most of the speed-up; three exceed a 256MB budget.
#ifdef IMAGEDECODER_SINGLE_THREADED
inline constexpr uint32_t MAX_DECODE_THREADS = 1;
#else
inline constexpr uint32_t MAX_DECODE_THREADS = 2;
#endif

void check_dimensions(uint64_t width, uint64_t height);

size_t checked_buffer_size(const ImageInfo &info);

size_t checked_buffer_bytes(size_t stride, uint32_t height);

uint32_t checked_frame_count(uint64_t frames);

cmsHPROFILE create_srgb_gray();

class SrgbTransform final {
public:
  SrgbTransform() = default;

  SrgbTransform(const ImageInfo &info, cmsHPROFILE src_profile, bool wanted);

  ~SrgbTransform() {
    if (m_transform) {
      cmsDeleteTransform(m_transform);
    }
  }

  SrgbTransform(const SrgbTransform &) = delete;
  SrgbTransform &operator=(const SrgbTransform &) = delete;

  SrgbTransform &operator=(SrgbTransform &&other) noexcept {
    std::swap(m_transform, other.m_transform);
    m_width = other.m_width;
    return *this;
  }

  explicit operator bool() const { return m_transform != nullptr; }

  void apply(uint8_t *base, size_t row_stride, uint32_t y,
             uint32_t height) const;

private:
  cmsHTRANSFORM m_transform = nullptr;
  uint32_t m_width = 0;
};

void convert_rows_to_rgba8(uint8_t *base, size_t row_stride,
                           const ImageInfo &src, uint32_t y, uint32_t height);

void convert_rows_to_f16(uint8_t *base, size_t src_row_stride,
                         size_t dst_row_stride, const ImageInfo &src_info,
                         HdrKind hdr_kind, int primaries, uint32_t y,
                         uint32_t height);

void expand_rows_to_rgba(uint8_t *base, size_t src_row_stride,
                         size_t dst_row_stride, const ImageInfo &info,
                         uint32_t y, uint32_t height);

class BaseDecoder {
public:
  BaseDecoder() = default;
  virtual ~BaseDecoder() = default;

  BaseDecoder(const BaseDecoder &) = delete;
  BaseDecoder &operator=(const BaseDecoder &) = delete;

  ImageInfo info;

  void push_data(const uint8_t *data, size_t size) {
    m_source.push(data, size);
  }

  void mark_complete() { m_source.mark_complete(); }

  bool source_complete() const { return m_source.complete(); }

  const uint8_t *source_data() const { return m_source.data(); }
  size_t source_size() const { return m_source.size(); }

  void set_data(const uint8_t *data, size_t size) {
    push_data(data, size);
    mark_complete();
  }

  bool has_info() const { return m_has_info; }

  virtual bool read_header() = 0;

  // False where the header can only be read from the whole file.
  virtual bool supports_streaming() const { return true; }

  // Non-virtual: decode_impl decodes; this compacts, turns and reports.
  DecodeProgress decode(const DecodeOptions &options);

  // Back to frame 0 over the bytes already held, to replay an animation. Needs
  // the whole file. A throw here, or one earlier, leaves the decoder failed.
  void rewind();

  /* The first frame alone, cropped to `rect` (in turned coordinates when the
   * options turn), or whole when null. Needs the whole file; rewinds first if
   * anything was decoded, so the decoder can be reused for region after region.
   * The next decode() call starts over from frame 0.
   * The result's width/height (and stride) are the region's; original_* stay
   * the image's.
   * A sample above 1 shrinks the region to ceil(size / sample), needing an
   * Rgba output mode: by the codec's own scaling where it has one (JPEG,
   * WebP, JPEG 2000), else to the nearest pixels. Codecs that can shrink or
   * stream rows do so while decoding, so the region is never whole at full
   * size. */
  DecodeProgress decode_region(const DecodeOptions &options,
                               const InvalidRect *rect = nullptr,
                               uint32_t sample = 1);

  const std::vector<uint8_t> &buffer() const { return m_buffer; }

  std::vector<uint8_t> take_buffer() { return std::move(m_buffer); }

  void restore_buffer(std::vector<uint8_t> &&pixels) {
    m_buffer = std::move(pixels);
  }

  virtual cmsHPROFILE get_color_profile() = 0;
  virtual std::string get_name() = 0;

  virtual std::vector<uint8_t> exif_data() { return {}; }

  virtual uint32_t frame_count() const { return 1; }

  virtual uint32_t loop_count() const { return 0; }

  virtual HdrKind hdr_kind() const { return HdrKind::None; }

  virtual float hdr_headroom() const { return 0.0f; }

  virtual int hdr_primaries() const { return CICP_PRIMARIES_BT709; }

  virtual const GainmapData *gainmap() { return nullptr; }

  bool is_hdr() const { return hdr_kind() != HdrKind::None; }

protected:
  virtual void restart() = 0;

  // Back to frame 0 for [rewind], the header kept. A codec with its own rewind
  // keeps its state; the rest start over.
  virtual void rewind_codec() { restart(); }

  /* Region decoding: `want` is in stored coordinates, inside the image. Returns
   * the rect the codec will make (containing `want`, e.g. widened to block
   * boundaries); the default makes the whole image, cropped afterwards.
   *
   * Called after read_header() on a fresh (rewound) codec, before the first
   * decode_impl(). Until that frame finishes, info.original_width/height (and
   * width/height) are the returned rect's size, so the buffer, stride and
   * rows the codec writes are the made rect's alone; m_region holds where it
   * lies in the image. Only the first frame is decoded. */
  virtual InvalidRect begin_region(const InvalidRect &want) {
    (void)want;
    m_region = InvalidRect{0, 0, info.original_width, info.original_height};
    return m_region;
  }

  // The made rect during a region decode, in stored coordinates; else empty.
  InvalidRect m_region;

  /* Sampled region decoding: called before begin_region() with the sample
   * (above 1), and the largest power of two dividing it that the request's
   * stored x and y are multiples of: a codec whose scaled pixels sit on a grid
   * of its own keeps to that, so they line up with the sample's blocks.
   * `want` is the request widened to whole blocks of the sample (clipped to
   * the image), which begin_region() gets when the codec returns the sample
   * itself; otherwise it gets the request. Returns the factor the codec
   * itself shrinks by, which must divide the sample; 1 by default. A codec
   * scaling all the way must keep made.x - want.x and made.y - want.y
   * multiples of the sample.
   * A codec returning f > 1 makes the made rect at
   * ceil(size / f) (info.original_* say so until the frame finishes) and
   * keeps it in m_scale. begin_region() still takes and returns full-size
   * rects. */
  virtual uint32_t begin_scale(uint32_t sample, uint32_t aligned,
                               const InvalidRect &want) {
    (void)sample;
    (void)aligned;
    (void)want;
    return 1;
  }
  uint32_t m_scale = 1;

  /* During a sampled decode, a codec whose finished rows come out in order
   * may hand them over here (after finish_layout) instead of keeping them;
   * rows y.. of the made image, `first` pointing at row y. A codec that does
   * so must hand over every row, and its buffer need not hold the frame. */
  bool sampling_rows() const { return m_sampler != nullptr; }
  void emit_rows(const uint8_t *first, size_t stride, uint32_t y, uint32_t n);
  /* For a codec whose frame sits decoded in a buffer of its own: its `rows`
   * rows (as `dec` lays them out, `src_stride` apart) a band at a time
   * through m_buffer, converted (srgb if given, then finish_layout) and
   * handed on. */
  void emit_frame(const uint8_t *src, size_t src_stride, const ImageInfo &dec,
                  const DecodeOptions &opts, const SrgbTransform *srgb,
                  uint32_t rows);

  void finish_layout(uint8_t *base, size_t stride, const ImageInfo &dec,
                     const DecodeOptions &opts, uint32_t y, uint32_t height);

  /* finished_stride: a frame just completed at this stride (reported once).
   * Otherwise partial is what advanced, unturned, at row stride `stride`. */
  struct StepResult final {
    bool complete = false;
    size_t finished_stride = 0;
    InvalidRect partial;
    size_t stride = 0;
  };

  // Given the options already reduced to what this image can honour.
  virtual StepResult decode_impl(const DecodeOptions &opts) = 0;

  uint32_t m_frame = 0;
  uint32_t m_duration_ms = 0;

  SourceBuffer m_source;
  bool m_has_info = false;
  std::vector<uint8_t> m_buffer;

private:
  // The options the buffer was laid out under; others mean starting over.
  DecodeOptions m_layout_opts;
  bool m_laid_out = false;
  // What buffer() holds once a frame has finished, reported until the next.
  bool m_shown_valid = false;
  ImageInfo m_shown;
  size_t m_shown_stride = 0;
  // Something was decoded since the last rewind.
  bool m_started = false;
  // Codec state is region-sized, so decode() must rewind first.
  bool m_region_done = false;
  // A throw leaves the codec mid-operation, so every later call repeats it.
  std::exception_ptr m_error;
  std::unique_ptr<class RowSampler> m_sampler;

  DecodeProgress decode_region_once(const DecodeOptions &options,
                                    const InvalidRect *rect, uint32_t sample);
  // `native`: let the codec shrink by itself; a refusal retries without.
  DecodeProgress decode_sampled(const DecodeOptions &options,
                                const InvalidRect &want, uint32_t sample,
                                bool native = true);
  DecodeProgress finish_sampled(DecodeProgress progress,
                                std::vector<uint8_t> &&out,
                                const ImageInfo &full, uint32_t ow, uint32_t oh,
                                uint32_t orientation);
};

/* Shrinks a frame whose rows arrive in order to its nearest pixels: output
 * pixel i is the first of input [start + i*k - phase, start + (i+1)*k -
 * phase) clipped to [start, end), or the last where `last` (an axis the
 * turn reverses, so it's first once turned), on each axis. Only those rows
 * are read. */
class RowSampler final {
public:
  struct Axis final {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t k = 1;
    uint32_t phase = 0;
    uint32_t out = 0;
    bool last = false;
  };

  RowSampler(size_t pixel_bytes, Axis x, Axis y);

  void feed(const uint8_t *first, size_t stride, uint32_t y, uint32_t n);

  // Rows fed so far, in [y.start, y.end); all of them once all are in.
  uint32_t fed() const { return m_fed; }
  bool streamed = false;

  // The out.x by out.y frame, packed.
  std::vector<uint8_t> finish();

private:
  size_t m_pixel;
  Axis m_x;
  Axis m_y;
  std::vector<uint32_t> m_cols; // input column of each output one
  std::vector<uint32_t> m_rows; // input row of each output one
  std::vector<uint8_t> m_out;
  uint32_t m_row = 0; // next output row to fill
  uint32_t m_fed = 0;
  uint32_t m_next = 0;
};

// A stored-coordinate rect of a w x h image, turned per orientation, and back
// (w x h being the stored size both ways).
InvalidRect turn_rect(const InvalidRect &r, uint32_t orientation, uint32_t w,
                      uint32_t h);
InvalidRect unturn_rect(const InvalidRect &r, uint32_t orientation, uint32_t w,
                        uint32_t h);

// Packs original_width x original_height pixels, turned per orientation.
std::vector<uint8_t> orient_copy(const uint8_t *src, size_t stride,
                                 const ImageInfo &layout, uint32_t orientation);

} // namespace imagedecoder
