#include <stdint.h>
#include <vector>

#include <glib.h>
#include <jni.h>
#include <vips/vips8>

using namespace vips;

jint
JNI_OnLoad(JavaVM* vm, void*)
{
  VIPS_INIT("VipsDecoder");
  vips_concurrency_set(1);

  JNIEnv* env;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  return JNI_VERSION_1_6;
}

jlong
get_ptr(JNIEnv* env, jobject obj)
{
  jclass cls = env->GetObjectClass(obj);
  jfieldID ptr_field = env->GetFieldID(cls, "ptr", "J");
  return env->GetLongField(obj, ptr_field);
}

std::vector<uint8_t>
read_all(JNIEnv* env, jobject jstream)
{
  jclass cls = env->GetObjectClass(jstream);
  jmethodID readMethod = env->GetMethodID(cls, "read", "([B)I");

  jbyteArray buf = env->NewByteArray(8192);
  std::vector<uint8_t> result;

  while (true) {
    jint n = env->CallIntMethod(jstream, readMethod, buf);
    if (n <= 0) {
      break;
    }
    jbyte* bytes = env->GetByteArrayElements(buf, nullptr);
    result.insert(result.end(), bytes, bytes + n);
    env->ReleaseByteArrayElements(buf, bytes, JNI_ABORT);
  }

  return result;
}

struct Decoder
{
  std::vector<uint8_t> buffer;
  int pages;
  std::vector<int> durations;
};

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_new(JNIEnv* env, jclass, jobject jstream)
{
  auto* decoder = new Decoder();
  decoder->buffer = read_all(env, jstream);

  vips::VImage image;
  try {
    image = vips::VImage::new_from_buffer(decoder->buffer.data(), decoder->buffer.size(), "");
    decoder->pages =
      image.get_typeof(VIPS_META_N_PAGES) != 0 ? image.get_int(VIPS_META_N_PAGES) : 1;

    if (decoder->pages > 0 && image.get_typeof("delay") != 0) {
      int* delays;
      int n;

      image.get_array_int("delay", &delays, &n);

      decoder->durations.resize(n);
      memcpy(decoder->durations.data(), delays, n * 4);
    }

    auto is_hdr = image.interpretation() == VIPS_INTERPRETATION_scRGB;

    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(JIIZ)V");
    return env->NewObject(cls, ctor, reinterpret_cast<jlong>(decoder), decoder->pages, 0, is_hdr);
  } catch (const vips::VError& e) {
    delete decoder;

    env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"), e.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_free(JNIEnv* env, jobject obj)
{
  jlong ptr = get_ptr(env, obj);
  auto* decoder = reinterpret_cast<Decoder*>(ptr);
  delete decoder;
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_decode(JNIEnv* env, jobject obj, jint page, jboolean crop,
                                               jboolean getTrim)
{
  jlong ptr = get_ptr(env, obj);
  auto* decoder = reinterpret_cast<Decoder*>(ptr);

  try {
    vips::VImage frame = vips::VImage::new_from_buffer(
      decoder->buffer.data(), decoder->buffer.size(), "",
      vips::VImage::option()
        ->set("access", (crop || getTrim) ? VIPS_ACCESS_RANDOM : VIPS_ACCESS_SEQUENTIAL)
        ->set("page", page));

    if (frame.interpretation() != VIPS_INTERPRETATION_sRGB &&
        frame.interpretation() != VIPS_INTERPRETATION_scRGB) {
      frame = frame.colourspace(VIPS_INTERPRETATION_sRGB);
    }

    if (frame.bands() < 4) {
      frame = frame.bandjoin(255);
    }
    if (frame.bands() > 4) {
      frame = frame.extract_band(0, vips::VImage::option()->set("n", 4));
    }

    int width = frame.width();
    int height = frame.height();

    int duration = 0;
    if (decoder->pages > 0 && page < decoder->durations.size()) {
      duration = decoder->durations[page];
    }

    int trim_left = 0;
    int trim_top = 0;
    int trim_width = 0;
    int trim_height = 0;

    if (crop || getTrim) {
      int trim_left_w, trim_top_w, trim_width_w, trim_height_w;
      trim_left_w = frame.find_trim(&trim_top_w, &trim_width_w, &trim_height_w,
                                    vips::VImage::option() //
                                      ->set("line_art", true));

      int trim_left_b, trim_top_b, trim_width_b, trim_height_b;
      trim_left_b = frame.find_trim(&trim_top_b, &trim_width_b, &trim_height_b,
                                    vips::VImage::option() //
                                      ->set("line_art", true)
                                      ->set("background", 0.0));

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

    size_t size;
    void* data = frame.write_to_memory(&size);

    if (!data) {
      env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
                    "Out of memory");
      return nullptr;
    }

    jobject byteBuffer = env->NewDirectByteBuffer(data, size);

    if (!byteBuffer) {
      g_free(data);
      env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
                    "Failed to allocate direct byte buffer");
      return nullptr;
    }

    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeResult");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(JLjava/nio/ByteBuffer;IIIIIII)V");
    return env->NewObject(cls, ctor, (jlong)(intptr_t)data, byteBuffer, width, height, duration,
                          trim_left, trim_top, trim_width, trim_height);
  } catch (const vips::VError& e) {
    env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"), e.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_00024DecodeResult_free(JNIEnv* env, jobject obj)
{
  jlong ptr = get_ptr(env, obj);
  g_free((void*)(intptr_t)ptr);
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_encode(JNIEnv* env, jobject obj, jstring jsuffix, jint page)
{
  jlong ptr = get_ptr(env, obj);
  auto* decoder = reinterpret_cast<Decoder*>(ptr);

  try {
    vips::VImage frame =
      vips::VImage::new_from_buffer(decoder->buffer.data(), decoder->buffer.size(), "",
                                    vips::VImage::option()->set("page", page));

    const char* suffix = env->GetStringUTFChars(jsuffix, NULL);

    size_t size;
    void* data;
    frame.write_to_buffer(suffix, &data, &size);

    env->ReleaseStringUTFChars(jsuffix, suffix);

    jobject byteBuffer = env->NewDirectByteBuffer(data, size);

    jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$EncodeResult");
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(JLjava/nio/ByteBuffer;)V");
    return env->NewObject(cls, ctor, (jlong)(intptr_t)data, byteBuffer);
  } catch (const vips::VError& e) {
    env->ThrowNew(env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"), e.what());
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_00024EncodeResult_free(JNIEnv* env, jobject obj)
{
  jlong ptr = get_ptr(env, obj);
  g_free((void*)(intptr_t)ptr);
}
