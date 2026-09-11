#include <limits.h>
#include <stdint.h>
#include <string>
#include <vector>

#include <glib.h>
#include <jni.h>
#include <vips/vips8>

#include "hdr.h"

using namespace vips;

/* ------------------------------------------------------------------ limits
 *
 * Each one turns an abort - glib's checked allocators die on failure - or an allocation sized
 * from an attacker-controlled header field into a DecodeException. */

/* Buffered whole, so the stream length is the first lever a hostile file has on the heap. */
static const size_t MAX_INPUT_BYTES = (size_t)512 * 1024 * 1024;

/* 268M pixels: past any real photograph, and pixels * 8 still fits a signed 32-bit size. */
static const guint64 MAX_PIXELS = (guint64)1 << 28;

/* A direct ByteBuffer is addressed by a jint, so this is a hard ceiling rather than a policy. */
static const guint64 MAX_OUTPUT_BYTES = (guint64)G_MAXINT;

/* A malformed n-pages otherwise multiplies straight into an allocation. */
static const int MAX_PAGES = 100000;

static const char* const EXC_DECODE = "ca/mpreg/imagedecoder/ImageDecoder$DecodeException";
static const char* const EXC_UNKNOWN_FORMAT =
  "ca/mpreg/imagedecoder/ImageDecoder$UnknownFormatException";
static const char* const EXC_OOM = "ca/mpreg/imagedecoder/ImageDecoder$OutOfMemoryException";

/* ------------------------------------------------------------------- errors */

/* Leaves exactly one pending exception, so callers can return unconditionally afterwards. */
static void
throw_decode_error(JNIEnv* env, const char* cls_name, const char* msg)
{
  if (env->ExceptionCheck())
    env->ExceptionClear();

  jclass cls = env->FindClass(cls_name);
  if (!cls) {
    /* FindClass left NoClassDefFoundError pending; that is the throw. */
    return;
  }
  env->ThrowNew(cls, msg && *msg ? msg : "Image decode failed");
  env->DeleteLocalRef(cls);
}

static void
throw_vips_error(JNIEnv* env, const vips::VError& e)
{
  std::string msg;
  try {
    const char* what = e.what();
    if (what)
      msg = what;
  } catch (...) {
    /* what() builds a std::string; a failure there must not escape. */
  }
  vips_error_clear();

  if (msg.empty())
    msg = "Image decode failed";

  const char* cls =
    msg.find("not in a known format") != std::string::npos ? EXC_UNKNOWN_FORMAT : EXC_DECODE;
  throw_decode_error(env, cls, msg.c_str());
}

/* Funnels every failure into the entry point's catch chain, which frees what the call owns. */
struct DecodeError
{
  const char* cls;
  std::string msg;

  DecodeError(const char* cls_, std::string msg_)
    : cls(cls_)
    , msg(std::move(msg_))
  {
  }
};

static void
fail(const char* cls, const std::string& msg)
{
  throw DecodeError(cls, msg);
}

static void
fail_oom(const std::string& what, guint64 bytes)
{
  char buf[64];
  g_snprintf(buf, sizeof(buf), " (%" G_GUINT64_FORMAT " bytes)", bytes);
  throw DecodeError(EXC_OOM, what + buf);
}

/* --------------------------------------------------------------- JNI lookup
 *
 * Both return null with an exception pending, and feeding that null back into JNI aborts the VM
 * rather than throwing. */

static jclass
find_class_checked(JNIEnv* env, const char* name)
{
  jclass cls = env->FindClass(name);
  if (!cls || env->ExceptionCheck()) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    fail(EXC_DECODE, std::string("Class not found: ") + name);
  }
  return cls;
}

static jmethodID
get_method_checked(JNIEnv* env, jclass cls, const char* name, const char* sig)
{
  jmethodID id = env->GetMethodID(cls, name, sig);
  if (!id || env->ExceptionCheck()) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    fail(EXC_DECODE, std::string("Method not found: ") + name);
  }
  return id;
}

/* The VM reclaims local refs only when the native frame returns, and a decode takes a dozen. */
struct LocalRef
{
  JNIEnv* env;
  jobject ref;

  LocalRef(JNIEnv* e, jobject r)
    : env(e)
    , ref(r)
  {
  }
  ~LocalRef()
  {
    if (ref)
      env->DeleteLocalRef(ref);
  }
  LocalRef(const LocalRef&) = delete;
  LocalRef& operator=(const LocalRef&) = delete;
};

/* vips_region_prepare fails deep inside the pipeline, and that throw used to skip the unref. */
struct RegionRef
{
  VipsRegion* region;

  explicit RegionRef(VipsRegion* r)
    : region(r)
  {
  }
  ~RegionRef()
  {
    if (region)
      g_object_unref(region);
  }
  RegionRef(const RegionRef&) = delete;
  RegionRef& operator=(const RegionRef&) = delete;
};

/* ---------------------------------------------------------------- lifecycle */

jint
JNI_OnLoad(JavaVM* vm, void*)
{
  JNIEnv* env;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;

  /* Ignoring this left every later call on an uninitialised type system. */
  if (VIPS_INIT("VipsDecoder")) {
    vips_error_clear();
    return JNI_ERR;
  }

  vips_concurrency_set(1);

  /* One-shot pipelines never hit the operation cache; it only pins memory. */
  vips_cache_set_max(0);
  vips_cache_set_max_mem(0);
  vips_cache_set_max_files(0);

  return JNI_VERSION_1_6;
}

static jfieldID
ptr_field_id(JNIEnv* env, jobject obj)
{
  jclass cls = env->GetObjectClass(obj);
  if (!cls)
    return nullptr;
  jfieldID id = env->GetFieldID(cls, "ptr", "J");
  env->DeleteLocalRef(cls);
  if (!id && env->ExceptionCheck())
    env->ExceptionClear();
  return id;
}

static jlong
get_ptr(JNIEnv* env, jobject obj)
{
  jfieldID id = ptr_field_id(env, obj);
  return id ? env->GetLongField(obj, id) : 0;
}

/* Read and zero in one step, so a second free is a no-op instead of a double free. */
static jlong
take_ptr(JNIEnv* env, jobject obj)
{
  jfieldID id = ptr_field_id(env, obj);
  if (!id)
    return 0;
  jlong ptr = env->GetLongField(obj, id);
  env->SetLongField(obj, id, 0L);
  return ptr;
}

struct Decoder
{
  uint8_t* buffer;
  size_t buffer_size;
  int pages;
  int* durations;
  int durations_count;

  /* Decided once at header read: every page of an animation shares the container's signal. */
  int hdr_kind;

  /* Stops above SDR white the source reaches; understating it clips every highlight. */
  float hdr_headroom;

  /* For the PQ/HLG paths, straight from the container. */
  ColourSignal signal;
};

static Decoder*
decoder_new()
{
  return g_try_new0(Decoder, 1);
}

static void
decoder_free(Decoder* d)
{
  if (!d)
    return;
  g_free(d->buffer);
  g_free(d->durations);
  g_free(d);
}

/* Null once free has run, so a call after close reports instead of dereferencing it. */
static Decoder*
decoder_for(JNIEnv* env, jobject obj)
{
  Decoder* d = reinterpret_cast<Decoder*>(get_ptr(env, obj));
  if (!d)
    fail(EXC_DECODE, "ImageDecoder has been closed");
  if (!d->buffer || d->buffer_size == 0)
    fail(EXC_DECODE, "ImageDecoder holds no image data");
  return d;
}

/* ---------------------------------------------------------------- stream in
 *
 * g_try_realloc rather than GByteArray, whose g_realloc aborts the process on failure. */
static uint8_t*
read_all(JNIEnv* env, jobject jstream, size_t* out_size, std::string* error)
{
  *out_size = 0;

  jclass cls = env->GetObjectClass(jstream);
  if (!cls) {
    *error = "Cannot resolve the InputStream class";
    return nullptr;
  }
  jmethodID read_method = env->GetMethodID(cls, "read", "([B)I");
  env->DeleteLocalRef(cls);
  if (!read_method) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    *error = "InputStream.read([B) not found";
    return nullptr;
  }

  const jint chunk = 64 * 1024;
  jbyteArray jbuf = env->NewByteArray(chunk);
  if (!jbuf || env->ExceptionCheck()) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    *error = "Out of memory allocating the read buffer";
    return nullptr;
  }
  LocalRef buf_ref(env, jbuf);

  uint8_t* data = nullptr;
  size_t len = 0;
  size_t cap = 0;

  while (true) {
    jint n = env->CallIntMethod(jstream, read_method, jbuf);

    /* Swallowing this handed the loader a truncated file to report as a corrupt one. */
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_free(data);
      *error = "InputStream.read failed";
      return nullptr;
    }

    if (n <= 0)
      break;

    /* A stream lying about how much it wrote would otherwise drive an overread. */
    if (n > chunk) {
      g_free(data);
      *error = "InputStream.read returned more than the buffer length";
      return nullptr;
    }

    if ((guint64)len + (guint64)n > (guint64)MAX_INPUT_BYTES) {
      g_free(data);
      *error = "Out of memory: image exceeds the maximum input size";
      return nullptr;
    }

    if (len + (size_t)n > cap) {
      size_t want = cap ? cap * 2 : (size_t)chunk * 4;
      while (want < len + (size_t)n && want < MAX_INPUT_BYTES)
        want *= 2;
      if (want > MAX_INPUT_BYTES)
        want = MAX_INPUT_BYTES;

      uint8_t* grown = (uint8_t*)g_try_realloc(data, want);
      if (!grown) {
        g_free(data);
        *error = "Out of memory buffering the image";
        return nullptr;
      }
      data = grown;
      cap = want;
    }

    jbyte* bytes = env->GetByteArrayElements(jbuf, nullptr);
    if (!bytes) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      g_free(data);
      *error = "Out of memory reading the image";
      return nullptr;
    }
    memcpy(data + len, bytes, (size_t)n);
    env->ReleaseByteArrayElements(jbuf, bytes, JNI_ABORT);
    len += (size_t)n;
  }

  *out_size = len;
  return data;
}

/* By metadata, not vips_image_get_gainmap, which would decompress the map's JPEG. */
static bool
has_gainmap(vips::VImage& image)
{
  return image.get_typeof("gainmap") != 0 || image.get_typeof("gainmap-data") != 0;
}

/* Zero if the metadata is missing, which makes the image ordinary SDR to any caller. */
static float
gainmap_headroom(vips::VImage& image)
{
  if (image.get_typeof("gainmap-max-content-boost") == 0)
    return 0.0f;

  double* boost = nullptr;
  int n = 0;
  try {
    image.get_array_double("gainmap-max-content-boost", &boost, &n);
  } catch (const vips::VError&) {
    vips_error_clear();
    return 0.0f;
  }

  if (!boost || n <= 0)
    return 0.0f;

  double max_boost = 1.0;
  for (int i = 0; i < n; i++) {
    /* The caller sizes its display pipeline from this, and every compare against NaN is false. */
    if (std::isfinite(boost[i]))
      max_boost = VIPS_MAX(max_boost, boost[i]);
  }

  float stops = (float)log2(max_boost);
  return std::isfinite(stops) && stops > 0.0f ? stops : 0.0f;
}

/* A gainmap wins over a transfer function: an UltraHDR file is SDR base plus map. */
static void
detect_hdr(Decoder* decoder, vips::VImage& image)
{
  decoder->hdr_kind = HDR_NONE;
  decoder->hdr_headroom = 0.0f;
  decoder->signal.primaries = CICP_PRIMARIES_BT709;
  decoder->signal.transfer = CICP_TRANSFER_SRGB;

  if (has_gainmap(image)) {
    float headroom = gainmap_headroom(image);
    /* A map that cannot brighten is not worth a float buffer. */
    if (headroom > 0.0f) {
      decoder->hdr_kind = HDR_GAINMAP;
      decoder->hdr_headroom = headroom;
      return;
    }
  }

  /* Linear integer samples have no room above white and are just SDR. */
  const bool float_samples =
    image.format() == VIPS_FORMAT_FLOAT || image.format() == VIPS_FORMAT_DOUBLE;

  ColourSignal signal;
  float peak_nits = 0.0f;
  if (hdr_find_colour_signal(decoder->buffer, decoder->buffer_size, &signal, &peak_nits)) {
    int kind = hdr_kind_for_transfer(signal.transfer);

    /* Only JXL declares a linear transfer, and always with float samples. */
    if (kind == HDR_NONE && signal.transfer == CICP_TRANSFER_LINEAR && float_samples)
      kind = HDR_LINEAR;

    if (kind != HDR_NONE) {
      decoder->hdr_kind = kind;
      decoder->signal = signal;

      /* A container is free to declare a garbage peak. */
      if (!std::isfinite(peak_nits) || peak_nits <= 0.0f)
        peak_nits = 0.0f;

      /* A declared peak beats the nominal one; used only to tone map when HDR can't present. */
      float headroom = peak_nits > HDR_SDR_WHITE_NITS ? (float)log2(peak_nits / HDR_SDR_WHITE_NITS)
                       : kind == HDR_PQ ? (float)log2(10000.0 / HDR_SDR_WHITE_NITS)
                                        : (float)log2(HDR_HLG_PEAK_NITS / HDR_SDR_WHITE_NITS);
      decoder->hdr_headroom = std::isfinite(headroom) && headroom > 0.0f ? headroom : 0.0f;
      return;
    }
  }

  /* A loader that already handed us linear scRGB - libvips does this for a few
   * genuinely floating-point formats. No transfer function to undo. */
  if (image.interpretation() == VIPS_INTERPRETATION_scRGB) {
    decoder->hdr_kind = HDR_LINEAR;
    /* Nothing declares a peak, so claim the range half-float can carry. */
    decoder->hdr_headroom = 4.0f;
  }
}

/* Header fields, so attacker-controlled: checked before anything sizes a buffer from them. */
static void
check_dimensions(int width, int height)
{
  if (width <= 0 || height <= 0)
    fail(EXC_DECODE, "Image has no pixels");

  if ((guint64)width * (guint64)height > MAX_PIXELS)
    fail(EXC_OOM, "Image exceeds the maximum supported pixel count");
}

/* Multiplied wide: size_t is 32 bits on the 32-bit ABIs, where the product wraps to a small
 * allocation the write then overruns. */
static size_t
checked_buffer_size(guint64 pixels, guint64 bytes_per_pixel)
{
  if (pixels == 0 || bytes_per_pixel == 0)
    fail(EXC_DECODE, "Image has no pixels");

  guint64 total = pixels * bytes_per_pixel;
  if (total / pixels != bytes_per_pixel)
    fail_oom("Pixel buffer size overflows", MAX_OUTPUT_BYTES);
  if (total > MAX_OUTPUT_BYTES || total > (guint64)SIZE_MAX)
    fail_oom("Pixel buffer exceeds the maximum direct buffer size", total);

  return (size_t)total;
}

/* Converts the VM's OutOfMemoryError: the caller can handle a DecodeException, where an Error
 * unwinds past every handler it has. */
static jobject
allocate_direct(JNIEnv* env, size_t size, void** out_data)
{
  if ((guint64)size > MAX_OUTPUT_BYTES)
    fail_oom("Buffer exceeds the maximum direct buffer size", (guint64)size);

  jclass cls = find_class_checked(env, "java/nio/ByteBuffer");
  LocalRef cls_ref(env, cls);

  jmethodID allocate = env->GetStaticMethodID(cls, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
  if (!allocate || env->ExceptionCheck()) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    fail(EXC_DECODE, "ByteBuffer.allocateDirect not found");
  }

  jobject buffer = env->CallStaticObjectMethod(cls, allocate, (jint)size);
  if (env->ExceptionCheck() || !buffer) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    if (buffer)
      env->DeleteLocalRef(buffer);
    fail_oom("Out of memory allocating the pixel buffer", (guint64)size);
  }

  void* data = env->GetDirectBufferAddress(buffer);
  if (!data) {
    env->DeleteLocalRef(buffer);
    if (env->ExceptionCheck())
      env->ExceptionClear();
    fail(EXC_DECODE, "Failed to map the direct byte buffer");
  }

  *out_data = data;
  return buffer;
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeNew(JNIEnv* env, jclass, jobject jstream)
{
  if (!jstream) {
    throw_decode_error(env, EXC_DECODE, "Input stream is null");
    return nullptr;
  }

  Decoder* decoder = decoder_new();
  if (!decoder) {
    throw_decode_error(env, EXC_OOM, "Out of memory creating the decoder");
    return nullptr;
  }

  std::string read_error;
  decoder->buffer = read_all(env, jstream, &decoder->buffer_size, &read_error);

  if (!decoder->buffer || decoder->buffer_size == 0) {
    decoder_free(decoder);
    const bool oom = read_error.compare(0, 13, "Out of memory") == 0;
    throw_decode_error(env, oom ? EXC_OOM : EXC_DECODE,
                       read_error.empty() ? "Empty or unreadable image stream"
                                          : read_error.c_str());
    return nullptr;
  }

  try {
    vips::VImage image = vips::VImage::new_from_buffer(decoder->buffer, decoder->buffer_size, "");

    check_dimensions(image.width(), image.height());

    int pages = image.get_typeof(VIPS_META_N_PAGES) != 0 ? image.get_int(VIPS_META_N_PAGES) : 1;
    /* A corrupt n-pages of 0 divided by zero in decodeNext; a negative one indexed backwards. */
    decoder->pages = CLAMP(pages, 1, MAX_PAGES);

    if (image.get_typeof("delay") != 0) {
      int* delays = nullptr;
      int n = 0;

      image.get_array_int("delay", &delays, &n);
      if (delays && n > 0) {
        n = VIPS_MIN(n, MAX_PAGES);
        decoder->durations = g_try_new(int, n);
        if (!decoder->durations)
          fail_oom("Out of memory reading the frame durations", (guint64)n * sizeof(int));
        decoder->durations_count = n;
        memcpy(decoder->durations, delays, (size_t)n * sizeof(int));
      }
    }

    detect_hdr(decoder, image);
    const bool is_hdr = decoder->hdr_kind != HDR_NONE;

    const char* loader_cstr =
      image.get_typeof("vips-loader") != 0 ? image.get_string("vips-loader") : "";
    std::string loader_str = loader_cstr ? loader_cstr : "";

    image = vips::VImage();

    jstring jloader = env->NewStringUTF(loader_str.c_str());
    if (!jloader || env->ExceptionCheck()) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      fail(EXC_OOM, "Out of memory creating the loader name");
    }
    LocalRef loader_ref(env, jloader);

    jclass cls = find_class_checked(env, "ca/mpreg/imagedecoder/ImageDecoder");
    LocalRef cls_ref(env, cls);
    jmethodID ctor = get_method_checked(env, cls, "<init>", "(JIIZIFLjava/lang/String;)V");

    jobject result =
      env->NewObject(cls, ctor, reinterpret_cast<jlong>(decoder), decoder->pages, 0,
                     (jboolean)is_hdr, decoder->hdr_kind, decoder->hdr_headroom, jloader);
    if (!result || env->ExceptionCheck()) {
      /* Ownership never reached Kotlin, so nothing else will ever free this. */
      if (env->ExceptionCheck())
        env->ExceptionClear();
      if (result)
        env->DeleteLocalRef(result);
      fail(EXC_OOM, "Out of memory creating the decoder object");
    }

    return result;
  } catch (const DecodeError& e) {
    decoder_free(decoder);
    throw_decode_error(env, e.cls, e.msg.c_str());
    return nullptr;
  } catch (const vips::VError& e) {
    decoder_free(decoder);
    throw_vips_error(env, e);
    return nullptr;
  } catch (const std::bad_alloc&) {
    decoder_free(decoder);
    vips_error_clear();
    throw_decode_error(env, EXC_OOM, "Out of memory reading the image header");
    return nullptr;
  } catch (...) {
    decoder_free(decoder);
    vips_error_clear();
    throw_decode_error(env, EXC_DECODE, "Unknown error reading the image header");
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeFree(JNIEnv* env, jobject obj)
{
  /* take, not get: leaving the pointer in place let the finalizer free the block a second
   * time, and a decode after close read it. */
  decoder_free(reinterpret_cast<Decoder*>(take_ptr(env, obj)));
}

/* The value a full-scale sample holds in [frame]'s band format. */
static double
sample_max(vips::VImage& frame)
{
  switch (frame.format()) {
    case VIPS_FORMAT_UCHAR:
      return 255.0;
    case VIPS_FORMAT_USHORT:
      return 65535.0;
    default:
      /* Float and friends are normalised already. */
      return 1.0;
  }
}

/* Three entries always, even for a mono map, which simply repeats. Missing metadata gives the
 * identity, so a malformed file degrades to the base image rather than to a wrong one. */
static jfloatArray
gainmap_metadata_array(JNIEnv* env, vips::VImage& image, const char* field, float identity)
{
  float values[3] = { identity, identity, identity };

  if (image.get_typeof(field) != 0) {
    try {
      double* d = nullptr;
      int n = 0;
      image.get_array_double(field, &d, &n);
      /* An empty array indexed d[-1]. */
      if (d && n > 0) {
        for (int i = 0; i < 3; i++) {
          double v = d[VIPS_MIN(i, n - 1)];
          if (std::isfinite(v))
            values[i] = (float)v;
        }
      }
    } catch (const vips::VError&) {
      vips_error_clear();
    }
  }

  jfloatArray arr = env->NewFloatArray(3);
  if (!arr || env->ExceptionCheck()) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    return nullptr;
  }
  env->SetFloatArrayRegion(arr, 0, 3, values);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(arr);
    return nullptr;
  }
  return arr;
}

/* Handed over unapplied for the viewer to combine: applying the gain is a display decision, and
 * libvips's own uhdr2scRGB gets three-channel maps wrong anyway - it runs the map through the
 * sRGB EOTF where libultrahdr's applyGain uses the raw value.
 *
 * Null on any failure: a missing gainmap costs the caller its HDR, a failed decode the picture. */
static jobject
build_gainmap(JNIEnv* env, vips::VImage& image)
{
  VipsImage* raw = vips_image_get_gainmap(image.get_image());
  if (!raw) {
    vips_error_clear();
    return nullptr;
  }

  /* VImage takes the reference get_gainmap returned, down every throw path below too. */
  vips::VImage map(raw);

  jobject buffer = nullptr;
  jfloatArray gamma = nullptr;
  jfloatArray min_boost = nullptr;
  jfloatArray max_boost = nullptr;
  jfloatArray offset_sdr = nullptr;
  jfloatArray offset_hdr = nullptr;
  jobject result = nullptr;

  try {
    /* uchar, however the map's JPEG was coded. */
    if (map.format() != VIPS_FORMAT_UCHAR)
      map = map.cast(VIPS_FORMAT_UCHAR);

    /* One or three bands - anything else is not a gainmap we understand. */
    if (map.bands() == 2)
      map = map.extract_band(0);
    else if (map.bands() > 3)
      map = map.extract_band(0, vips::VImage::option()->set("n", 3));

    const int width = map.width();
    const int height = map.height();
    const int bands = map.bands();

    if (width <= 0 || height <= 0 || bands <= 0)
      fail(EXC_DECODE, "Gainmap has no pixels");
    if ((guint64)width * (guint64)height > MAX_PIXELS)
      fail(EXC_OOM, "Gainmap exceeds the maximum supported pixel count");

    const size_t size = checked_buffer_size((guint64)width * (guint64)height, (guint64)bands);

    void* data = nullptr;
    buffer = allocate_direct(env, size, &data);

    map.write(vips::VImage::new_from_memory(data, size, width, height, bands, VIPS_FORMAT_UCHAR));

    gamma = gainmap_metadata_array(env, image, "gainmap-gamma", 1.0f);
    min_boost = gainmap_metadata_array(env, image, "gainmap-min-content-boost", 1.0f);
    max_boost = gainmap_metadata_array(env, image, "gainmap-max-content-boost", 1.0f);
    offset_sdr = gainmap_metadata_array(env, image, "gainmap-offset-sdr", 0.0f);
    offset_hdr = gainmap_metadata_array(env, image, "gainmap-offset-hdr", 0.0f);

    /* The Kotlin constructor declares these non-null. */
    if (!gamma || !min_boost || !max_boost || !offset_sdr || !offset_hdr)
      fail(EXC_OOM, "Out of memory reading the gainmap metadata");

    jclass cls = find_class_checked(env, "ca/mpreg/imagedecoder/ImageDecoder$Gainmap");
    LocalRef cls_ref(env, cls);
    jmethodID ctor =
      get_method_checked(env, cls, "<init>", "(Ljava/nio/ByteBuffer;III[F[F[F[F[F)V");

    result = env->NewObject(cls, ctor, buffer, width, height, bands, gamma, min_boost, max_boost,
                            offset_sdr, offset_hdr);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      if (result)
        env->DeleteLocalRef(result);
      result = nullptr;
    }
  } catch (...) {
    vips_error_clear();
    if (env->ExceptionCheck())
      env->ExceptionClear();
    result = nullptr;
  }

  if (buffer)
    env->DeleteLocalRef(buffer);
  if (gamma)
    env->DeleteLocalRef(gamma);
  if (min_boost)
    env->DeleteLocalRef(min_boost);
  if (max_boost)
    env->DeleteLocalRef(max_boost);
  if (offset_sdr)
    env->DeleteLocalRef(offset_sdr);
  if (offset_hdr)
    env->DeleteLocalRef(offset_hdr);

  return result;
}

/* An UltraHDR JPEG's base is usually Display P3, and the embedded profile is the only place that
 * is written down. Treating those values as sRGB skews the whole SDR part of the picture.
 *
 * A file with no profile is left alone. PQ and HLG do not come through here - their primaries
 * are signalled in the container and handled by the matrix in the pixel loop. */
static vips::VImage
to_srgb_primaries(vips::VImage frame)
{
  if (frame.get_typeof(VIPS_META_ICC_NAME) == 0)
    return frame;

  try {
    /* Relative colorimetric: perceptual would add lcms's own gamut compression. depth pinned -
     * it follows the input otherwise, leaving the 8-bit path holding ushort reported as RGBA8. */
    return frame.icc_transform("srgb", vips::VImage::option()
                                         ->set("embedded", true)
                                         ->set("intent", VIPS_INTENT_RELATIVE)
                                         ->set("depth", 8));
  } catch (const vips::VError&) {
    /* A profile lcms cannot load is not worth failing the decode over. */
    vips_error_clear();
    return frame;
  }
}

/* Four float bands, so the pixel loop varies only by transfer function. The colour bands hold
 * whatever that loop's EOTF expects - the raw signal in [0, 1] for PQ and HLG, linear light
 * otherwise - and alpha always [0, 1]. */
static vips::VImage
prepare_hdr(Decoder* decoder, vips::VImage frame)
{
  /* Gainmaps never reach here: their base leaves through the plain 8-bit path. */
  double max = sample_max(frame);

  if (frame.bands() < 1)
    fail(EXC_DECODE, "Image has no bands");

  /* Grey to RGB first: the alpha bandjoin would leave two bands and the pixel loop reads four. */
  if (frame.bands() < 3) {
    vips::VImage grey = frame.extract_band(0);
    vips::VImage rgb = grey.bandjoin(std::vector<vips::VImage>{ grey, grey });
    frame = frame.bands() == 2 ? rgb.bandjoin(frame.extract_band(1)) : rgb;
  }

  if (frame.bands() < 4)
    frame = frame.bandjoin(max);
  else if (frame.bands() > 4)
    frame = frame.extract_band(0, vips::VImage::option()->set("n", 4));

  return frame.cast(VIPS_FORMAT_FLOAT).linear(1.0 / max, 0.0);
}

/* Into tightly packed RGBA half-float at [out], which must hold exactly [out_samples] uint16s.
 *
 * Extended sRGB, so an SDR pixel is numerically identical to what the 8-bit path would have
 * produced and nothing downstream has to know which it got. */
static void
write_hdr_pixels(Decoder* decoder, vips::VImage frame, uint16_t* out, size_t out_samples)
{
  const int width = frame.width();
  const int height = frame.height();
  const int kind = decoder->hdr_kind;

  /* The loop below steps four float bands per pixel, which prepare_hdr guarantees. */
  if (frame.bands() != 4 || frame.format() != VIPS_FORMAT_FLOAT)
    fail(EXC_DECODE, "write_hdr_pixels needs four float bands");

  check_dimensions(width, height);

  /* The caller sized [out] itself; a resize anywhere between would overrun a direct buffer. */
  if ((guint64)width * (guint64)height * 4 != (guint64)out_samples)
    fail(EXC_DECODE, "Pixel buffer does not match the image dimensions");

  /* Kinds with no signalled primaries default to BT.709, so the matrix comes back null - they
   * are ICC-converted before the frame gets here. */
  const float* matrix = hdr_matrix_to_srgb(decoder->signal.primaries);

  /* A strip at a time: a whole-image float copy would cost six more bytes per pixel. */
  const int strip = 64;

  RegionRef region(vips_region_new(frame.get_image()));
  if (!region.region)
    fail(EXC_OOM, "Out of memory creating the pixel region");

  for (int y = 0; y < height; y += strip) {
    VipsRect r;
    r.left = 0;
    r.top = y;
    r.width = width;
    r.height = VIPS_MIN(strip, height - y);

    /* RegionRef unrefs through the throw this can raise several frames down. */
    if (vips_region_prepare(region.region, &r))
      throw vips::VError();

    for (int i = 0; i < r.height; i++) {
      const float* p = (const float*)VIPS_REGION_ADDR(region.region, 0, y + i);
      if (!p)
        fail(EXC_DECODE, "Region address out of bounds");

      uint16_t* q = out + (size_t)(y + i) * (size_t)width * 4;

      for (int x = 0; x < width; x++) {
        float rgb[3] = { p[0], p[1], p[2] };
        float alpha = p[3];
        p += 4;

        switch (kind) {
          case HDR_PQ:
            rgb[0] = hdr_pq_eotf(rgb[0]);
            rgb[1] = hdr_pq_eotf(rgb[1]);
            rgb[2] = hdr_pq_eotf(rgb[2]);
            break;

          case HDR_HLG:
            rgb[0] = hdr_hlg_inverse_oetf(rgb[0]);
            rgb[1] = hdr_hlg_inverse_oetf(rgb[1]);
            rgb[2] = hdr_hlg_inverse_oetf(rgb[2]);
            hdr_hlg_ootf(rgb);
            break;

          default:
            /* Gainmap and scRGB arrive as linear light already. */
            break;
        }

        if (matrix)
          hdr_apply_matrix(matrix, rgb);

        q[0] = hdr_encode_half(rgb[0]);
        q[1] = hdr_encode_half(rgb[1]);
        q[2] = hdr_encode_half(rgb[2]);
        q[3] = hdr_encode_alpha(alpha);
        q += 4;
      }
    }
  }
}

/* The gainmap has to rotate with the base, or the two disagree on which way is up. */
static vips::VImage
apply_orientation(vips::VImage frame)
{
  if (frame.get_typeof(VIPS_META_ORIENTATION) == 0)
    return frame;

  int orientation = frame.get_int(VIPS_META_ORIENTATION);
  frame = frame.copy_memory().autorot();

  VipsImage* gainmap = vips_image_get_gainmap(frame.get_image());
  if (!gainmap) {
    vips_error_clear();
    return frame;
  }

  vips_image_set_int(gainmap, VIPS_META_ORIENTATION, orientation);

  VipsImage* rotated = nullptr;
  if (!vips_autorot(gainmap, &rotated, nullptr) && rotated) {
    vips_image_set_image(frame.get_image(), "gainmap", rotated);
    g_object_unref(rotated);
  } else {
    /* Leaving the map unrotated is wrong but recoverable; failing the decode is not. */
    vips_error_clear();
  }
  g_object_unref(gainmap);

  return frame;
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeDecode(JNIEnv* env, jobject obj, jint page,
                                                     jboolean crop, jboolean getTrim)
{
  jobject jgainmap = nullptr;
  jobject byteBuffer = nullptr;
  jobject result = nullptr;
  const char* err_cls = nullptr;
  std::string err_msg;

  try {
    Decoder* decoder = decoder_for(env, obj);

    if (page < 0 || page >= decoder->pages)
      fail(EXC_DECODE, "Page index out of range");

    vips::VImage frame = vips::VImage::new_from_buffer(
      decoder->buffer, decoder->buffer_size, "",
      vips::VImage::option()
        ->set("access", (crop || getTrim) ? VIPS_ACCESS_RANDOM : VIPS_ACCESS_SEQUENTIAL)
        ->set("page", page));

    check_dimensions(frame.width(), frame.height());

    frame = apply_orientation(frame);

    /* A gainmap's base is 8-bit sRGB; only the transfer-function paths hand back float. */
    const bool float_out = decoder->hdr_kind == HDR_PQ || decoder->hdr_kind == HDR_HLG ||
                           decoder->hdr_kind == HDR_LINEAR;

    /* Before anything else touches the image: vips metadata rides along by convention, so an
     * ICC transform dropping the gainmap fields would lose the HDR silently. The map is a
     * multiplier, not colour, so it wants no colour management of its own. */
    if (decoder->hdr_kind == HDR_GAINMAP)
      jgainmap = build_gainmap(env, frame);

    /* Container-signalled primaries go through the matrix in the pixel loop; an ICC profile -
     * the only place a plain Display P3 JPEG says so - goes through lcms here. */
    if (decoder->hdr_kind == HDR_NONE || decoder->hdr_kind == HDR_GAINMAP)
      frame = to_srgb_primaries(frame);

    /* find_trim matches in 8-bit sRGB, so trimming measures an SDR rendition even when the
     * pixels handed back are HDR. Both views come off the same load. */
    vips::VImage trim_frame = frame;
    if (trim_frame.interpretation() != VIPS_INTERPRETATION_sRGB &&
        trim_frame.interpretation() != VIPS_INTERPRETATION_scRGB)
      trim_frame = trim_frame.colourspace(VIPS_INTERPRETATION_sRGB);

    if (float_out) {
      frame = prepare_hdr(decoder, frame);
    } else {
      frame = trim_frame;

      if (frame.bands() < 4)
        frame = frame.bandjoin(255);
      if (frame.bands() > 4)
        frame = frame.extract_band(0, vips::VImage::option()->set("n", 4));
    }

    const int full_width = frame.width();
    const int full_height = frame.height();
    check_dimensions(full_width, full_height);

    int width = full_width;
    int height = full_height;

    int duration = 0;
    if (decoder->durations && page < decoder->durations_count)
      duration = decoder->durations[page];
    /* A negative delay from a corrupt header runs an animation backwards forever. */
    if (duration < 0)
      duration = 0;

    int trim_left = 0;
    int trim_top = 0;
    int trim_width = 0;
    int trim_height = 0;

    if (crop || getTrim) {
      int trim_top_w = 0, trim_width_w = 0, trim_height_w = 0;
      int trim_left_w = trim_frame.find_trim(&trim_top_w, &trim_width_w, &trim_height_w,
                                             vips::VImage::option()->set("line_art", true));

      int trim_top_b = 0, trim_width_b = 0, trim_height_b = 0;
      int trim_left_b =
        trim_frame.find_trim(&trim_top_b, &trim_width_b, &trim_height_b,
                             vips::VImage::option()->set("line_art", true)->set("background", 0.0));

      trim_left = std::max(trim_left_w, trim_left_b);
      trim_top = std::max(trim_top_w, trim_top_b);
      trim_width = std::min(trim_width_w, trim_width_b);
      trim_height = std::min(trim_height_w, trim_height_b);

      /* An all-background image trims to an empty rect, and combining two independent runs can
       * push it past the edge. Either one reaching crop failed the whole decode. */
      trim_left = CLAMP(trim_left, 0, full_width - 1);
      trim_top = CLAMP(trim_top, 0, full_height - 1);
      trim_width = CLAMP(trim_width, 0, full_width - trim_left);
      trim_height = CLAMP(trim_height, 0, full_height - trim_top);

      if (trim_width <= 0 || trim_height <= 0) {
        trim_left = 0;
        trim_top = 0;
        trim_width = full_width;
        trim_height = full_height;
      }
    }

    if (crop) {
      frame = frame.crop(trim_left, trim_top, trim_width, trim_height);
      width = trim_width;
      height = trim_height;
      trim_left = 0;
      trim_top = 0;
      trim_width = 0;
      trim_height = 0;
    }

    /* From the image about to be written, not the dimensions reported before the crop. */
    check_dimensions(frame.width(), frame.height());
    if (frame.width() != width || frame.height() != height)
      fail(EXC_DECODE, "Image dimensions changed unexpectedly");

    const int pixel_format = float_out ? PIXFMT_RGBA16F : PIXFMT_RGBA8;
    const guint64 pixels = (guint64)width * (guint64)height;

    /* Four float bands in, half-float out, so the HDR buffer is half what the vips image says. */
    const size_t size =
      float_out ? checked_buffer_size(pixels, 4 * sizeof(uint16_t))
                : checked_buffer_size(pixels, (guint64)VIPS_IMAGE_SIZEOF_PEL(frame.get_image()));

    void* data = nullptr;
    byteBuffer = allocate_direct(env, size, &data);

    if (float_out)
      write_hdr_pixels(decoder, frame, (uint16_t*)data, size / sizeof(uint16_t));
    else
      frame.write(vips::VImage::new_from_memory(data, size, frame.width(), frame.height(),
                                                frame.bands(), frame.format()));

    jclass cls = find_class_checked(env, "ca/mpreg/imagedecoder/ImageDecoder$DecodeResult");
    LocalRef cls_ref(env, cls);
    jmethodID ctor = get_method_checked(
      env, cls, "<init>",
      "(Ljava/nio/ByteBuffer;IIIIIIIIFLca/mpreg/imagedecoder/ImageDecoder$Gainmap;)V");

    result = env->NewObject(cls, ctor, byteBuffer, width, height, duration, trim_left, trim_top,
                            trim_width, trim_height, pixel_format, decoder->hdr_headroom, jgainmap);
    if (!result || env->ExceptionCheck()) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      if (result)
        env->DeleteLocalRef(result);
      result = nullptr;
      fail(EXC_OOM, "Out of memory creating the decode result");
    }
  } catch (const DecodeError& e) {
    err_cls = e.cls;
    err_msg = e.msg;
  } catch (const vips::VError& e) {
    /* Formatted here so the refs below are released either way. */
    err_cls = nullptr;
    if (byteBuffer)
      env->DeleteLocalRef(byteBuffer);
    if (jgainmap)
      env->DeleteLocalRef(jgainmap);
    throw_vips_error(env, e);
    return nullptr;
  } catch (const std::bad_alloc&) {
    err_cls = EXC_OOM;
    err_msg = "Out of memory decoding the image";
  } catch (...) {
    err_cls = EXC_DECODE;
    err_msg = "Unknown error decoding the image";
  }

  if (byteBuffer)
    env->DeleteLocalRef(byteBuffer);
  if (jgainmap)
    env->DeleteLocalRef(jgainmap);

  if (err_cls) {
    vips_error_clear();
    throw_decode_error(env, err_cls, err_msg.c_str());
    return nullptr;
  }

  return result;
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeEncode(JNIEnv* env, jobject obj, jstring jsuffix,
                                                     jint page)
{
  if (!jsuffix) {
    throw_decode_error(env, EXC_DECODE, "Encode suffix is null");
    return nullptr;
  }

  const char* suffix = env->GetStringUTFChars(jsuffix, nullptr);
  if (!suffix) {
    if (env->ExceptionCheck())
      env->ExceptionClear();
    throw_decode_error(env, EXC_OOM, "Out of memory reading the encode suffix");
    return nullptr;
  }

  /* write_to_buffer hands over g_malloc'd bytes; every path must transfer or free them. */
  void* data = nullptr;
  size_t size = 0;
  jobject byteBuffer = nullptr;
  jobject result = nullptr;
  const char* err_cls = nullptr;
  std::string err_msg;
  bool vips_thrown = false;

  try {
    Decoder* decoder = decoder_for(env, obj);

    /* -1 is libvips's "every page"; anything else has to name a real one. */
    if (page < -1 || page >= decoder->pages)
      fail(EXC_DECODE, "Page index out of range");

    vips::VImage frame = vips::VImage::new_from_buffer(decoder->buffer, decoder->buffer_size, "",
                                                       vips::VImage::option()->set("page", page));

    check_dimensions(frame.width(), frame.height());

    frame = apply_orientation(frame);

    frame.write_to_buffer(suffix, &data, &size);

    if (!data || size == 0)
      fail(EXC_DECODE, "Encoder produced no data");
    if ((guint64)size > MAX_OUTPUT_BYTES)
      fail_oom("Encoded image exceeds the maximum direct buffer size", (guint64)size);

    byteBuffer = env->NewDirectByteBuffer(data, (jlong)size);
    if (!byteBuffer || env->ExceptionCheck()) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      fail(EXC_OOM, "Failed to allocate the direct byte buffer");
    }

    jclass cls = find_class_checked(env, "ca/mpreg/imagedecoder/ImageDecoder$EncodeResult");
    LocalRef cls_ref(env, cls);
    jmethodID ctor = get_method_checked(env, cls, "<init>", "(JLjava/nio/ByteBuffer;)V");

    result = env->NewObject(cls, ctor, (jlong)(intptr_t)data, byteBuffer);
    if (!result || env->ExceptionCheck()) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      if (result)
        env->DeleteLocalRef(result);
      result = nullptr;
      /* Nothing took the buffer, so this call still owns it. */
      fail(EXC_OOM, "Out of memory creating the encode result");
    }

    /* Ownership of [data] now sits in the EncodeResult. */
    data = nullptr;
  } catch (const DecodeError& e) {
    err_cls = e.cls;
    err_msg = e.msg;
  } catch (const vips::VError& e) {
    vips_thrown = true;
    env->ReleaseStringUTFChars(jsuffix, suffix);
    suffix = nullptr;
    if (byteBuffer)
      env->DeleteLocalRef(byteBuffer);
    byteBuffer = nullptr;
    g_free(data);
    data = nullptr;
    throw_vips_error(env, e);
  } catch (const std::bad_alloc&) {
    err_cls = EXC_OOM;
    err_msg = "Out of memory encoding the image";
  } catch (...) {
    err_cls = EXC_DECODE;
    err_msg = "Unknown error encoding the image";
  }

  if (suffix)
    env->ReleaseStringUTFChars(jsuffix, suffix);
  if (byteBuffer)
    env->DeleteLocalRef(byteBuffer);
  g_free(data);

  if (vips_thrown)
    return nullptr;

  if (err_cls) {
    vips_error_clear();
    throw_decode_error(env, err_cls, err_msg.c_str());
    return nullptr;
  }

  return result;
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_00024EncodeResult_nativeFree(JNIEnv* env, jobject obj)
{
  jlong ptr = take_ptr(env, obj);
  if (ptr == 0)
    return;
  g_free((void*)(intptr_t)ptr);
}
