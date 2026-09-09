#include <stdint.h>
#include <vector>

#include <glib.h>
#include <jni.h>
#include <vips/vips8>

#include "hdr.h"

using namespace vips;

static void
throw_vips_error(JNIEnv* env, const vips::VError& e)
{
  std::string msg = e.what();
  vips_error_clear();
  if (env->ExceptionCheck())
    env->ExceptionClear();
  const char* cls = msg.find("not in a known format") != std::string::npos
                      ? "ca/mpreg/imagedecoder/ImageDecoder$UnknownFormatException"
                      : "ca/mpreg/imagedecoder/ImageDecoder$DecodeException";
  env->ThrowNew(env->FindClass(cls), msg.c_str());
}

jint
JNI_OnLoad(JavaVM* vm, void*)
{
  VIPS_INIT("VipsDecoder");
  vips_concurrency_set(1);

  JNIEnv* env;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;

  return JNI_VERSION_1_6;
}

jlong
get_ptr(JNIEnv* env, jobject obj)
{
  jclass cls = env->GetObjectClass(obj);
  jfieldID ptr_field = env->GetFieldID(cls, "ptr", "J");
  return env->GetLongField(obj, ptr_field);
}

jlong
take_ptr(JNIEnv* env, jobject obj)
{
  jclass cls = env->GetObjectClass(obj);
  jfieldID ptr_field = env->GetFieldID(cls, "ptr", "J");
  jlong ptr = env->GetLongField(obj, ptr_field);
  env->SetLongField(obj, ptr_field, 0L);
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
  Decoder* d = g_new0(Decoder, 1);
  return d;
}

static void
decoder_free(Decoder* d)
{
  if (!d) {
    return;
  }
  g_free(d->buffer);
  g_free(d->durations);
  g_free(d);
}

static uint8_t*
read_all(JNIEnv* env, jobject jstream, size_t* out_size)
{
  jclass cls = env->GetObjectClass(jstream);
  jmethodID readMethod = env->GetMethodID(cls, "read", "([B)I");

  jbyteArray buf = env->NewByteArray(8192);
  GByteArray* result = g_byte_array_new();

  while (true) {
    jint n = env->CallIntMethod(jstream, readMethod, buf);

    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      break;
    }

    if (n <= 0) {
      break;
    }
    jbyte* bytes = env->GetByteArrayElements(buf, nullptr);
    if (bytes == nullptr)
      break;
    g_byte_array_append(result, (const guint8*)bytes, n);
    env->ReleaseByteArrayElements(buf, bytes, JNI_ABORT);
  }

  *out_size = result->len;
  uint8_t* data = g_byte_array_free(result, FALSE); // transfers ownership
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

  double* boost;
  int n;
  try {
    image.get_array_double("gainmap-max-content-boost", &boost, &n);
  } catch (const vips::VError&) {
    vips_error_clear();
    return 0.0f;
  }

  double max_boost = 1.0;
  for (int i = 0; i < n; i++)
    max_boost = VIPS_MAX(max_boost, boost[i]);

  return (float)log2(max_boost);
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
      /* A declared peak beats the nominal one; used only to tone map when HDR can't present. */
      decoder->hdr_headroom =
        peak_nits > HDR_SDR_WHITE_NITS ? (float)log2(peak_nits / HDR_SDR_WHITE_NITS)
        : kind == HDR_PQ               ? (float)log2(10000.0 / HDR_SDR_WHITE_NITS)
                                       : (float)log2(HDR_HLG_PEAK_NITS / HDR_SDR_WHITE_NITS);
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

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_new(JNIEnv* env, jclass, jobject jstream)
{
  Decoder* decoder = decoder_new();
  decoder->buffer = read_all(env, jstream, &decoder->buffer_size);

  if (decoder->buffer_size == 0 || decoder->buffer == nullptr) {
    decoder_free(decoder);
    if (env->ExceptionCheck())
      env->ExceptionClear();
    env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
                  "Empty or unreadable image stream");
    return nullptr;
  }

  {
    const size_t n = MIN(decoder->buffer_size, (size_t)16);
    char hex[64] = {};
    for (size_t i = 0; i < n; i++) {
      sprintf(hex + i * 3, "%02x ", decoder->buffer[i]);
    }
  }

  try {
    vips::VImage image = vips::VImage::new_from_buffer(decoder->buffer, decoder->buffer_size, "");

    decoder->pages =
      image.get_typeof(VIPS_META_N_PAGES) != 0 ? image.get_int(VIPS_META_N_PAGES) : 1;

    if (decoder->pages > 0 && image.get_typeof("delay") != 0) {
      int* delays;
      int n;

      image.get_array_int("delay", &delays, &n);
      decoder->durations = g_new(int, n);
      decoder->durations_count = n;
      memcpy(decoder->durations, delays, n * sizeof(int));
    }

    detect_hdr(decoder, image);
    bool is_hdr = decoder->hdr_kind != HDR_NONE;

    const char* loader_cstr =
      image.get_typeof("vips-loader") != 0 ? image.get_string("vips-loader") : "";
    std::string loader_str = loader_cstr ? loader_cstr : "";

    image = vips::VImage();
    jstring jloader = env->NewStringUTF(loader_str.c_str());
    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(JIIZIFLjava/lang/String;)V");
    return env->NewObject(cls, ctor, reinterpret_cast<jlong>(decoder), decoder->pages, 0, is_hdr,
                          decoder->hdr_kind, decoder->hdr_headroom, jloader);
  } catch (const vips::VError& e) {
    decoder_free(decoder);
    throw_vips_error(env, e);
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_free(JNIEnv* env, jobject obj)
{
  jlong ptr = get_ptr(env, obj);
  Decoder* decoder = reinterpret_cast<Decoder*>(ptr);
  decoder_free(decoder);
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
      double* d;
      int n;
      image.get_array_double(field, &d, &n);
      for (int i = 0; i < 3; i++)
        values[i] = (float)d[VIPS_MIN(i, n - 1)];
    } catch (const vips::VError&) {
      vips_error_clear();
    }
  }

  jfloatArray arr = env->NewFloatArray(3);
  if (arr)
    env->SetFloatArrayRegion(arr, 0, 3, values);
  return arr;
}

/* Handed over unapplied for the viewer to combine: applying the gain is a display decision, and
 * libvips's own uhdr2scRGB gets three-channel maps wrong anyway - it runs the map through the
 * sRGB EOTF where libultrahdr's applyGain uses the raw value. */
static jobject
build_gainmap(JNIEnv* env, vips::VImage& image)
{
  VipsImage* raw = vips_image_get_gainmap(image.get_image());
  if (!raw) {
    vips_error_clear();
    return nullptr;
  }

  vips::VImage map(raw);

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
    const size_t size = (size_t)width * height * bands;

    jclass buffer_cls = env->FindClass("java/nio/ByteBuffer");
    jmethodID allocate =
      env->GetStaticMethodID(buffer_cls, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
    jobject buffer = size <= (size_t)G_MAXINT
                       ? env->CallStaticObjectMethod(buffer_cls, allocate, (jint)size)
                       : nullptr;
    if (!buffer || env->ExceptionCheck()) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      return nullptr;
    }

    void* data = env->GetDirectBufferAddress(buffer);
    if (!data)
      return nullptr;

    map.write(vips::VImage::new_from_memory(data, size, width, height, bands, VIPS_FORMAT_UCHAR));

    jfloatArray gamma = gainmap_metadata_array(env, image, "gainmap-gamma", 1.0f);
    jfloatArray min_boost = gainmap_metadata_array(env, image, "gainmap-min-content-boost", 1.0f);
    jfloatArray max_boost = gainmap_metadata_array(env, image, "gainmap-max-content-boost", 1.0f);
    jfloatArray offset_sdr = gainmap_metadata_array(env, image, "gainmap-offset-sdr", 0.0f);
    jfloatArray offset_hdr = gainmap_metadata_array(env, image, "gainmap-offset-hdr", 0.0f);

    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$Gainmap");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/nio/ByteBuffer;III[F[F[F[F[F)V");
    return env->NewObject(cls, ctor, buffer, width, height, bands, gamma, min_boost, max_boost,
                          offset_sdr, offset_hdr);
  } catch (const vips::VError&) {
    vips_error_clear();
    return nullptr;
  }
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

/* Into tightly packed RGBA half-float at [out], which must hold width * height * 4 samples.
 *
 * Extended sRGB, so an SDR pixel is numerically identical to what the 8-bit path would have
 * produced and nothing downstream has to know which it got. */
static void
write_hdr_pixels(Decoder* decoder, vips::VImage frame, uint16_t* out)
{
  const int width = frame.width();
  const int height = frame.height();
  const int kind = decoder->hdr_kind;

  /* The loop below steps four float bands per pixel, which prepare_hdr guarantees. */
  if (frame.bands() != 4 || frame.format() != VIPS_FORMAT_FLOAT)
    throw vips::VError("write_hdr_pixels needs four float bands");

  /* Kinds with no signalled primaries default to BT.709, so the matrix comes back null - they
   * are ICC-converted before the frame gets here. */
  const float* matrix = hdr_matrix_to_srgb(decoder->signal.primaries);

  /* A strip at a time: a whole-image float copy would cost six more bytes per pixel. */
  const int strip = 64;

  VipsRegion* region = vips_region_new(frame.get_image());
  if (!region)
    throw vips::VError("vips_region_new failed");

  for (int y = 0; y < height; y += strip) {
    VipsRect r;
    r.left = 0;
    r.top = y;
    r.width = width;
    r.height = VIPS_MIN(strip, height - y);

    if (vips_region_prepare(region, &r)) {
      g_object_unref(region);
      throw vips::VError();
    }

    for (int i = 0; i < r.height; i++) {
      const float* p = (const float*)VIPS_REGION_ADDR(region, 0, y + i);
      uint16_t* q = out + (size_t)(y + i) * width * 4;

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

  g_object_unref(region);
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_decode(JNIEnv* env, jobject obj, jint page, jboolean crop,
                                               jboolean getTrim)
{
  jlong ptr = get_ptr(env, obj);
  Decoder* decoder = reinterpret_cast<Decoder*>(ptr);

  try {
    vips::VImage frame = vips::VImage::new_from_buffer(
      decoder->buffer, decoder->buffer_size, "",
      vips::VImage::option()
        ->set("access", (crop || getTrim) ? VIPS_ACCESS_RANDOM : VIPS_ACCESS_SEQUENTIAL)
        ->set("page", page));

    /* A gainmap's base is 8-bit sRGB; only the transfer-function paths hand back float. */
    const bool float_out = decoder->hdr_kind == HDR_PQ || decoder->hdr_kind == HDR_HLG ||
                           decoder->hdr_kind == HDR_LINEAR;

    /* Before anything else touches the image: vips metadata rides along by convention, so an
     * ICC transform dropping the gainmap fields would lose the HDR silently. The map is a
     * multiplier, not colour, so it wants no colour management of its own. */
    jobject jgainmap = decoder->hdr_kind == HDR_GAINMAP ? build_gainmap(env, frame) : nullptr;

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

    int width = frame.width();
    int height = frame.height();

    int duration = 0;
    if (decoder->pages > 0 && page < decoder->durations_count)
      duration = decoder->durations[page];

    int trim_left = 0;
    int trim_top = 0;
    int trim_width = 0;
    int trim_height = 0;

    if (crop || getTrim) {
      int trim_left_w, trim_top_w, trim_width_w, trim_height_w;
      trim_left_w = trim_frame.find_trim(&trim_top_w, &trim_width_w, &trim_height_w,
                                         vips::VImage::option()->set("line_art", true));

      int trim_left_b, trim_top_b, trim_width_b, trim_height_b;
      trim_left_b =
        trim_frame.find_trim(&trim_top_b, &trim_width_b, &trim_height_b,
                             vips::VImage::option()->set("line_art", true)->set("background", 0.0));

      trim_left = std::max(trim_left_w, trim_left_b);
      trim_top = std::max(trim_top_w, trim_top_b);
      trim_width = std::min(trim_width_w, trim_width_b);
      trim_height = std::min(trim_height_w, trim_height_b);
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

    /* Four float bands in, half-float out, so the buffer is half what the vips image says. */
    const int pixel_format = float_out ? PIXFMT_RGBA16F : PIXFMT_RGBA8;
    size_t size = float_out ? (size_t)width * height * 4 * sizeof(uint16_t)
                            : VIPS_IMAGE_SIZEOF_IMAGE(frame.get_image());

    jclass bufferCls = env->FindClass("java/nio/ByteBuffer");
    jmethodID allocateDirect =
      env->GetStaticMethodID(bufferCls, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");

    jobject byteBuffer = size <= (size_t)G_MAXINT
                           ? env->CallStaticObjectMethod(bufferCls, allocateDirect, (jint)size)
                           : nullptr;
    if (!byteBuffer || env->ExceptionCheck()) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
                    "Out of memory");
      return nullptr;
    }

    void* data = env->GetDirectBufferAddress(byteBuffer);
    if (!data) {
      if (env->ExceptionCheck())
        env->ExceptionClear();
      env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
                    "Failed to allocate direct byte buffer");
      return nullptr;
    }

    if (float_out)
      write_hdr_pixels(decoder, frame, (uint16_t*)data);
    else
      frame.write(vips::VImage::new_from_memory(data, size, frame.width(), frame.height(),
                                                frame.bands(), frame.format()));

    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeResult");
    jmethodID ctor = env->GetMethodID(
      cls, "<init>",
      "(Ljava/nio/ByteBuffer;IIIIIIIIFLca/mpreg/imagedecoder/ImageDecoder$Gainmap;)V");
    return env->NewObject(cls, ctor, byteBuffer, width, height, duration, trim_left, trim_top,
                          trim_width, trim_height, pixel_format, decoder->hdr_headroom, jgainmap);
  } catch (const vips::VError& e) {
    throw_vips_error(env, e);
    return nullptr;
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_encode(JNIEnv* env, jobject obj, jstring jsuffix, jint page)
{
  jlong ptr = get_ptr(env, obj);
  Decoder* decoder = reinterpret_cast<Decoder*>(ptr);

  const char* suffix = env->GetStringUTFChars(jsuffix, nullptr);

  try {
    vips::VImage frame = vips::VImage::new_from_buffer(decoder->buffer, decoder->buffer_size, "",
                                                       vips::VImage::option()->set("page", page));

    size_t size;
    void* data;
    frame.write_to_buffer(suffix, &data, &size);

    env->ReleaseStringUTFChars(jsuffix, suffix);

    jobject byteBuffer = env->NewDirectByteBuffer(data, size);
    if (!byteBuffer) {
      g_free(data);
      if (env->ExceptionCheck())
        env->ExceptionClear();
      env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
                    "Failed to allocate direct byte buffer");
      return nullptr;
    }

    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$EncodeResult");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(JLjava/nio/ByteBuffer;)V");
    return env->NewObject(cls, ctor, (jlong)(intptr_t)data, byteBuffer);
  } catch (const vips::VError& e) {
    env->ReleaseStringUTFChars(jsuffix, suffix);
    throw_vips_error(env, e);
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_00024EncodeResult_free(JNIEnv* env, jobject obj)
{
  jlong ptr = take_ptr(env, obj);
  if (ptr == 0)
    return;
  g_free((void*)(intptr_t)ptr);
}
