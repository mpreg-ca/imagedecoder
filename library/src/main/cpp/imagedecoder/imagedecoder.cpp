#include <stdint.h>
#include <vector>

#include <glib.h>
#include <jni.h>
#include <vips/vips8>

using namespace vips;

jint JNI_OnLoad(JavaVM *vm, void *) {
  VIPS_INIT("VipsDecoder");
  vips_concurrency_set(1);

  JNIEnv *env;
  if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  return JNI_VERSION_1_6;
}

std::vector<uint8_t> read_all(JNIEnv *env, jobject jstream) {
  jclass cls = env->GetObjectClass(jstream);
  jmethodID readMethod = env->GetMethodID(cls, "read", "([B)I");

  jbyteArray buf = env->NewByteArray(8192);
  std::vector<uint8_t> result;

  while (true) {
    jint n = env->CallIntMethod(jstream, readMethod, buf);
    if (n <= 0) {
      break;
    }
    jbyte *bytes = env->GetByteArrayElements(buf, nullptr);
    result.insert(result.end(), bytes, bytes + n);
    env->ReleaseByteArrayElements(buf, bytes, JNI_ABORT);
  }

  return result;
}

struct Decoder {
  std::vector<uint8_t> buffer;
  int page;
  int count;
};

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_newInstance(JNIEnv *env, jclass,
                                                    jobject jstream) {
  auto *decoder = new Decoder();
  decoder->buffer = read_all(env, jstream);
  decoder->page = 0;

  vips::VImage image;
  try {
    image = vips::VImage::new_from_buffer(
        decoder->buffer.data(), decoder->buffer.size(), "",
        vips::VImage::option()->set("access", VIPS_ACCESS_SEQUENTIAL));
  } catch (const vips::VError &e) {
    delete decoder;

    env->ThrowNew(
        env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
        e.what());
  }

  decoder->count = image.get_typeof(VIPS_META_N_PAGES) != 0
                       ? image.get_int(VIPS_META_N_PAGES)
                       : 1;
  auto is_hdr = image.interpretation() == VIPS_INTERPRETATION_scRGB;

  jclass cls = env->FindClass("ca/mpreg/imagedecoder/ImageDecoder");
  jmethodID ctor = env->GetMethodID(cls, "<init>", "(JIZ)V");
  return env->NewObject(cls, ctor, reinterpret_cast<jlong>(decoder),
                        decoder->count, is_hdr);
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_decodeNext(JNIEnv *env, jobject,
                                                   jlong ptr, bool crop) {
  auto *decoder = reinterpret_cast<Decoder *>(ptr);

  if (decoder->page >= decoder->count) {
    decoder->page = 0;
  }

  try {
    vips::VImage frame = vips::VImage::new_from_buffer(
        decoder->buffer.data(), decoder->buffer.size(), "",
        vips::VImage::option()
            ->set("access", crop ? VIPS_ACCESS_RANDOM : VIPS_ACCESS_SEQUENTIAL)
            ->set("page", decoder->page));

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
    if (decoder->count > 0 && frame.get_typeof("delay") != 0) {
      int *delays;
      int n;
      frame.get_array_int("delay", &delays, &n);
      if (decoder->page < n) {
        duration = delays[decoder->page];
      }
    }

    if (crop) {
      int top;
      int left = frame.find_trim(&top, &width, &height);
      frame = frame.crop(left, top, width, height);
    }

    size_t size;
    void *data = frame.write_to_memory(&size);

    decoder->page++;

    jobject byteBuffer = env->NewDirectByteBuffer(data, size);

    jclass cls =
        env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeResult");
    jmethodID ctor =
        env->GetMethodID(cls, "<init>", "(JLjava/nio/ByteBuffer;IIII)V");
    return env->NewObject(cls, ctor, data, byteBuffer, width, height, duration,
                          decoder->page - 1);
  } catch (const vips::VError &e) {
    env->ThrowNew(
        env->FindClass("ca/mpreg/imagedecoder/ImageDecoder$DecodeException"),
        e.what());
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_free(JNIEnv *, jobject, jlong ptr) {
  auto *decoder = reinterpret_cast<Decoder *>(ptr);
  delete decoder;
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_00024DecodeResult_free(JNIEnv *,
                                                               jobject,
                                                               jlong ptr) {
  g_free((void *)(intptr_t)ptr);
}
