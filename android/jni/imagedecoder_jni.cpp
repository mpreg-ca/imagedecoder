#include <jni.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdint.h>
#include <string>
#include <strings.h>
#include <vector>

#include "decoder_factory.h"
#include "exif.h"

using imagedecoder::BaseDecoder;
using imagedecoder::DecodeOptions;
using imagedecoder::DecodeProgress;
using imagedecoder::ImageInfo;

// Whole file is buffered, so its length is the first lever on the heap.
static const size_t MAX_INPUT_BYTES = (size_t)512 * 1024 * 1024;

static const uint64_t MAX_PIXELS = (uint64_t)1 << 28;

static const uint64_t MAX_OUTPUT_BYTES = (uint64_t)INT32_MAX;

static const char *const CLS_DECODER = "ca/mpreg/imagedecoder/ImageDecoder";
static const char *const CLS_RESULT =
    "ca/mpreg/imagedecoder/ImageDecoder$Frame";
static const char *const CLS_GAINMAP =
    "ca/mpreg/imagedecoder/ImageDecoder$Gainmap";
static const char *const EXC_DECODE =
    "ca/mpreg/imagedecoder/ImageDecoder$DecodeException";
static const char *const EXC_UNKNOWN_FORMAT =
    "ca/mpreg/imagedecoder/ImageDecoder$UnknownFormatException";
static const char *const EXC_OOM =
    "ca/mpreg/imagedecoder/ImageDecoder$OutOfMemoryException";
static const char *const EXC_NEED_DATA =
    "ca/mpreg/imagedecoder/ImageDecoder$NeedMoreDataException";

template <typename T> struct LocalRef {
  JNIEnv *env;
  T ref;

  LocalRef(JNIEnv *env_, T ref_) : env(env_), ref(ref_) {}
  ~LocalRef() {
    if (ref) {
      env->DeleteLocalRef(ref);
    }
  }
  LocalRef(const LocalRef &) = delete;
  LocalRef &operator=(const LocalRef &) = delete;

  operator T() const { return ref; }
  explicit operator bool() const { return ref != nullptr; }
};

struct DecodeError {
  const char *cls;
  std::string msg;
  DecodeError(const char *cls_, std::string msg_)
      : cls(cls_), msg(std::move(msg_)) {}
};

[[noreturn]] static void fail(const char *cls, const std::string &msg) {
  throw DecodeError(cls, msg);
}

[[noreturn]] static void fail_oom(const std::string &what, uint64_t bytes) {
  throw DecodeError(EXC_OOM, what + " (" + std::to_string(bytes) + " bytes)");
}

static void throw_decode_error(JNIEnv *env, const char *cls_name,
                               const char *msg) {
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }

  jclass cls = env->FindClass(cls_name);
  if (!cls) {
    return;
  }
  env->ThrowNew(cls, msg && *msg ? msg : "Image decode failed");
  env->DeleteLocalRef(cls);
}

static jclass find_class_checked(JNIEnv *env, const char *name) {
  jclass cls = env->FindClass(name);
  if (!cls || env->ExceptionCheck()) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    fail(EXC_DECODE, std::string("Class not found: ") + name);
  }
  return cls;
}

static jmethodID get_method_checked(JNIEnv *env, jclass cls, const char *name,
                                    const char *sig) {
  jmethodID m = env->GetMethodID(cls, name, sig);
  if (!m || env->ExceptionCheck()) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    fail(EXC_DECODE, std::string("Method not found: ") + name + sig);
  }
  return m;
}

static jfieldID get_field_checked(JNIEnv *env, jclass cls, const char *name,
                                  const char *sig) {
  jfieldID f = env->GetFieldID(cls, name, sig);
  if (!f || env->ExceptionCheck()) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    fail(EXC_DECODE, std::string("Field not found: ") + name + sig);
  }
  return f;
}

struct DecoderHandle {
  std::unique_ptr<BaseDecoder> decoder;
  std::vector<uint8_t> sniff;
  size_t total = 0;
  // Repacked partial frame; its Frame views it.
  std::vector<uint8_t> partial;

  void push(const uint8_t *data, size_t size) {
    if (total + size > MAX_INPUT_BYTES) {
      fail_oom("Image is too large to buffer", (uint64_t)total + size);
    }

    if (decoder) {
      decoder->push_data(data, size);
      total += size;
      return;
    }

    sniff.insert(sniff.end(), data, data + size);
    total += size;
    if (sniff.size() < imagedecoder::SNIFF_BYTES) {
      return;
    }

    // Kept local until it holds the sniffed bytes, so a throw loses none.
    std::unique_ptr<BaseDecoder> made =
        imagedecoder::create_decoder(sniff.data(), sniff.size());
    if (!made) {
      fail(EXC_UNKNOWN_FORMAT, "Not a supported image format");
    }
    made->push_data(sniff.data(), sniff.size());
    decoder = std::move(made);
    sniff.clear();
    sniff.shrink_to_fit();
  }

  void mark_complete() {
    if (!decoder) {
      fail(EXC_UNKNOWN_FORMAT, "Not a supported image: too short");
    }
    decoder->mark_complete();
  }
};

// The classes are final, so the object's own class is the named one.
static jlong ptr_of(JNIEnv *env, jobject self, const char *) {
  LocalRef<jclass> cls(env, env->GetObjectClass(self));
  return env->GetLongField(self, get_field_checked(env, cls, "ptr", "J"));
}

static void clear_ptr(JNIEnv *env, jobject self, const char *) {
  LocalRef<jclass> cls(env, env->GetObjectClass(self));
  env->SetLongField(self, get_field_checked(env, cls, "ptr", "J"), (jlong)0);
}

static void read_stream_into(JNIEnv *env, jobject stream, DecoderHandle *handle,
                             bool complete) {
  if (!stream) {
    fail(EXC_DECODE, "No input stream given");
  }

  jmethodID read;
  jmethodID available;
  {
    LocalRef<jclass> cls(env, find_class_checked(env, "java/io/InputStream"));
    read = get_method_checked(env, cls, "read", "([BII)I");
    available = get_method_checked(env, cls, "available", "()I");
  }

  const jint CHUNK = 1 << 16;
  LocalRef<jbyteArray> buf(env, env->NewByteArray(CHUNK));
  if (!buf || env->ExceptionCheck()) {
    fail_oom("Read buffer", (uint64_t)CHUNK);
  }

  std::vector<uint8_t> chunk(CHUNK);
  int empty_reads = 0;
  try {
    for (;;) {
      jint n = env->CallIntMethod(stream, read, (jbyteArray)buf, 0, CHUNK);
      if (env->ExceptionCheck()) {
        throw DecodeError(nullptr, "");
      }
      if (n < 0) {
        break;
      }
      if (n == 0) {
        if (++empty_reads > 64) {
          fail(EXC_DECODE, "Input stream is not producing data");
        }
        continue;
      }
      empty_reads = 0;
      if (n > CHUNK) {
        fail(EXC_DECODE, "Input stream read more than it was asked for");
      }

      env->GetByteArrayRegion(buf, 0, n, (jbyte *)chunk.data());
      if (env->ExceptionCheck()) {
        throw DecodeError(nullptr, "");
      }
      handle->push(chunk.data(), (size_t)n);

      // Stop at what's available, but only past the header: available() is
      // often 0.
      if (!complete && handle->decoder &&
          !handle->decoder->supports_streaming()) {
        fail(EXC_DECODE,
             handle->decoder->get_name() + " needs the whole file: use open()");
      }
      if (!complete && handle->decoder && handle->decoder->read_header()) {
        const jint more = env->CallIntMethod(stream, available);
        if (env->ExceptionCheck()) {
          throw DecodeError(nullptr, "");
        }
        if (more <= 0) {
          break;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    fail_oom("Image is too large to buffer", (uint64_t)handle->total);
  }

  if (complete) {
    handle->mark_complete();
    return;
  }
  if (!handle->decoder) {
    fail(EXC_UNKNOWN_FORMAT, "Not a supported image: too short");
  }
}

// sRGB for display, except HDR, where tone mapping is the caller's.
static DecodeOptions kotlin_options(BaseDecoder *decoder) {
  DecodeOptions options;
  options.srgb_output = true;
  options.apply_orientation = true;
  const imagedecoder::HdrKind hdr = decoder->hdr_kind();
  options.output_mode = (hdr == imagedecoder::HdrKind::None ||
                         hdr == imagedecoder::HdrKind::Gainmap)
                            ? imagedecoder::OutputMode::Rgba8
                            : imagedecoder::OutputMode::RgbaF16;
  return options;
}

static bool kotlin_is_hdr(BaseDecoder *decoder) {
  return kotlin_options(decoder).output_mode ==
         imagedecoder::OutputMode::RgbaF16;
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeNew(JNIEnv *env, jclass,
                                                  jobject stream,
                                                  jboolean complete) {
  std::unique_ptr<DecoderHandle> handle;

  try {
    handle.reset(new DecoderHandle());
    read_stream_into(env, stream, handle.get(), complete != JNI_FALSE);

    if (!handle->decoder->read_header()) {
      fail(EXC_DECODE, "Truncated image");
    }

    // Kotlin always asks for RGBA, which is always turned.
    const ImageInfo &info = handle->decoder->info;
    uint32_t width = info.original_width;
    uint32_t height = info.original_height;
    if (imagedecoder::orientation_swaps_axes(info.orientation)) {
      std::swap(width, height);
    }
    if (width == 0 || height == 0) {
      fail(EXC_DECODE, "Image has no pixels");
    }
    const uint64_t pixels_bytes =
        (uint64_t)width * height *
        (kotlin_is_hdr(handle->decoder.get()) ? 8 : 4);
    if ((uint64_t)width * height > MAX_PIXELS ||
        pixels_bytes > (uint64_t)MAX_OUTPUT_BYTES) {
      fail_oom("Image is too large", pixels_bytes);
    }

    const std::string name = handle->decoder->get_name();
    LocalRef<jstring> format(env, env->NewStringUTF(name.c_str()));
    if (!format || env->ExceptionCheck()) {
      fail(EXC_DECODE, "Failed to allocate format name");
    }

    LocalRef<jclass> cls(env, find_class_checked(env, CLS_DECODER));
    jmethodID ctor =
        get_method_checked(env, cls, "<init>", "(JZIFLjava/lang/String;II)V");

    const imagedecoder::HdrKind hdr = handle->decoder->hdr_kind();
    const jboolean is_hdr =
        kotlin_is_hdr(handle->decoder.get()) ? JNI_TRUE : JNI_FALSE;
    const float headroom = is_hdr ? handle->decoder->hdr_headroom() : 0.0f;

    jobject obj = env->NewObject(cls, ctor, (jlong)(intptr_t)handle.get(),
                                 is_hdr, (jint)hdr, (jfloat)headroom,
                                 (jstring)format, (jint)width, (jint)height);
    if (obj) {
      handle.release();
    }
    if (!obj || env->ExceptionCheck()) {
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      fail(EXC_DECODE, "Failed to construct ImageDecoder");
    }

    return obj;
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
    return nullptr;
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory reading image");
    return nullptr;
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
    return nullptr;
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Image decode failed");
    return nullptr;
  }
}

// Java-owned, so no ByteBuffer can outlive its memory.
static jobject java_buffer(JNIEnv *env, const uint8_t *data, size_t size) {
  if (size > (size_t)INT32_MAX) {
    fail_oom("Buffer is too large", (uint64_t)size);
  }
  LocalRef<jclass> cls(env, find_class_checked(env, "java/nio/ByteBuffer"));
  jmethodID alloc =
      env->GetStaticMethodID(cls, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
  if (!alloc || env->ExceptionCheck()) {
    env->ExceptionClear();
    fail(EXC_DECODE, "ByteBuffer.allocateDirect not found");
  }
  jobject buffer = env->CallStaticObjectMethod(cls, alloc, (jint)size);
  if (!buffer || env->ExceptionCheck()) {
    env->ExceptionClear();
    fail_oom("Out of memory for pixels", (uint64_t)size);
  }
  void *dst = env->GetDirectBufferAddress(buffer);
  if (!dst) {
    env->DeleteLocalRef(buffer);
    fail(EXC_DECODE, "Direct buffer has no address");
  }
  if (size) {
    memcpy(dst, data, size);
  }
  return buffer;
}

static jobject make_gainmap(JNIEnv *env, DecoderHandle *handle,
                            uint32_t orientation);

// A whole frame's pixels, freed by Frame.close(). Native: outside the Java heap
// limit.
struct FramePixels {
  std::vector<uint8_t> data;
};

// Wraps, doesn't copy.
static jobject native_buffer(JNIEnv *env, uint8_t *data, size_t size) {
  if (size > (size_t)INT32_MAX) {
    fail_oom("Buffer is too large", (uint64_t)size);
  }
  jobject buffer = env->NewDirectByteBuffer(data, (jlong)size);
  if (!buffer || env->ExceptionCheck()) {
    env->ExceptionClear();
    fail(EXC_DECODE, "Failed to wrap pixels");
  }
  return buffer;
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeDecode(JNIEnv *env,
                                                     jobject self) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    if (!handle || !handle->decoder) {
      fail(EXC_DECODE, "ImageDecoder has been closed");
    }

    const DecodeOptions options = kotlin_options(handle->decoder.get());

    /* To the next whole frame, or to where the bytes run out. Steps between
     * are never copied out; only the state the last one leaves is. */
    DecodeProgress progress;
    DecodeProgress step;
    bool changed = false;
    imagedecoder::DirtyRegion dirty;
    for (;;) {
      progress = handle->decoder->decode(options);
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

    LocalRef<jclass> cls(env, find_class_checked(env, CLS_RESULT));
    jmethodID ctor = get_method_checked(env, cls, "<init>",
                                        "(Ljava/nio/ByteBuffer;JIIZZZIIIIII)V");

    ImageInfo shown = at.info;
    if (changed && shown.components != 4) {
      fail(EXC_DECODE, "Image has no RGBA layout");
    }
    const uint32_t orientation =
        options.apply_orientation ? shown.orientation : 1;
    imagedecoder::InvalidRect rect = dirty.rect();

    const std::vector<uint8_t> &now = handle->decoder->buffer();
    if (now.size() > (size_t)MAX_OUTPUT_BYTES) {
      fail_oom("Decoded image is too large", (uint64_t)now.size());
    }

    LocalRef<jobject> buffer(env, nullptr);
    std::unique_ptr<FramePixels> owned;
    if (whole && !now.empty()) {
      owned.reset(new FramePixels());
      if (progress.complete) {
        owned->data = handle->decoder->take_buffer();
      } else {
        // The next frame composites over it.
        owned->data = now;
      }
      buffer.ref = native_buffer(env, owned->data.data(), owned->data.size());
      rect = {0, 0, shown.width, shown.height};
    } else if (changed && at.stride > 0 && shown.original_height > 0 &&
               (uint64_t)now.size() >=
                   (uint64_t)at.stride * (shown.original_height - 1) +
                       imagedecoder::row_bytes(shown)) {
      // A view, valid until the next decode.
      const bool turned = orientation >= 2 && orientation <= 8;
      const size_t row = imagedecoder::row_bytes(shown);
      if (!turned && at.stride == row) {
        buffer.ref = native_buffer(env, const_cast<uint8_t *>(now.data()),
                                   row * shown.original_height);
      } else {
        handle->partial = imagedecoder::orient_copy(now.data(), at.stride,
                                                    shown, orientation);
        buffer.ref =
            native_buffer(env, handle->partial.data(), handle->partial.size());
      }
      if (turned) {
        if (imagedecoder::orientation_swaps_axes(orientation)) {
          std::swap(shown.width, shown.height);
        }
        rect = {0, 0, shown.width, shown.height};
      }
    }

    // A Frame always has pixels: none yet means waiting, or no frame at all.
    if (!buffer) {
      if (progress.complete) {
        fail(EXC_DECODE, "No frames remain");
      }
      fail(EXC_NEED_DATA, "More data is needed before anything can be shown");
    }

    jobject obj =
        env->NewObject(cls, ctor, (jobject)buffer, (jlong)(intptr_t)owned.get(),
                       (jint)shown.width, (jint)shown.height,
                       (jboolean)(changed ? JNI_TRUE : JNI_FALSE),
                       (jboolean)(whole ? JNI_FALSE : JNI_TRUE),
                       (jboolean)(progress.complete ? JNI_TRUE : JNI_FALSE),
                       (jint)rect.x, (jint)rect.y, (jint)rect.width,
                       (jint)rect.height, (jint)at.frame, (jint)at.duration_ms);
    if (!obj || env->ExceptionCheck()) {
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      fail(EXC_DECODE, "Failed to construct Frame");
    }
    owned.release(); // The Frame frees it.

    return obj;
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
    return nullptr;
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory decoding image");
    return nullptr;
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
    return nullptr;
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Image decode failed");
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeFreePixels(JNIEnv *, jclass,
                                                         jlong pixels) {
  delete (FramePixels *)(intptr_t)pixels;
}

extern "C" JNIEXPORT jint JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeFrameCount(JNIEnv *env,
                                                         jobject self) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck() || !handle || !handle->decoder) {
      return 1;
    }
    return (jint)handle->decoder->frame_count();
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
    return 1;
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Frame count failed");
    return 1;
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeGainmap(JNIEnv *env,
                                                      jobject self) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    if (!handle || !handle->decoder) {
      fail(EXC_DECODE, "ImageDecoder has been closed");
    }
    if (!handle->decoder->source_complete()) {
      fail(EXC_DECODE, "The image is not complete yet");
    }
    if (!handle->decoder->read_header()) {
      fail(EXC_DECODE, "Truncated image");
    }
    // Turned like the frames, which always apply the orientation.
    return make_gainmap(env, handle, handle->decoder->info.orientation);
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
    return nullptr;
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory decoding the gain map");
    return nullptr;
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
    return nullptr;
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Gain map decode failed");
    return nullptr;
  }
}

// On failure returns null with the exception left pending.
static jobjectArray string_array(JNIEnv *env,
                                 const std::vector<std::string> &names) {
  jclass string_cls = find_class_checked(env, "java/lang/String");
  jobjectArray array =
      env->NewObjectArray((jsize)names.size(), string_cls, nullptr);
  env->DeleteLocalRef(string_cls);
  if (!array || env->ExceptionCheck()) {
    return nullptr;
  }

  for (jsize i = 0; i < (jsize)names.size(); i++) {
    jstring name = env->NewStringUTF(names[(size_t)i].c_str());
    if (!name || env->ExceptionCheck()) {
      env->DeleteLocalRef(array);
      return nullptr;
    }
    env->SetObjectArrayElement(array, i, name);
    env->DeleteLocalRef(name);
  }
  return array;
}

// Null if none or unusable; only running out of memory throws.
static jobject make_gainmap(JNIEnv *env, DecoderHandle *handle,
                            uint32_t orientation) {
  const imagedecoder::GainmapData *map = handle->decoder->gainmap();
  if (!map || map->empty()) {
    return nullptr;
  }

  const uint64_t need = (uint64_t)map->width * map->height * map->channels;
  if (need == 0 || need > MAX_OUTPUT_BYTES || map->pixels.size() < need) {
    return nullptr;
  }

  // Turned with the base image, or every pixel would pick up the wrong gain.
  ImageInfo layout;
  layout.original_width = map->width;
  layout.original_height = map->height;
  layout.components = map->channels;
  layout.bits = 8;
  uint32_t map_w = map->width;
  uint32_t map_h = map->height;
  if (orientation >= 2 && orientation <= 8 &&
      imagedecoder::orientation_swaps_axes(orientation)) {
    std::swap(map_w, map_h);
  }
  // Out of memory here reaches the caller as OutOfMemoryException.
  jobject pixels = nullptr;
  {
    const std::vector<uint8_t> turned = imagedecoder::orient_copy(
        map->pixels.data(), (size_t)map->width * map->channels, layout,
        orientation);
    pixels = java_buffer(env, turned.data(), turned.size());
  }

  jfloatArray sets[5] = {};
  jclass cls = nullptr;

  auto give_up = [&]() -> jobject {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    for (jfloatArray a : sets) {
      if (a) {
        env->DeleteLocalRef(a);
      }
    }
    if (pixels) {
      env->DeleteLocalRef(pixels);
    }
    if (cls) {
      env->DeleteLocalRef(cls);
    }
    return nullptr;
  };

  const float *const from[5] = {map->gamma, map->min_content_boost,
                                map->max_content_boost, map->offset_sdr,
                                map->offset_hdr};
  for (int i = 0; i < 5; i++) {
    sets[i] = env->NewFloatArray(3);
    if (!sets[i] || env->ExceptionCheck()) {
      return give_up();
    }
    env->SetFloatArrayRegion(sets[i], 0, 3, from[i]);
    if (env->ExceptionCheck()) {
      return give_up();
    }
  }

  cls = env->FindClass(CLS_GAINMAP);
  if (!cls || env->ExceptionCheck()) {
    return give_up();
  }
  jmethodID ctor = env->GetMethodID(cls, "<init>",
                                    "(Ljava/nio/ByteBuffer;III[F[F[F[F[FFF)V");
  if (!ctor || env->ExceptionCheck()) {
    return give_up();
  }

  jobject obj = env->NewObject(cls, ctor, pixels, (jint)map_w, (jint)map_h,
                               (jint)map->channels, sets[0], sets[1], sets[2],
                               sets[3], sets[4], (jfloat)map->base_headroom,
                               (jfloat)map->alternate_headroom);
  if (!obj || env->ExceptionCheck()) {
    return give_up();
  }

  for (jfloatArray a : sets) {
    env->DeleteLocalRef(a);
  }
  env->DeleteLocalRef(pixels);
  env->DeleteLocalRef(cls);
  return obj;
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeListTags(JNIEnv *env,
                                                       jobject self) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    if (!handle || !handle->decoder) {
      fail(EXC_DECODE, "ImageDecoder has been closed");
    }

    std::vector<uint8_t> exif = handle->decoder->exif_data();
    std::vector<std::string> names =
        imagedecoder::exif_list_tags(exif.data(), exif.size());

    jobjectArray array = string_array(env, names);
    if (!array) {
      fail_oom("Tag list", (uint64_t)names.size());
    }
    return array;
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
    return nullptr;
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory reading tags");
    return nullptr;
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
    return nullptr;
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Image decode failed");
    return nullptr;
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeGetTag(JNIEnv *env, jobject self,
                                                     jstring name) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
    if (!handle || !handle->decoder) {
      fail(EXC_DECODE, "ImageDecoder has been closed");
    }
    if (!name) {
      return nullptr;
    }

    struct Utf8 {
      JNIEnv *env;
      jstring str;
      const char *chars;
      ~Utf8() {
        if (chars) {
          env->ReleaseStringUTFChars(str, chars);
        }
      }
    } wanted{env, name, env->GetStringUTFChars(name, nullptr)};
    if (!wanted.chars || env->ExceptionCheck()) {
      env->ExceptionClear();
      fail_oom("Tag name", 0);
    }

    std::vector<uint8_t> exif = handle->decoder->exif_data();
    std::string value;
    bool found = imagedecoder::exif_get_tag(exif.data(), exif.size(),
                                            wanted.chars, &value);

    return found ? env->NewStringUTF(value.c_str()) : nullptr;
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
    return nullptr;
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory reading tag");
    return nullptr;
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
    return nullptr;
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Image decode failed");
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativePushData(
    JNIEnv *env, jobject self, jbyteArray data, jint offset, jint length) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      return;
    }
    if (!handle) {
      fail(EXC_DECODE, "ImageDecoder has been closed");
    }
    if (!data) {
      fail(EXC_DECODE, "No data given");
    }

    const jsize have = env->GetArrayLength(data);
    if (offset < 0 || length < 0 || offset > have || length > have - offset) {
      fail(EXC_DECODE, "Data range is outside the array");
    }
    if (length == 0) {
      return;
    }

    if (handle->total + (size_t)length > MAX_INPUT_BYTES) {
      fail_oom("Image is too large to buffer",
               (uint64_t)handle->total + (uint64_t)length);
    }

    const jint WINDOW = 1 << 16;
    std::vector<uint8_t> chunk((size_t)std::min(length, WINDOW));
    for (jint at = 0; at < length;) {
      const jint n = std::min(WINDOW, length - at);
      env->GetByteArrayRegion(data, offset + at, n, (jbyte *)chunk.data());
      if (env->ExceptionCheck()) {
        return;
      }
      handle->push(chunk.data(), (size_t)n);
      at += n;
    }
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory buffering image");
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Image decode failed");
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeMarkComplete(JNIEnv *env,
                                                           jobject self) {
  try {
    auto *handle = (DecoderHandle *)(intptr_t)ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      return;
    }
    if (!handle) {
      fail(EXC_DECODE, "ImageDecoder has been closed");
    }
    handle->mark_complete();
  } catch (const DecodeError &e) {
    if (e.cls) {
      throw_decode_error(env, e.cls, e.msg.c_str());
    }
  } catch (const std::bad_alloc &) {
    throw_decode_error(env, EXC_OOM, "Out of memory reading image");
  } catch (const std::exception &e) {
    throw_decode_error(env, EXC_DECODE, e.what());
  } catch (...) {
    throw_decode_error(env, EXC_DECODE, "Image decode failed");
  }
}

extern "C" JNIEXPORT void JNICALL
Java_ca_mpreg_imagedecoder_ImageDecoder_nativeFree(JNIEnv *env, jobject self) {
  try {
    jlong p = ptr_of(env, self, CLS_DECODER);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      return;
    }
    clear_ptr(env, self, CLS_DECODER);
    delete (DecoderHandle *)(intptr_t)p;
  } catch (...) {
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
  }
}
