#pragma once

/* HDR detection and transfer-function conversion.
 *
 * libvips loads the pixels but drops the colour signalling: heifload ignores nclx, and PNG's
 * cICP never reaches us. So it is read out of the container here and undone in the pixel loop.
 *
 * Gainmaps are the exception - uhdrload carries those itself. */

#include <cmath>
#include <cstring>
#include <jxl/decode.h>
#include <stdint.h>

/* Mirrored by ImageDecoder.HdrKind. */
enum HdrKind
{
  HDR_NONE = 0,
  HDR_GAINMAP = 1,
  HDR_PQ = 2,
  HDR_HLG = 3,
  HDR_LINEAR = 4,
};

/* Mirrored by ImageDecoder.PixelFormat. */
enum PixelFormat
{
  PIXFMT_RGBA8 = 0,
  PIXFMT_RGBA16F = 1,
};

/* ITU-T H.273 code points. */
enum
{
  CICP_PRIMARIES_BT709 = 1,
  CICP_PRIMARIES_BT2020 = 9,
  CICP_PRIMARIES_P3 = 12,
  CICP_TRANSFER_BT709 = 1,
  CICP_TRANSFER_LINEAR = 8,
  CICP_TRANSFER_SRGB = 13,
  CICP_TRANSFER_PQ = 16,
  CICP_TRANSFER_HLG = 18,
};

struct ColourSignal
{
  int primaries;
  int transfer;
};

/* What 1.0 means in nits - BT.2408 diffuse white. Both EOTFs normalise against it, putting a
 * 1000-nit highlight at 4.93 and PQ's peak at 49.3.
 *
 * 100 is the tidier arithmetic but renders diffuse white about twice as bright as the SDR pages
 * beside it. Changing this needs nothing else changed: the headroom asked of the display derives
 * from it, so the two cannot disagree. */
static const float HDR_SDR_WHITE_NITS = 203.0f;

/* ---------------------------------------------------------------- detection */

static inline uint16_t
hdr_be16(const uint8_t* p)
{
  return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t
hdr_be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* An nclx colr box in an ISOBMFF file.
 *
 * Scanned for rather than walked to: the box sits at the end of a path that differs per brand,
 * and the payload validates itself. The first match wins, which is the primary
 * item's in every file that has one.
 */
static inline bool
hdr_find_isobmff_nclx(const uint8_t* buf, size_t size, ColourSignal* out)
{
  if (size < 19)
    return false;

  /* Only scan what could hold one. Every ISOBMFF file opens with an ftyp box, whose
   * type sits at offset 4 - so this both skips the scan for a JPEG or a TIFF and
   * keeps it from ever being a full pass over a file that can't match. */
  if (memcmp(buf + 4, "ftyp", 4) != 0)
    return false;

  for (size_t i = 0; i + 19 <= size; i++) {
    if (memcmp(buf + i, "colr", 4) != 0 || memcmp(buf + i + 4, "nclx", 4) != 0)
      continue;

    int primaries = hdr_be16(buf + i + 8);
    int transfer = hdr_be16(buf + i + 10);

    /* H.273 leaves 0 and 2 unspecified and assigns nothing above 22 - a match
     * failing this is a false positive, not a real box. */
    if (primaries < 1 || primaries > 22 || transfer < 1 || transfer > 22)
      continue;

    out->primaries = primaries;
    out->transfer = transfer;
    return true;
  }

  return false;
}

/* PNG's cICP chunk carries the same code points in one byte each. Walked properly,
 * since PNG's chunk layout makes that trivial and its length field is a far stronger
 * check than a scan would be. */
static inline bool
hdr_find_png_cicp(const uint8_t* buf, size_t size, ColourSignal* out)
{
  static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

  if (size < 8 || memcmp(buf, signature, 8) != 0)
    return false;

  /* Chunks are length(4) type(4) data(length) crc(4), from just past the
   * signature. */
  size_t p = 8;
  while (p + 12 <= size) {
    uint32_t length = hdr_be32(buf + p);
    const uint8_t* type = buf + p + 4;

    /* A length running past the buffer means a truncated or misparsed file -
     * stop rather than resync on whatever follows. */
    if (length > size - p - 12)
      return false;

    if (memcmp(type, "cICP", 4) == 0 && length == 4) {
      const uint8_t* data = buf + p + 8;
      out->primaries = data[0];
      out->transfer = data[1];
      return true;
    }

    /* No cICP can follow the image data. */
    if (memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0)
      return false;

    p += 12 + length;
  }

  return false;
}

/* The colour signalling for [buf], if a container we can read carries it. */
/* JXL keeps transfer and primaries in its own metadata, which libvips passes on only as a
 * synthesised ICC blob - so a 10-bit PQ file arrives looking like ordinary RGB16 and clips.
 * libjxl's enum values are the CICP ones bar P3, which JXL numbers 11 against CICP's 12.
 *
 * Enum profiles only: an ICC-only JXL has no code point, and inferring a transfer from a
 * sampled tone curve would be worse than treating it as SDR. */
static inline bool
hdr_find_jxl_signal(const uint8_t* buf, size_t size, ColourSignal* out, float* peak_nits)
{
  if (!buf || size == 0)
    return false;

  const JxlSignature sig = JxlSignatureCheck(buf, size);
  if (sig != JXL_SIG_CODESTREAM && sig != JXL_SIG_CONTAINER)
    return false;

  JxlDecoder* dec = JxlDecoderCreate(nullptr);
  if (!dec)
    return false;

  bool found = false;

  if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) ==
        JXL_DEC_SUCCESS &&
      JxlDecoderSetInput(dec, buf, size) == JXL_DEC_SUCCESS) {
    /* Closed now, or the decoder waits for pixel data that never comes. */
    JxlDecoderCloseInput(dec);

    for (;;) {
      JxlDecoderStatus status = JxlDecoderProcessInput(dec);

      if (status == JXL_DEC_BASIC_INFO) {
        JxlBasicInfo info;
        if (JxlDecoderGetBasicInfo(dec, &info) == JXL_DEC_SUCCESS && info.intensity_target > 0.0f)
          *peak_nits = info.intensity_target;
        continue;
      }

      if (status == JXL_DEC_COLOR_ENCODING) {
        JxlColorEncoding colour;
        /* TARGET_DATA describes the pixels libvips hands back, which is what the transfer
         * loop undoes; it matches ORIGINAL only because jxlload sets no preferred profile. */
        if (JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, &colour) ==
            JXL_DEC_SUCCESS) {
          out->transfer = (int)colour.transfer_function;
          out->primaries =
            colour.primaries == JXL_PRIMARIES_P3 ? CICP_PRIMARIES_P3 : (int)colour.primaries;
          found = true;
        }
        break;
      }

      /* An error, more input wanted, or events exhausted: no enum profile coming. */
      break;
    }
  }

  JxlDecoderDestroy(dec);
  return found;
}

static inline bool
hdr_find_colour_signal(const uint8_t* buf, size_t size, ColourSignal* out, float* peak_nits)
{
  /* Before the nclx byte scan, which a container-format JXL could false-positive with a stray
   * "colr" "nclx" pair. Signature-gated, so it cannot false-positive in return. */
  return hdr_find_png_cicp(buf, size, out) || hdr_find_jxl_signal(buf, size, out, peak_nits) ||
         hdr_find_isobmff_nclx(buf, size, out);
}

/* Which HdrKind a transfer characteristic implies. */
static inline int
hdr_kind_for_transfer(int transfer)
{
  switch (transfer) {
    case CICP_TRANSFER_PQ:
      return HDR_PQ;
    case CICP_TRANSFER_HLG:
      return HDR_HLG;
    default:
      return HDR_NONE;
  }
}

/* ------------------------------------------------------- transfer functions */

/* Mirrored through zero, so it holds for the negatives an out-of-gamut colour lands on. This is
 * what Android's extended-sRGB dataspace expects and what every PIXFMT_RGBA16F buffer holds. */
static inline float
hdr_srgb_encode(float x)
{
  float a = fabsf(x);
  float y = a <= 0.0031308f ? a * 12.92f : 1.055f * powf(a, 1.0f / 2.4f) - 0.055f;
  return x < 0.0f ? -y : y;
}

/* Mirrored the same way. */
static inline float
hdr_srgb_decode(float x)
{
  float a = fabsf(x);
  float y = a <= 0.04045f ? a / 12.92f : powf((a + 0.055f) / 1.055f, 2.4f);
  return x < 0.0f ? -y : y;
}

/* Normalised so 1.0 is HDR_SDR_WHITE_NITS, not the format's 10000-nit peak. */
static inline float
hdr_pq_eotf(float e)
{
  const float m1 = 0.1593017578125f;
  const float m2 = 78.84375f;
  const float c1 = 0.8359375f;
  const float c2 = 18.8515625f;
  const float c3 = 18.6875f;
  const float scale = 10000.0f / HDR_SDR_WHITE_NITS;

  if (e <= 0.0f)
    return 0.0f;

  float p = powf(e, 1.0f / m2);
  float num = p - c1;
  if (num <= 0.0f)
    return 0.0f;

  float den = c2 - c3 * p;
  if (den <= 0.0f)
    return scale;

  return powf(num / den, 1.0f / m1) * scale;
}

/* Scene light in [0, 1]. The OOTF needs all three channels, so it is applied separately. */
static inline float
hdr_hlg_inverse_oetf(float e)
{
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;

  if (e <= 0.0f)
    return 0.0f;
  if (e <= 0.5f)
    return e * e / 3.0f;

  return (expf((e - c) / a) + b) / 12.0f;
}

/* Peak signal lands on 1000/HDR_SDR_WHITE_NITS, its 75% reference white on 1.0. */
static const float HDR_HLG_PEAK_NITS = 1000.0f;
static const float HDR_HLG_SYSTEM_GAMMA = 1.2f;

/* For the HLG OOTF. */
static const float HDR_BT2020_LUMA[3] = { 0.2627f, 0.6780f, 0.0593f };

/* Scene light in, diffuse-white-relative display light out. */
static inline void
hdr_hlg_ootf(float* rgb)
{
  float luma =
    HDR_BT2020_LUMA[0] * rgb[0] + HDR_BT2020_LUMA[1] * rgb[1] + HDR_BT2020_LUMA[2] * rgb[2];

  float gain = luma > 0.0f ? powf(luma, HDR_HLG_SYSTEM_GAMMA - 1.0f) : 0.0f;
  gain *= HDR_HLG_PEAK_NITS / HDR_SDR_WHITE_NITS;

  rgb[0] *= gain;
  rgb[1] *= gain;
  rgb[2] *= gain;
}

/* ----------------------------------------------------------- gamut matrices */

/* Wide-gamut colours land outside [0, 1] - what the mirrored sRGB encode above is for. */
static const float HDR_BT2020_TO_SRGB[9] = {
  1.660491f,  -0.587641f, -0.072850f, //
  -0.124551f, 1.132900f,  -0.008349f, //
  -0.018151f, -0.100579f, 1.118730f,  //
};

static const float HDR_P3_TO_SRGB[9] = {
  1.224940f,  -0.224940f, 0.000000f, //
  -0.042056f, 1.042056f,  0.000000f, //
  -0.019637f, -0.078636f, 1.098273f, //
};

/* The matrix taking [primaries] to sRGB, or null when they already are sRGB's (or are
 * something we have no matrix for, and so leave alone). */
static inline const float*
hdr_matrix_to_srgb(int primaries)
{
  switch (primaries) {
    case CICP_PRIMARIES_BT2020:
      return HDR_BT2020_TO_SRGB;
    case CICP_PRIMARIES_P3:
      return HDR_P3_TO_SRGB;
    default:
      return nullptr;
  }
}

static inline void
hdr_apply_matrix(const float* m, float* rgb)
{
  float r = m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2];
  float g = m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2];
  float b = m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2];
  rgb[0] = r;
  rgb[1] = g;
  rgb[2] = b;
}

/* ------------------------------------------------------------- half float */

/* IEEE 754 binary16, written by hand rather than via __fp16 so the x86 ABIs build the
 * same way as the ARM ones. The subnormal and overflow ends are handled because an
 * HDR highlight genuinely can reach binary16's limit once scaled against diffuse
 * white. */
static inline uint16_t
hdr_float_to_half(float f)
{
  uint32_t bits;
  memcpy(&bits, &f, 4);

  uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127;
  uint32_t mantissa = bits & 0x7fffffu;

  /* NaN and infinity keep their class; a NaN payload of zero would read back as
   * infinity, so force a bit on. */
  if (exponent == 128)
    return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));

  /* Past binary16's range - saturate rather than go infinite, so a blend
   * downstream stays a number. */
  if (exponent > 15)
    return (uint16_t)(sign | 0x7bffu);

  /* Subnormal, or too small to represent at all. */
  if (exponent < -14) {
    if (exponent < -25)
      return (uint16_t)sign;
    uint32_t shift = (uint32_t)(-exponent - 14);
    uint32_t full = mantissa | 0x800000u;
    uint32_t sub = full >> (shift + 13);
    uint32_t round = (full >> (shift + 12)) & 1u;
    return (uint16_t)(sign | (sub + round));
  }

  uint32_t half = (uint32_t)((exponent + 15) << 10) | (mantissa >> 13);
  /* Round to nearest on the dropped mantissa bits. Carrying into the exponent is
   * exactly what we want, and falls out of the addition. */
  if (mantissa & 0x1000u)
    half++;

  return (uint16_t)(sign | half);
}

/* A corrupt scRGB source can hand over NaN or infinity, hdr_float_to_half preserves both by
 * design, and a NaN in a sampled texture is undefined on the GPU. Stopped here, at the one place
 * every HDR pixel passes through. */
static inline uint16_t
hdr_encode_half(float v)
{
  return hdr_float_to_half(isfinite(v) ? hdr_srgb_encode(v) : 0.0f);
}

/* Alpha is a coverage fraction, not light: clamped rather than encoded, opaque if unusable. */
static inline uint16_t
hdr_encode_alpha(float a)
{
  if (!isfinite(a))
    return hdr_float_to_half(1.0f);
  return hdr_float_to_half(a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a));
}
