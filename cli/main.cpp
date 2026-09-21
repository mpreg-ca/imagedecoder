
#include "decoder_factory.h"
#include "exif.h"

#include <algorithm>
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
