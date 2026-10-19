#include "decoder_factory.h"
#include "exif.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace imagedecoder;

static std::vector<uint8_t> read_file(const char *path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    throw std::runtime_error(std::string("cannot open ") + path);
  }
  const std::streamoff end = f.tellg();
  if (end < 0) {
    throw std::runtime_error(std::string("cannot size ") + path);
  }
  std::vector<uint8_t> v((size_t)end);
  f.seekg(0);
  if (!v.empty() && !f.read((char *)v.data(), (std::streamsize)v.size())) {
    throw std::runtime_error(std::string("cannot read ") + path);
  }
  return v;
}

static void write_file(const char *path, const uint8_t *data, size_t size) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error(std::string("cannot create ") + path);
  }
  f.write((const char *)data, (std::streamsize)size);
  if (!f) {
    throw std::runtime_error(std::string("cannot write ") + path);
  }
}

static const char *kind_name(HdrKind k) {
  switch (k) {
  case HdrKind::None:
    return "none";
  case HdrKind::Gainmap:
    return "gainmap";
  case HdrKind::PQ:
    return "pq";
  case HdrKind::HLG:
    return "hlg";
  case HdrKind::Linear:
    return "linear";
  }
  return "?";
}

static const char *family_name(ColorFamily c) {
  switch (c) {
  case ColorFamily::Gray:
    return "gray";
  case ColorFamily::RGB:
    return "rgb";
  case ColorFamily::YUV:
    return "yuv";
  default:
    return "undefined";
  }
}

struct Args {
  DecodeOptions options;
  size_t chunk = 0;
  uint32_t sample = 1;
  const char *out = nullptr;
  std::vector<const char *> files;
};

static Args parse(int argc, char **argv, int from) {
  Args a;
  auto number = [&](int &i, const std::string &name) {
    if (i + 1 >= argc) {
      throw std::runtime_error(name + " needs a value");
    }
    char *end = nullptr;
    const unsigned long long v = strtoull(argv[++i], &end, 10);
    if (!end || *end || end == argv[i] || argv[i][0] == '-') {
      throw std::runtime_error(name + " wants a number, not " + argv[i]);
    }
    return v;
  };
  for (int i = from; i < argc; i++) {
    std::string s = argv[i];
    if (s == "--srgb") {
      a.options.srgb_output = true;
    } else if (s == "--rgba8") {
      a.options.output_mode = OutputMode::Rgba8;
    } else if (s == "--f16") {
      a.options.output_mode = OutputMode::RgbaF16;
    } else if (s == "--orient") {
      a.options.apply_orientation = true;
    } else if (s == "--original") {
      a.options.output_mode = OutputMode::Original;
    } else if (s == "--chunk") {
      a.chunk = (size_t)number(i, s);
    } else if (s == "--sample") {
      a.sample = (uint32_t)std::min<unsigned long long>(number(i, s), 1u << 20);
    } else if (s == "--out" && i + 1 < argc) {
      a.out = argv[++i];
    } else if (s == "--out") {
      throw std::runtime_error(s + " needs a value");
    } else if (s.rfind("--", 0) == 0) {
      throw std::runtime_error("unknown option " + s);
    } else {
      a.files.push_back(argv[i]);
    }
  }
  return a;
}

struct Run {
  std::vector<std::vector<uint8_t>> frames;
  std::vector<uint32_t> durations;
  ImageInfo out_info{};
  uint32_t steps = 0;
  uint32_t changes = 0;
};

// Pushes in chunk-sized pieces, exercising the streaming path.
static Run decode_all(BaseDecoder &dec, const std::vector<uint8_t> &data,
                      const DecodeOptions &options, size_t chunk) {
  Run r;
  size_t at = 0;
  DecodeProgress p;
  bool ended = false;
  uint32_t stalled = 0;

  do {
    if (at < data.size()) {
      size_t n = chunk ? std::min(chunk, data.size() - at) : data.size() - at;
      dec.push_data(data.data() + at, n);
      at += n;
    }
    if (!ended && at >= data.size()) {
      dec.mark_complete();
      ended = true;
    }
    p = dec.decode(options);
    r.steps++;

    if (ended && !p.changed && !p.complete && ++stalled > 4) {
      throw std::runtime_error("decoder stopped making progress");
    }

    if (p.changed) {
      stalled = 0;
      r.changes++;
    }
    if (p.changed && !p.finished && can_expand_rgba(p.info)) {
      // What the JNI relies on to pack a partial frame.
      const uint32_t h = p.info.original_height;
      if (p.stride < row_bytes(p.info) ||
          dec.buffer().size() < p.stride * (h - 1) + row_bytes(p.info)) {
        throw std::runtime_error("partial frame is smaller than its layout");
      }
    }
    if (p.changed && p.finished) {
      if (r.frames.size() <= p.frame) {
        r.frames.resize(p.frame + 1);
        r.durations.resize(p.frame + 1);
      }
      r.frames[p.frame] = dec.buffer();
      r.durations[p.frame] = p.duration_ms;
    }
  } while (!p.complete);

  r.out_info = p.info;
  return r;
}

static int cmd_info(const Args &a) {
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      printf("%s: not a supported image\n", path);
      continue;
    }
    dec->set_data(data.data(), data.size());
    dec->read_header();

    printf("%s\n", path);
    printf("  format      %s\n", dec->get_name().c_str());
    printf("  size        %ux%u\n", dec->info.original_width,
           dec->info.original_height);
    printf("  stored      %s %u x %u-bit%s\n", family_name(dec->info.color),
           dec->info.components, dec->info.bits,
           dec->info.has_alpha ? " +alpha" : "");
    if (dec->info.subsampling_w || dec->info.subsampling_h) {
      printf("  subsampling %u,%u\n", dec->info.subsampling_w,
             dec->info.subsampling_h);
    }
    printf("  orientation %u%s\n", dec->info.orientation,
           dec->info.orientation == 1 ? "" : " (turned)");
    printf("  frames      %u\n", dec->frame_count());
    printf("  hdr         %s", kind_name(dec->hdr_kind()));
    if (dec->hdr_kind() != HdrKind::None) {
      printf("  headroom %.2f stops", dec->hdr_headroom());
    }
    printf("\n");
    if (const GainmapData *g = dec->gainmap()) {
      printf("  gainmap     %ux%u x %u, max boost %.3f\n", g->width, g->height,
             g->channels,
             std::max({g->max_content_boost[0], g->max_content_boost[1],
                       g->max_content_boost[2]}));
    }
    printf("  icc         %s\n", dec->get_color_profile() ? "yes" : "none");
    auto exif = dec->exif_data();
    printf("  exif        %zu bytes\n", exif.size());
  }
  return 0;
}

static int cmd_tags(const Args &a) {
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      continue;
    }
    dec->set_data(data.data(), data.size());
    dec->read_header();

    auto exif = dec->exif_data();
    auto names = exif_list_tags(exif.data(), exif.size());
    printf("%s: %zu tags\n", path, names.size());
    for (const std::string &name : names) {
      std::string value;
      if (exif_get_tag(exif.data(), exif.size(), name.c_str(), &value)) {
        if (value.size() > 72) {
          value = value.substr(0, 72) + "...";
        }
        printf("  %-28s %s\n", name.c_str(), value.c_str());
      } else {
        printf("  %-28s <unreadable>\n", name.c_str());
      }
    }
  }
  return 0;
}

static int cmd_decode(const Args &a) {
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      printf("%s: not a supported image\n", path);
      return 1;
    }
    if (a.sample > 1) {
      // The whole first frame, shrunk while it decodes.
      dec->set_data(data.data(), data.size());
      const DecodeProgress p = dec->decode_region(a.options, nullptr, a.sample);
      printf("%s: %ux%u /%u, %zu bytes\n", path, p.info.width, p.info.height,
             a.sample, dec->buffer().size());
      if (a.out) {
        write_file(a.out, dec->buffer().data(), dec->buffer().size());
        printf("  wrote %s\n", a.out);
      }
      continue;
    }
    Run r = decode_all(*dec, data, a.options, a.chunk);

    printf("%s: %ux%u %s %u x %u-bit %s, %zu bytes, %u steps, %u changes, "
           "%zu frames\n",
           path, r.out_info.width, r.out_info.height,
           family_name(r.out_info.color), r.out_info.components,
           r.out_info.bits,
           r.out_info.sample_type == SampleType::Float ? "float" : "integer",
           dec->buffer().size(), r.steps, r.changes, r.frames.size());

    if (a.out) {
      write_file(a.out, dec->buffer().data(), dec->buffer().size());
      printf("  wrote %s\n", a.out);
    }
  }
  return 0;
}

static float half_value(const uint8_t *p) {
  uint16_t h;
  memcpy(&h, p, 2);
  const int e = (h >> 10) & 0x1f;
  const int m = h & 0x3ff;
  float v = e == 0    ? std::ldexp((float)m, -24)
            : e == 31 ? 0.0f
                      : std::ldexp((float)(m | 0x400), e - 25);
  return h & 0x8000 ? -v : v;
}

// Sampled region decodes against the whole frame: exactly its nearest pixels
// where the codec doesn't shrink natively, else near a box filter of it.
static int cmd_region_sampled(const Args &a) {
  using clock = std::chrono::steady_clock;
  const uint32_t s = a.sample;
  int bad = 0;
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      printf("%s: not a supported image\n", path);
      bad++;
      continue;
    }
    dec->set_data(data.data(), data.size());
    DecodeProgress p = dec->decode_region(a.options);
    const std::vector<uint8_t> full = dec->buffer();
    const uint32_t w = p.info.width;
    const uint32_t h = p.info.height;
    if (p.info.components != 4) {
      printf("%s: not RGBA, skipped\n", path);
      continue;
    }
    const bool f16 = p.info.bits == 16;
    const size_t pixel = f16 ? 8 : 4;
    auto value = [&](const uint8_t *px, int c) {
      return f16 ? (double)half_value(px + c * 2) : (double)px[c];
    };
    printf("%s: %s %ux%u /%u\n", path, dec->get_name().c_str(), w, h, s);
    // The whole image sampled: aligned regions must be exact crops of it.
    const DecodeProgress wp = dec->decode_region(a.options, nullptr, s);
    const std::vector<uint8_t> whole = dec->buffer();

    std::vector<InvalidRect> rects = {
        {0, 0, w, h},
        {w / 3, h / 3, std::max(1u, w / 3), std::max(1u, h / 3)},
        {w / 5 + 1, h / 7 + 3, std::max(1u, w / 2 - 3),
         std::max(1u, h / 4 - 1)},
        {w - 1, h - 1, 1, 1},
    };
    for (uint32_t i = 1; i <= 3; i++) {
      const uint32_t x = w * i / 4 / s * s, y = h * i / 5 / s * s;
      if ((w - x) / s && (h - y) / s) {
        rects.push_back(
            {x, y, (w - x) / s * s, std::min((h - y) / s, 7 * i + 1) * s});
        rects.push_back({x / 2 / s * s, y,
                         std::min((w - x / 2 / s * s) / s, 13u) * s,
                         (h - y) / s * s});
      }
    }
    uint32_t seed = 777;
    auto rnd = [&](uint32_t n) {
      seed = seed * 1103515245 + 12345;
      return (seed >> 8) % n;
    };
    for (int i = 0; i < 5; i++) {
      uint32_t x = rnd(w), y = rnd(h);
      rects.push_back({x, y, 1 + rnd(w - x), 1 + rnd(h - y)});
    }

    for (const InvalidRect &r : rects) {
      if (r.x + r.width > w || r.y + r.height > h || r.empty()) {
        continue;
      }
      const auto t0 = clock::now();
      DecodeProgress rp;
      try {
        rp = dec->decode_region(a.options, &r, s);
      } catch (const std::exception &e) {
        printf("  %5u,%-5u %5ux%-5u ERROR %s\n", r.x, r.y, r.width, r.height,
               e.what());
        bad++;
        // A failed decoder stays failed; carry on with a fresh one.
        dec = create_decoder(data.data(), data.size());
        if (!dec) {
          break;
        }
        dec->set_data(data.data(), data.size());
        continue;
      }
      const double t =
          std::chrono::duration<double, std::milli>(clock::now() - t0).count();
      const std::vector<uint8_t> &got = dec->buffer();
      const uint32_t ow = (r.width + s - 1) / s;
      const uint32_t oh = (r.height + s - 1) / s;
      const bool shape = rp.info.width == ow && rp.info.height == oh &&
                         rp.stride == ow * pixel &&
                         got.size() == rp.stride * oh;
      // Edge blocks either stop at the region's edge, or (where the codec
      // scales natively) run on a whole block past it, from wherever the
      // region starts once turned: either passes.
      // Or, where libwebp scales a region whose size isn't whole blocks, each
      // pixel spread evenly over it (as a viewer would stretch the result).
      auto compare = [&](int mode, double *sum_out, double *worst_out) {
        const bool whole_blocks = mode == 1;
        double sum = 0, worst = 0;
        if (shape) {
          for (uint32_t oy = 0; oy < oh; oy++) {
            for (uint32_t ox = 0; ox < ow; ox++) {
              int64_t bx = (int64_t)r.x + (int64_t)ox * s;
              int64_t by = (int64_t)r.y + (int64_t)oy * s;
              int64_t ex = std::min<int64_t>(bx + s, r.x + r.width);
              int64_t ey = std::min<int64_t>(by + s, r.y + r.height);
              if (whole_blocks) {
                ex = std::min<int64_t>(bx + s, w);
                ey = std::min<int64_t>(by + s, h);
              } else if (mode == 2) {
                bx = r.x + (int64_t)ox * r.width / ow;
                by = r.y + (int64_t)oy * r.height / oh;
                ex = std::max(bx + 1, r.x + ((int64_t)ox + 1) * r.width / ow);
                ey = std::max(by + 1, r.y + ((int64_t)oy + 1) * r.height / oh);
              }
              const uint32_t x0 = (uint32_t)bx, y0 = (uint32_t)by;
              const uint32_t x1 = (uint32_t)ex, y1 = (uint32_t)ey;
              double acc[4] = {0, 0, 0, 0};
              for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {
                  const uint8_t *px = full.data() + p.stride * y + x * pixel;
                  const double al = value(px, 3);
                  for (int c = 0; c < 3; c++) {
                    acc[c] += value(px, c) * al;
                  }
                  acc[3] += al;
                }
              }
              const double n = (double)(x1 - x0) * (y1 - y0);
              const uint8_t *g = got.data() + rp.stride * oy + ox * pixel;
              for (int c = 0; c < 4; c++) {
                const double want =
                    c == 3 ? acc[3] / n : (acc[3] > 0 ? acc[c] / acc[3] : 0);
                // Colour under no alpha is anything.
                if (c < 3 && acc[3] == 0) {
                  continue;
                }
                const double d = std::fabs(value(g, c) - want);
                sum += d;
                worst = std::max(worst, d);
              }
            }
          }
        }
        *sum_out = sum;
        *worst_out = worst;
      };
      // Each block's first pixel, once turned, exactly.
      bool exact = shape;
      for (uint32_t oy = 0; oy < oh && exact; oy++) {
        exact = memcmp(got.data() + rp.stride * oy,
                       full.data() + p.stride * (r.y + oy * s), 0) == 0;
        for (uint32_t ox = 0; ox < ow && exact; ox++) {
          exact = memcmp(got.data() + rp.stride * oy + ox * pixel,
                         full.data() + p.stride * (r.y + oy * s) +
                             (size_t)(r.x + ox * s) * pixel,
                         pixel) == 0;
        }
      }
      double sum, worst;
      compare(0, &sum, &worst);
      for (int mode = 1; mode <= 2; mode++) {
        double sum2, worst2;
        compare(mode, &sum2, &worst2);
        if (sum2 < sum) {
          sum = sum2;
          worst = worst2;
        }
      }
      // A region on the sample's grid. (Not one ending off it at the edge:
      // libwebp stretches such a region's few pixels over its last block.)
      // libwebp spreads a whole image's partial last block over the row, so
      // there only images of whole blocks have crops to match. Its rescaler
      // runs through premultiplied alpha, so it is compared premultiplied, a
      // few steps either way passing. Lossy WebP scales its YUV planes before
      // converting them, which shifts with the crop, so it isn't checked.
      bool crop_ok = true;
      const bool webp = dec->get_name() == "WEBP";
      const bool lossy_webp =
          webp && data.size() >= 16 &&
          std::string(data.begin() + 12, data.begin() + 16) != "VP8L" &&
          !(data.size() > 30 && memcmp(data.data() + 12, "VP8X", 4) == 0 &&
            std::search(data.begin(), data.end(), "VP8L", "VP8L" + 4) !=
                data.end());
      const bool on_grid = r.x % s == 0 && r.y % s == 0 && r.width % s == 0 &&
                           r.height % s == 0 && !(webp && (w % s || h % s)) &&
                           !lossy_webp;
      if (shape && on_grid) {
        for (uint32_t oy = 0; oy < oh && crop_ok; oy++) {
          const uint8_t *g = got.data() + rp.stride * oy;
          const uint8_t *c = whole.data() + wp.stride * (r.y / s + oy) +
                             (size_t)(r.x / s) * pixel;
          for (size_t k = 0; k < rp.stride && crop_ok; k++) {
            if (!webp) {
              crop_ok = g[k] == c[k];
              continue;
            }
            const size_t a0 = k / pixel * pixel;
            const double ga = value(g + a0, 3), ca = value(c + a0, 3);
            const int ch = (int)(k % pixel) / (f16 ? 2 : 1);
            const double scale = f16 ? 1.0 : 255.0;
            const double gv = ch == 3 ? ga : value(g + a0, ch) * ga / scale;
            const double cv = ch == 3 ? ca : value(c + a0, ch) * ca / scale;
            crop_ok = std::fabs(gv - cv) <= (f16 ? 3.0 / 255 : 3.0);
          }
        }
      }
      const double mean = shape ? sum / ((double)ow * oh * 4) : 0;
      // Native shrinking (JPEG's scaled IDCT, libwebp's), perhaps with the
      // nearest pixels of that after, is neither, so only gross misses fail;
      // exactness is the crop check's. A JPEG 2000 resolution level is the
      // wavelet's low band, sharper than any box, so there the crop check
      // alone holds.
      const double scale = f16 ? 1.0 / 255 : 1.0;
      const bool ok =
          shape && crop_ok &&
          (exact || mean <= 32.0 * scale || dec->get_name() == "JP2");
      printf("  %5u,%-5u %5ux%-5u -> %ux%u %8.1f ms  %s%s", r.x, r.y, r.width,
             r.height, ow, oh, t,
             !shape     ? "BAD SHAPE"
             : !crop_ok ? "NOT A CROP"
             : exact    ? "nearest"
             : ok       ? "ok"
                        : "DIFFERS",
             on_grid && crop_ok && shape ? " =crop" : "");
      if (shape) {
        printf(" (mean %.3f max %.3f)", mean / scale, worst / scale);
      }
      printf("\n");
      if (!ok) {
        bad++;
      }
    }

    if (!dec) {
      continue;
    }
    // Plain decoding after a sampled region starts over, whole-sized.
    DecodeProgress after;
    do {
      after = dec->decode(a.options);
    } while (after.changed && !after.finished && !after.complete);
    const DecodeProgress again = dec->decode_region(a.options);
    const bool restarted = after.finished && after.frame == 0 &&
                           again.finished && dec->buffer() == full;
    printf("  decode() after sampled region: %s\n",
           restarted ? "identical" : "WRONG");
    if (!restarted) {
      bad++;
    }
  }
  return bad ? 1 : 0;
}

// Region decodes, on one reused decoder, must match crops of the whole frame.
static int cmd_region(const Args &a) {
  if (a.sample > 1) {
    return cmd_region_sampled(a);
  }
  using clock = std::chrono::steady_clock;
  auto ms = [](clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  };
  int bad = 0;
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      printf("%s: not a supported image\n", path);
      bad++;
      continue;
    }
    dec->set_data(data.data(), data.size());

    auto t0 = clock::now();
    DecodeProgress p = dec->decode_region(a.options);
    const double full_ms = ms(t0);
    const std::vector<uint8_t> full = dec->buffer();
    const size_t full_stride = p.stride;
    const uint32_t w = p.info.width;
    const uint32_t h = p.info.height;
    if (!can_expand_rgba(p.info)) {
      printf("%s: planar output, skipped\n", path);
      continue;
    }
    const size_t pixel = (size_t)p.info.components * ((p.info.bits + 7) / 8);
    printf("%s: %s %ux%u full %.1f ms\n", path, dec->get_name().c_str(), w, h,
           full_ms);

    std::vector<InvalidRect> rects = {
        {0, 0, w, h},
        {0, 0, 1, 1},
        {w - 1, h - 1, 1, 1},
        {w / 3, h / 3, std::max(1u, w / 3), std::max(1u, h / 3)},
        {w / 2, 0, w - w / 2, std::max(1u, h / 5)},
        {0, h / 2, std::max(1u, w / 7), h - h / 2},
        {w / 5 + 1, h / 7 + 3, std::max(1u, w / 2 - 3),
         std::max(1u, h / 4 - 1)},
        {0, h - std::max(1u, h / 9), w, std::max(1u, h / 9)},
    };
    uint32_t seed = 12345;
    auto rnd = [&](uint32_t n) {
      seed = seed * 1103515245 + 12345;
      return (seed >> 8) % n;
    };
    for (int i = 0; i < 8; i++) {
      uint32_t x = rnd(w), y = rnd(h);
      rects.push_back({x, y, 1 + rnd(w - x), 1 + rnd(h - y)});
    }

    for (const InvalidRect &r : rects) {
      if (r.x + r.width > w || r.y + r.height > h || r.empty()) {
        continue;
      }
      t0 = clock::now();
      DecodeProgress rp = dec->decode_region(a.options, &r);
      const double t = ms(t0);
      const std::vector<uint8_t> &got = dec->buffer();
      int maxdiff = 0;
      size_t ndiff = 0;
      bool shape = rp.info.width == r.width && rp.info.height == r.height &&
                   rp.stride == r.width * pixel &&
                   got.size() == rp.stride * r.height;
      if (shape) {
        for (uint32_t y = 0; y < r.height; y++) {
          const uint8_t *g = got.data() + rp.stride * y;
          const uint8_t *f =
              full.data() + full_stride * (r.y + y) + r.x * pixel;
          for (size_t k = 0; k < r.width * pixel; k++) {
            int d = std::abs((int)g[k] - (int)f[k]);
            if (d) {
              ndiff++;
              maxdiff = std::max(maxdiff, d);
            }
          }
        }
      }
      const bool ok = shape && maxdiff == 0;
      printf("  %5u,%-5u %5ux%-5u %8.1f ms  %s", r.x, r.y, r.width, r.height, t,
             !shape ? "BAD SHAPE"
             : ok   ? "identical"
                    : "DIFFERS");
      if (shape && !ok) {
        printf(" (%zu bytes, max %d)", ndiff, maxdiff);
      }
      printf("\n");
      if (!ok) {
        bad++;
      }
    }

    // Plain decoding after a region starts over at frame 0, whole-sized.
    DecodeProgress after;
    do {
      after = dec->decode(a.options);
    } while (after.changed && !after.finished && !after.complete);
    const bool restarted =
        after.finished && after.frame == 0 && dec->buffer() == full;
    printf("  decode() after region: %s\n",
           restarted ? "frame 0, identical" : "WRONG");
    if (!restarted) {
      bad++;
    }
  }
  return bad ? 1 : 0;
}

// Piecewise decode must produce the same bytes as whole-file decode.
static int cmd_stream(const Args &a) {
  int bad = 0;
  size_t chunk = a.chunk ? a.chunk : 997;

  for (const char *path : a.files) {
    auto data = read_file(path);

    auto whole_dec = create_decoder(data.data(), data.size());
    auto part_dec = create_decoder(data.data(), data.size());
    if (!whole_dec || !part_dec) {
      printf("%s: not a supported image\n", path);
      bad++;
      continue;
    }

    Run whole = decode_all(*whole_dec, data, a.options, 0);
    Run part = decode_all(*part_dec, data, a.options, chunk);

    bool same = whole_dec->buffer() == part_dec->buffer();
    printf("%s: chunk=%zu  whole=%u steps / part=%u steps  frames %zu/%zu  "
           "identical=%s\n",
           path, chunk, whole.steps, part.steps, whole.frames.size(),
           part.frames.size(), same ? "yes" : "NO");
    if (!same) {
      bad++;
    }
  }
  if (bad) {
    printf("%d file(s) differ\n", bad);
  }
  return bad ? 1 : 0;
}

static int cmd_frames(const Args &a) {
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      continue;
    }
    Run r = decode_all(*dec, data, a.options, a.chunk);

    printf("%s: %zu frames of %u\n", path, r.frames.size(), dec->frame_count());
    for (size_t i = 0; i < r.frames.size(); i++) {
      uint64_t sum = 0;
      for (uint8_t b : r.frames[i]) {
        sum = sum * 131 + b;
      }
      printf("  frame %-3zu %5u ms  %zu bytes  hash %016llx%s\n", i,
             r.durations[i], r.frames[i].size(), (unsigned long long)sum,
             i && r.frames[i] == r.frames[i - 1] ? "  (same as previous)" : "");
    }
  }
  return 0;
}

static int cmd_gainmap(const Args &a) {
  for (const char *path : a.files) {
    auto data = read_file(path);
    auto dec = create_decoder(data.data(), data.size());
    if (!dec) {
      printf("%s: not a supported image\n", path);
      return 1;
    }
    dec->set_data(data.data(), data.size());
    dec->read_header();
    const GainmapData *g = dec->gainmap();
    if (!g) {
      printf("%s: no gain map\n", path);
      continue;
    }
    printf("%s: %ux%u x %u, gamma %.3f, boost %.3f..%.3f\n", path, g->width,
           g->height, g->channels, g->gamma[0], g->min_content_boost[0],
           g->max_content_boost[0]);
    if (a.out) {
      write_file(a.out, g->pixels.data(), g->pixels.size());
      printf("  wrote %s\n", a.out);
    }
  }
  return 0;
}

static void usage() {
  printf(
      "usage: imagedecoder-cli <command> [options] <file>...\n"
      "\n"
      "commands:\n"
      "  info      header, layout, frame count, HDR kind, metadata sizes\n"
      "  tags      every Exif/TIFF tag and its value\n"
      "  decode    decode and report what came back\n"
      "  stream    check that piecewise decoding matches whole-file\n"
      "  region    check that region decodes match crops of the whole\n"
      "  frames    per-frame durations and hashes of an animation\n"
      "  gainmap   the gain map's shape and metadata; --out writes its pixels\n"
      "  formats   the formats this build understands\n"
      "\n"
      "options:\n"
      "  --srgb            convert to sRGB using the embedded profile\n"
      "  --rgba8           four interleaved 8-bit channels\n"
      "  --f16             four interleaved half-float channels\n"
      "  --original        keep a YUV JPEG or AVIF as planar subsampled YUV\n"
      "  --orient          turn the picture per the Exif Orientation tag\n"
      "  --chunk N         feed the file N bytes at a time\n"
      "  --sample N        decode: the first frame shrunk N times as it\n"
      "                    decodes; region: each region so, checked\n"
      "                    against a box filter of the whole frame\n"
      "  --out FILE        write the decoded pixels\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }

  std::string cmd = argv[1];
  try {
    if (cmd == "formats") {
      printf("decode:");
      for (const std::string &f : supported_formats()) {
        printf(" %s", f.c_str());
      }
      printf("\n");
      return 0;
    }

    Args a = parse(argc, argv, 2);
    if (a.files.empty() && cmd != "formats") {
      usage();
      return 2;
    }

    if (cmd == "info") {
      return cmd_info(a);
    }
    if (cmd == "tags") {
      return cmd_tags(a);
    }
    if (cmd == "decode") {
      return cmd_decode(a);
    }
    if (cmd == "region") {
      return cmd_region(a);
    }
    if (cmd == "stream") {
      return cmd_stream(a);
    }
    if (cmd == "gainmap") {
      return cmd_gainmap(a);
    }
    if (cmd == "frames") {
      return cmd_frames(a);
    }

    usage();
    return 2;
  } catch (const std::exception &e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
