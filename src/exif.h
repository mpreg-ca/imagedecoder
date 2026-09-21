#pragma once

// libtiff gives tag names only; values come from the IFD bytes.
// Bounds are written as a > size - b, never a + b > size.

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <tiffio.h>

namespace imagedecoder {

extern "C" const TIFFFieldArray *_TIFFGetFields(void);

// Caps on what a hostile file can spend; none reachable by a real image.
static const uint64_t EXIF_MAX_IFD_ENTRIES = 4096;
static const uint64_t EXIF_MAX_COMPONENTS = 64;
static const size_t EXIF_MAX_VALUE_BYTES = 4096;
static const tmsize_t EXIF_MAX_TIFF_ALLOC = 16 * 1024 * 1024;

struct TiffMemFile {
  const uint8_t *data;
  toff_t size;
  toff_t offset;
};

static tsize_t tiff_mem_read(thandle_t h, tdata_t buf, tsize_t size) {
  TiffMemFile *f = (TiffMemFile *)h;
  if (size <= 0) {
    return 0;
  }
  toff_t avail = f->offset < f->size ? f->size - f->offset : 0;
  tsize_t n = (tsize_t)std::min<toff_t>((toff_t)size, avail);
  memcpy(buf, f->data + f->offset, (size_t)n);
  f->offset += n;
  return n;
}

static tsize_t tiff_mem_write(thandle_t, tdata_t, tsize_t) { return -1; }

static toff_t tiff_mem_seek(thandle_t h, toff_t off, int whence) {
  TiffMemFile *f = (TiffMemFile *)h;
  switch (whence) {
  case SEEK_SET:
    f->offset = off;
    break;
  case SEEK_CUR:
    f->offset += off;
    break;
  case SEEK_END:
    f->offset = f->size + off;
    break;
  default:
    return (toff_t)-1;
  }
  return f->offset;
}

static int tiff_mem_close(thandle_t) { return 0; }

static toff_t tiff_mem_size(thandle_t h) { return ((TiffMemFile *)h)->size; }

// A broken IFD is skipped silently, not logged.
static int tiff_silence(TIFF *, void *, const char *, const char *, va_list) {
  return 1;
}

struct TiffBytes {
  const uint8_t *data;
  size_t size;
  bool le;
  bool big;
};

static uint64_t read_uint(const uint8_t *d, size_t n, bool le) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    v |= (uint64_t)d[le ? i : n - 1 - i] << (8 * i);
  }
  return v;
}

static size_t tiff_type_size(uint64_t type) {
  switch (type) {
  case TIFF_BYTE:
  case TIFF_ASCII:
  case TIFF_SBYTE:
  case TIFF_UNDEFINED:
    return 1;
  case TIFF_SHORT:
  case TIFF_SSHORT:
    return 2;
  case TIFF_LONG:
  case TIFF_SLONG:
  case TIFF_FLOAT:
  case TIFF_IFD:
    return 4;
  case TIFF_RATIONAL:
  case TIFF_SRATIONAL:
  case TIFF_DOUBLE:
  case TIFF_LONG8:
  case TIFF_SLONG8:
  case TIFF_IFD8:
    return 8;
  default:
    return 0;
  }
}

static void append_element(std::string &out, const uint8_t *p, uint64_t type,
                           bool le) {
  char buf[64];

  switch (type) {
  case TIFF_SBYTE:
    snprintf(buf, sizeof(buf), "%d", (int)(int8_t)p[0]);
    break;
  case TIFF_SSHORT:
    snprintf(buf, sizeof(buf), "%d", (int)(int16_t)read_uint(p, 2, le));
    break;
  case TIFF_SLONG:
    snprintf(buf, sizeof(buf), "%d", (int)(int32_t)read_uint(p, 4, le));
    break;
  case TIFF_SLONG8:
    snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)read_uint(p, 8, le));
    break;
  case TIFF_FLOAT: {
    const uint32_t bits = (uint32_t)read_uint(p, 4, le);
    float f;
    memcpy(&f, &bits, sizeof(f));
    snprintf(buf, sizeof(buf), "%.7g", (double)f);
    break;
  }
  case TIFF_DOUBLE: {
    const uint64_t bits = read_uint(p, 8, le);
    double d;
    memcpy(&d, &bits, sizeof(d));
    snprintf(buf, sizeof(buf), "%.15g", d);
    break;
  }
  case TIFF_RATIONAL:
  case TIFF_SRATIONAL: {
    const bool sign = type == TIFF_SRATIONAL;
    const double num = sign ? (double)(int32_t)read_uint(p, 4, le)
                            : (double)read_uint(p, 4, le);
    const double den = sign ? (double)(int32_t)read_uint(p + 4, 4, le)
                            : (double)read_uint(p + 4, 4, le);
    snprintf(buf, sizeof(buf), "%.10g", den == 0 ? 0.0 : num / den);
    break;
  }
  default: // BYTE, SHORT, LONG, LONG8 and the IFD types are unsigned.
    snprintf(buf, sizeof(buf), "%llu",
             (unsigned long long)read_uint(p, tiff_type_size(type), le));
    break;
  }

  out += buf;
}

static bool looks_like_text(const uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (p[i] != 0 && p[i] != '\t' && (p[i] < 0x20 || p[i] > 0x7e)) {
      return false;
    }
  }
  return len > 0;
}

static bool format_entry(const TiffBytes &b, const uint8_t *entry,
                         std::string *out) {
  const uint64_t type = read_uint(entry + 2, 2, b.le);
  const uint64_t count = read_uint(entry + 4, b.big ? 8 : 4, b.le);
  const size_t elem = tiff_type_size(type);
  if (elem == 0 || count == 0 || count > b.size / elem) {
    return false;
  }

  const uint64_t total = count * elem;
  const size_t inline_size = b.big ? 8 : 4;
  const uint8_t *value = entry + (b.big ? 12 : 8);
  if (total > inline_size) {
    const uint64_t offset = read_uint(value, inline_size, b.le);
    if (offset < 8 || offset > b.size - total) {
      return false;
    }
    value = b.data + offset;
  }

  out->clear();

  if (type == TIFF_ASCII ||
      (type == TIFF_UNDEFINED && looks_like_text(value, (size_t)total))) {
    size_t len = strnlen((const char *)value,
                         std::min<size_t>((size_t)total, EXIF_MAX_VALUE_BYTES));
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\0')) {
      len--;
    }
    out->assign((const char *)value, len);
    return !out->empty();
  }

  // Bounded prefix: a colour map runs to hundreds of components.
  const uint64_t shown = std::min<uint64_t>(count, EXIF_MAX_COMPONENTS);
  for (uint64_t i = 0; i < shown; i++) {
    if (i) {
      out->append(", ");
    }
    append_element(*out, value + i * elem, type, b.le);
  }
  if (shown < count) {
    out->append(", ...");
  }

  return true;
}

// Sub-IFD pointers are returned to follow, not visited as tags.
template <typename Visit>
static bool visit_ifd(TIFF *tiff, const TiffBytes &b, uint64_t offset,
                      Visit &visit, uint64_t *exif_ifd = nullptr,
                      uint64_t *gps_ifd = nullptr) {
  const size_t count_size = b.big ? 8 : 2;
  const size_t entry_size = b.big ? 20 : 12;
  if (offset < 8 || b.size < count_size || offset > b.size - count_size) {
    return true;
  }

  uint64_t count = read_uint(b.data + offset, count_size, b.le);
  if (count > (b.size - offset - count_size) / entry_size) {
    return true;
  }
  count = std::min<uint64_t>(count, EXIF_MAX_IFD_ENTRIES);

  for (uint64_t i = 0; i < count; i++) {
    const uint8_t *entry = b.data + offset + count_size + i * entry_size;
    const uint32_t tag = (uint32_t)read_uint(entry, 2, b.le);

    if (tag == TIFFTAG_EXIFIFD || tag == TIFFTAG_GPSIFD) {
      uint64_t *out = tag == TIFFTAG_EXIFIFD ? exif_ifd : gps_ifd;
      if (out) {
        *out = read_uint(entry + (b.big ? 12 : 8), b.big ? 8 : 4, b.le);
      }
      continue;
    }

    const TIFFField *fip = TIFFFieldWithTag(tiff, tag);
    const char *name = fip ? TIFFFieldName(fip) : nullptr;
    if (name && *name && !visit(name, b, entry)) {
      return false;
    }
  }

  return true;
}

// Thumbnail IFDs and a TIFF's later pages are left alone.
template <typename Visit>
static void walk_exif(const uint8_t *data, size_t size, Visit visit) {
  if (!data) {
    return;
  }
  if (size > 6 && memcmp(data, "Exif\0\0", 6) == 0) {
    data += 6;
    size -= 6;
  }
  if (size < 16) {
    return;
  }

  const bool le = data[0] == 'I' && data[1] == 'I';
  if (!le && !(data[0] == 'M' && data[1] == 'M')) {
    return;
  }

  const uint64_t magic = read_uint(data + 2, 2, le);
  uint64_t ifd0;
  if (magic == 42) {
    ifd0 = read_uint(data + 4, 4, le);
  } else if (magic == 43 && read_uint(data + 4, 2, le) == 8) {
    ifd0 = read_uint(data + 8, 8, le);
  } else {
    return;
  }

  const TiffBytes b = {data, size, le, magic == 43};

  TIFFOpenOptions *opts = TIFFOpenOptionsAlloc();
  if (!opts) {
    return;
  }
  TIFFOpenOptionsSetErrorHandlerExtR(opts, tiff_silence, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(opts, tiff_silence, nullptr);
  TIFFOpenOptionsSetMaxSingleMemAlloc(opts, EXIF_MAX_TIFF_ALLOC);

  TiffMemFile mem = {data, (toff_t)size, 0};
  TIFF *tiff = TIFFClientOpenExt("memory", "rhm", &mem, tiff_mem_read,
                                 tiff_mem_write, tiff_mem_seek, tiff_mem_close,
                                 tiff_mem_size, nullptr, nullptr, opts);
  TIFFOpenOptionsFree(opts);
  if (!tiff) {
    return;
  }

  struct TiffGuard { // Closed however [visit] leaves, a throw included.
    TIFF *t;
    ~TiffGuard() { TIFFClose(t); }
  } guard = {tiff};

  if (!TIFFReadCustomDirectory(tiff, (toff_t)ifd0, _TIFFGetFields())) {
    return;
  }

  uint64_t exif_ifd = 0;
  uint64_t gps_ifd = 0;
  if (!visit_ifd(tiff, b, ifd0, visit, &exif_ifd, &gps_ifd)) {
    return;
  }

  if (exif_ifd && TIFFReadEXIFDirectory(tiff, (toff_t)exif_ifd) &&
      !visit_ifd(tiff, b, exif_ifd, visit)) {
    return;
  }

  if (gps_ifd && TIFFReadGPSDirectory(tiff, (toff_t)gps_ifd)) {
    visit_ifd(tiff, b, gps_ifd, visit);
  }
}

// Astral characters are re-encoded as surrogate pairs rather than replaced.
inline std::string make_valid_utf8(const std::string &in) {
  std::string out;
  out.reserve(in.size());

  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = (unsigned char)in[i];
    size_t len = c < 0x80           ? 1
                 : (c >> 5) == 0x6  ? 2
                 : (c >> 4) == 0xE  ? 3
                 : (c >> 3) == 0x1E ? 4
                                    : 0;

    // Replaced rather than spelled C0 80: no reader here wants to see that.
    if (c == 0 || len == 0 || i + len > in.size()) {
      out += '?';
      i++;
      continue;
    }

    static const uint32_t LEAD_MASK[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    uint32_t cp = c & LEAD_MASK[len];
    bool ok = true;
    for (size_t k = 1; k < len; k++) {
      unsigned char cont = (unsigned char)in[i + k];
      if ((cont >> 6) != 0x2) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }

    static const uint32_t MIN_CP[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (!ok || cp < MIN_CP[len] || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      out += '?';
      i++;
      continue;
    }

    if (len < 4) {
      out.append(in, i, len);
    } else {
      uint32_t v = cp - 0x10000;
      uint32_t hi = 0xD800 + (v >> 10);
      uint32_t lo = 0xDC00 + (v & 0x3FF);
      for (uint32_t half : {hi, lo}) {
        out += (char)(0xE0 | (half >> 12));
        out += (char)(0x80 | ((half >> 6) & 0x3F));
        out += (char)(0x80 | (half & 0x3F));
      }
    }
    i += len;
  }
  return out;
}

// Names only, so cost doesn't follow value size.
inline std::vector<std::string> exif_list_tags(const uint8_t *data,
                                               size_t size) {
  // Once each: a tag in two IFDs is still only reachable by one name.
  std::vector<std::string> names;
  walk_exif(data, size,
            [&names](const char *name, const TiffBytes &, const uint8_t *) {
              if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
              }
              return true;
            });
  return names;
}

inline int exif_orientation(const uint8_t *data, size_t size) {
  std::string value;
  bool found = false;

  walk_exif(data, size,
            [&](const char *name, const TiffBytes &b, const uint8_t *entry) {
              if (strcmp(name, "Orientation") != 0) {
                return true;
              }
              found = format_entry(b, entry, &value);
              return false;
            });

  if (!found) {
    return 1;
  }

  int n = atoi(value.c_str());
  return n >= 1 && n <= 8 ? n : 1;
}

inline bool exif_get_tag(const uint8_t *data, size_t size, const char *wanted,
                         std::string *out) {
  std::string value;
  bool found = false;

  walk_exif(data, size,
            [&](const char *name, const TiffBytes &b, const uint8_t *entry) {
              if (strcmp(name, wanted) != 0) {
                return true;
              }
              found = format_entry(b, entry, &value);
              return false;
            });

  if (found) {
    *out = make_valid_utf8(value);
  }
  return found;
}

} // namespace imagedecoder
