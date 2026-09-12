#pragma once

/* Exif and TIFF tag extraction. One walker for every format: a TIFF file is the structure, and
 * every other loader puts the same one in "exif-data".
 *
 * Tags come from the IFD bytes; libtiff supplies only names. Its reader is no use here - it rejects
 * an exif IFD0 for having no StripOffsets, its tag list omits every standard tag, and its
 * custom-directory path drops entries whose on-disk type disagrees with its table.
 *
 * Offsets and counts are the file's, so bounds are written as [a > size - b], never [a + b > size]:
 * BigTIFF's 64-bit values wrap. */

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include <glib.h>
#include <jni.h>
#include <tiffio.h>
#include <vips/vips8>

/* IFD0's tag names; internal to libtiff, which is linked statically. */
extern "C" const TIFFFieldArray*
_TIFFGetFields(void);

/* Caps on what a hostile file can spend; none is reachable by a real image. */
static const uint64_t MAX_IFD_ENTRIES = 4096;
static const uint64_t MAX_COMPONENTS = 64;
static const size_t MAX_VALUE_BYTES = 4096;
static const tmsize_t MAX_TIFF_ALLOC = 16 * 1024 * 1024;

/* Read-only, but libtiff demands the write and close callbacks. */
struct TiffMemFile
{
  const uint8_t* data;
  toff_t size;
  toff_t offset;
};

static tsize_t
tiff_mem_read(thandle_t h, tdata_t buf, tsize_t size)
{
  TiffMemFile* f = (TiffMemFile*)h;
  if (size <= 0)
    return 0;
  toff_t avail = f->offset < f->size ? f->size - f->offset : 0;
  tsize_t n = (tsize_t)VIPS_MIN((toff_t)size, avail);
  memcpy(buf, f->data + f->offset, (size_t)n);
  f->offset += n;
  return n;
}

static tsize_t
tiff_mem_write(thandle_t, tdata_t, tsize_t)
{
  return -1;
}

static toff_t
tiff_mem_seek(thandle_t h, toff_t off, int whence)
{
  TiffMemFile* f = (TiffMemFile*)h;
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

static int
tiff_mem_close(thandle_t)
{
  return 0;
}

static toff_t
tiff_mem_size(thandle_t h)
{
  return ((TiffMemFile*)h)->size;
}

/* Per-handle, so a broken IFD is skipped silently rather than logged. */
static int
tiff_silence(TIFF*, void*, const char*, const char*, va_list)
{
  return 1;
}

struct TiffBytes
{
  const uint8_t* data;
  size_t size;
  bool le;
  bool big;
};

static uint64_t
read_uint(const uint8_t* d, size_t n, bool le)
{
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++)
    v |= (uint64_t)d[le ? i : n - 1 - i] << (8 * i);
  return v;
}

/* 0 if unrecognised. */
static size_t
tiff_type_size(uint64_t type)
{
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

static void
append_element(std::string& out, const uint8_t* p, uint64_t type, bool le)
{
  char buf[64];

  switch (type) {
    case TIFF_SBYTE:
      g_snprintf(buf, sizeof(buf), "%d", (int)(int8_t)p[0]);
      break;
    case TIFF_SSHORT:
      g_snprintf(buf, sizeof(buf), "%d", (int)(int16_t)read_uint(p, 2, le));
      break;
    case TIFF_SLONG:
      g_snprintf(buf, sizeof(buf), "%d", (int)(int32_t)read_uint(p, 4, le));
      break;
    case TIFF_SLONG8:
      g_snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)read_uint(p, 8, le));
      break;
    case TIFF_FLOAT: {
      const uint32_t bits = (uint32_t)read_uint(p, 4, le);
      float f;
      memcpy(&f, &bits, sizeof(f));
      g_snprintf(buf, sizeof(buf), "%.7g", (double)f);
      break;
    }
    case TIFF_DOUBLE: {
      const uint64_t bits = read_uint(p, 8, le);
      double d;
      memcpy(&d, &bits, sizeof(d));
      g_snprintf(buf, sizeof(buf), "%.15g", d);
      break;
    }
    case TIFF_RATIONAL:
    case TIFF_SRATIONAL: {
      const bool sign = type == TIFF_SRATIONAL;
      const double num = sign ? (double)(int32_t)read_uint(p, 4, le) : (double)read_uint(p, 4, le);
      const double den =
        sign ? (double)(int32_t)read_uint(p + 4, 4, le) : (double)read_uint(p + 4, 4, le);
      g_snprintf(buf, sizeof(buf), "%.10g", den == 0 ? 0.0 : num / den);
      break;
    }
    default:
      /* BYTE, SHORT, LONG, LONG8 and the IFD types are unsigned. */
      g_snprintf(buf, sizeof(buf), "%llu",
                 (unsigned long long)read_uint(p, tiff_type_size(type), le));
      break;
  }

  out += buf;
}

/* An UNDEFINED field is either text (ExifVersion's "0230") or binary (a maker note). */
static bool
looks_like_text(const uint8_t* p, size_t len)
{
  for (size_t i = 0; i < len; i++)
    if (p[i] != 0 && p[i] != '\t' && (p[i] < 0x20 || p[i] > 0x7e))
      return false;
  return len > 0;
}

/* The printed value of one IFD entry, false if the file cannot back what the entry claims. */
static bool
format_entry(const TiffBytes& b, const uint8_t* entry, std::string* out)
{
  const uint64_t type = read_uint(entry + 2, 2, b.le);
  const uint64_t count = read_uint(entry + 4, b.big ? 8 : 4, b.le);
  const size_t elem = tiff_type_size(type);
  if (elem == 0 || count == 0 || count > b.size / elem)
    return false;

  /* A value that fits the entry's value field is inline; anything larger is at an offset. */
  const uint64_t total = count * elem;
  const size_t inline_size = b.big ? 8 : 4;
  const uint8_t* value = entry + (b.big ? 12 : 8);
  if (total > inline_size) {
    const uint64_t offset = read_uint(value, inline_size, b.le);
    if (offset < 8 || offset > b.size - total)
      return false;
    value = b.data + offset;
  }

  out->clear();

  if (type == TIFF_ASCII || (type == TIFF_UNDEFINED && looks_like_text(value, (size_t)total))) {
    size_t len = strnlen((const char*)value, VIPS_MIN((size_t)total, MAX_VALUE_BYTES));
    /* Trailing NUL and space padding is endemic here. */
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\0'))
      len--;
    out->assign((const char*)value, len);
    return !out->empty();
  }

  /* A colour map runs to hundreds of components; print a bounded prefix. */
  const uint64_t shown = VIPS_MIN(count, MAX_COMPONENTS);
  for (uint64_t i = 0; i < shown; i++) {
    if (i)
      out->append(", ");
    append_element(*out, value + i * elem, type, b.le);
  }
  if (shown < count)
    out->append(", ...");

  return true;
}

/* Each tag of the IFD at [offset], as (name, entry); false once [visit] asks to stop. [tiff] has
 * just read this directory, so its field table names these tags, anonymously for ones it has never
 * seen. Sub-IFD pointers are structure, not metadata: returned to follow, not visited. */
template<typename Visit>
static bool
visit_ifd(TIFF* tiff, const TiffBytes& b, uint64_t offset, Visit& visit,
          uint64_t* exif_ifd = nullptr, uint64_t* gps_ifd = nullptr)
{
  const size_t count_size = b.big ? 8 : 2;
  const size_t entry_size = b.big ? 20 : 12;
  if (offset < 8 || b.size < count_size || offset > b.size - count_size)
    return true;

  uint64_t count = read_uint(b.data + offset, count_size, b.le);
  if (count > (b.size - offset - count_size) / entry_size)
    return true;
  count = VIPS_MIN(count, MAX_IFD_ENTRIES);

  for (uint64_t i = 0; i < count; i++) {
    const uint8_t* entry = b.data + offset + count_size + i * entry_size;
    const uint32_t tag = (uint32_t)read_uint(entry, 2, b.le);

    if (tag == TIFFTAG_EXIFIFD || tag == TIFFTAG_GPSIFD) {
      uint64_t* out = tag == TIFFTAG_EXIFIFD ? exif_ifd : gps_ifd;
      if (out)
        *out = read_uint(entry + (b.big ? 12 : 8), b.big ? 8 : 4, b.le);
      continue;
    }

    const TIFFField* fip = TIFFFieldWithTag(tiff, tag);
    const char* name = fip ? TIFFFieldName(fip) : nullptr;
    if (name && *name && !visit(name, b, entry))
      return false;
  }

  return true;
}

/* Every tag of the main image: IFD0 and the Exif and GPS sub-IFDs it points at. A thumbnail IFD
 * and a tiff's later pages describe other images, so they are left alone. [data, size] is raw TIFF
 * bytes - a TIFF file's, or an "exif-data" blob. */
template<typename Visit>
static void
walk_exif(const uint8_t* data, size_t size, Visit visit)
{
  if (!data)
    return;
  if (size > 6 && memcmp(data, "Exif\0\0", 6) == 0) {
    data += 6;
    size -= 6;
  }
  /* A BigTIFF header, the longest this reads before checking anything. */
  if (size < 16)
    return;

  const bool le = data[0] == 'I' && data[1] == 'I';
  if (!le && !(data[0] == 'M' && data[1] == 'M'))
    return;

  /* Parsed here: reading IFD0 needs the handle opened without it. */
  const uint64_t magic = read_uint(data + 2, 2, le);
  uint64_t ifd0;
  if (magic == 42)
    ifd0 = read_uint(data + 4, 4, le);
  else if (magic == 43 && read_uint(data + 4, 2, le) == 8)
    ifd0 = read_uint(data + 8, 8, le);
  else
    return;

  const TiffBytes b = { data, size, le, magic == 43 };

  TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
  if (!opts)
    return;
  TIFFOpenOptionsSetErrorHandlerExtR(opts, tiff_silence, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(opts, tiff_silence, nullptr);
  TIFFOpenOptionsSetMaxSingleMemAlloc(opts, MAX_TIFF_ALLOC);

  /* "h" reads the header only; loading IFD0 as an image directory is what rejects it. */
  TiffMemFile mem = { data, (toff_t)size, 0 };
  TIFF* tiff =
    TIFFClientOpenExt("memory", "rhm", &mem, tiff_mem_read, tiff_mem_write, tiff_mem_seek,
                      tiff_mem_close, tiff_mem_size, nullptr, nullptr, opts);
  TIFFOpenOptionsFree(opts);
  if (!tiff)
    return;

  /* Closed however [visit] leaves, a throw included. */
  struct TiffGuard
  {
    TIFF* t;
    ~TiffGuard() { TIFFClose(t); }
  } guard = { tiff };

  /* Each read installs the field table naming that directory's tags. */
  if (!TIFFReadCustomDirectory(tiff, (toff_t)ifd0, _TIFFGetFields()))
    return;

  uint64_t exif_ifd = 0;
  uint64_t gps_ifd = 0;
  if (!visit_ifd(tiff, b, ifd0, visit, &exif_ifd, &gps_ifd))
    return;

  if (exif_ifd && TIFFReadEXIFDirectory(tiff, (toff_t)exif_ifd) &&
      !visit_ifd(tiff, b, exif_ifd, visit))
    return;

  if (gps_ifd && TIFFReadGPSDirectory(tiff, (toff_t)gps_ifd))
    visit_ifd(tiff, b, gps_ifd, visit);
}

/* Points [out, out_size] at the image's raw TIFF bytes: its own, for a TIFF, which libvips gives
 * no "exif-data"; the blob otherwise. The blob dies with the VImage so it is copied, while a TIFF
 * is referenced in place however large. Returns the copy to free, or null if none was made. */
static uint8_t*
exif_bytes(vips::VImage& image, const uint8_t* file_data, size_t file_size, const uint8_t** out,
           size_t* out_size)
{
  *out = nullptr;
  *out_size = 0;

  if (!file_data || file_size == 0)
    return nullptr;

  if (file_size >= 4 &&
      (memcmp(file_data, "II\x2a\0", 4) == 0 || memcmp(file_data, "MM\0\x2a", 4) == 0 ||
       memcmp(file_data, "II\x2b\0", 4) == 0 || memcmp(file_data, "MM\0\x2b", 4) == 0)) {
    *out = file_data;
    *out_size = file_size;
    return nullptr;
  }

  if (image.get_typeof(VIPS_META_EXIF_NAME) == 0)
    return nullptr;

  const void* blob = nullptr;
  size_t blob_size = 0;
  try {
    blob = image.get_blob(VIPS_META_EXIF_NAME, &blob_size);
  } catch (const vips::VError&) {
    vips_error_clear();
    return nullptr;
  }
  if (!blob || blob_size == 0)
    return nullptr;

  /* Metadata is optional: a failed copy costs the tags, not the decode. */
  uint8_t* copy = (uint8_t*)g_try_malloc(blob_size);
  if (!copy)
    return nullptr;

  memcpy(copy, blob, blob_size);
  *out = copy;
  *out_size = blob_size;
  return copy;
}

/* Names only, so cost does not follow value size. Null with a pending exception if the VM refused
 * the array or a string. */
static jobjectArray
exif_list_tags(JNIEnv* env, const uint8_t* data, size_t size)
{
  std::vector<std::string> names;
  walk_exif(data, size, [&names](const char* name, const TiffBytes&, const uint8_t*) {
    names.push_back(name);
    return true;
  });

  jclass string_cls = env->FindClass("java/lang/String");
  if (!string_cls)
    return nullptr;

  jobjectArray array = env->NewObjectArray((jsize)names.size(), string_cls, nullptr);
  env->DeleteLocalRef(string_cls);
  if (!array)
    return nullptr;

  for (jsize i = 0; i < (jsize)names.size(); i++) {
    jstring jname = env->NewStringUTF(names[(size_t)i].c_str());
    if (!jname) {
      env->DeleteLocalRef(array);
      return nullptr;
    }
    env->SetObjectArrayElement(array, i, jname);
    env->DeleteLocalRef(jname);
  }

  return array;
}

/* Null if the image does not carry [wanted], or its value will not parse. */
static jstring
exif_get_tag(JNIEnv* env, const uint8_t* data, size_t size, const char* wanted)
{
  std::string value;
  bool found = false;

  walk_exif(data, size, [&](const char* name, const TiffBytes& b, const uint8_t* entry) {
    if (strcmp(name, wanted) != 0)
      return true;
    found = format_entry(b, entry, &value);
    return false;
  });

  if (!found)
    return nullptr;

  /* Exif text carries Latin-1 and junk; NewStringUTF on invalid UTF-8 is fatal on some VMs. */
  char* valid = g_utf8_make_valid(value.c_str(), (gssize)value.size());
  jstring result = env->NewStringUTF(valid);
  g_free(valid);
  return result;
}
