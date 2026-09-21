#pragma once

// Target is Android's extended-sRGB dataspace (RGBA16F).

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace imagedecoder {

enum : int {
  CICP_PRIMARIES_BT709 = 1,
  CICP_PRIMARIES_BT2020 = 9,
  CICP_PRIMARIES_P3 = 12,
};

inline constexpr float HDR_SDR_WHITE_NITS = 203.0f;

inline float hdr_srgb_encode(float x) {
  float a = fabsf(x);
  float y =
      a <= 0.0031308f ? a * 12.92f : 1.055f * powf(a, 1.0f / 2.4f) - 0.055f;
  return x < 0.0f ? -y : y;
}

inline float hdr_srgb_decode(float x) {
  float a = fabsf(x);
  float y = a <= 0.04045f ? a / 12.92f : powf((a + 0.055f) / 1.055f, 2.4f);
  return x < 0.0f ? -y : y;
}

// Normalised so 1.0 is diffuse white, not the format's 10000-nit peak.
inline float hdr_pq_eotf(float e) {
  const float m1 = 0.1593017578125f;
  const float m2 = 78.84375f;
  const float c1 = 0.8359375f;
  const float c2 = 18.8515625f;
  const float c3 = 18.6875f;
  const float scale = 10000.0f / HDR_SDR_WHITE_NITS;

  if (e <= 0.0f) {
    return 0.0f;
  }

  float p = powf(e, 1.0f / m2);
  float num = p - c1;
  if (num <= 0.0f) {
    return 0.0f;
  }

  float den = c2 - c3 * p;
  if (den <= 0.0f) {
    return scale;
  }

  return powf(num / den, 1.0f / m1) * scale;
}

inline float hdr_hlg_inverse_oetf(float e) {
  const float a = 0.17883277f;
  const float b = 0.28466892f;
  const float c = 0.55991073f;

  if (e <= 0.0f) {
    return 0.0f;
  }
  if (e <= 0.5f) {
    return e * e / 3.0f;
  }

  return (expf((e - c) / a) + b) / 12.0f;
}

inline constexpr float HDR_HLG_PEAK_NITS = 1000.0f;
inline constexpr float HDR_HLG_SYSTEM_GAMMA = 1.2f;

inline constexpr float HDR_BT2020_LUMA[3] = {0.2627f, 0.6780f, 0.0593f};

inline void hdr_hlg_ootf(float *rgb) {
  float luma = HDR_BT2020_LUMA[0] * rgb[0] + HDR_BT2020_LUMA[1] * rgb[1] +
               HDR_BT2020_LUMA[2] * rgb[2];

  float gain = luma > 0.0f ? powf(luma, HDR_HLG_SYSTEM_GAMMA - 1.0f) : 0.0f;
  gain *= HDR_HLG_PEAK_NITS / HDR_SDR_WHITE_NITS;

  rgb[0] *= gain;
  rgb[1] *= gain;
  rgb[2] *= gain;
}

inline constexpr float HDR_BT2020_TO_SRGB[9] = {
    1.660491f,  -0.587641f, -0.072850f, //
    -0.124551f, 1.132900f,  -0.008349f, //
    -0.018151f, -0.100579f, 1.118730f,  //
};

inline constexpr float HDR_P3_TO_SRGB[9] = {
    1.224940f,  -0.224940f, 0.000000f, //
    -0.042056f, 1.042056f,  0.000000f, //
    -0.019637f, -0.078636f, 1.098273f, //
};

inline const float *hdr_matrix_to_srgb(int primaries) {
  switch (primaries) {
  case CICP_PRIMARIES_BT2020:
    return HDR_BT2020_TO_SRGB;
  case CICP_PRIMARIES_P3:
    return HDR_P3_TO_SRGB;
  default:
    return nullptr;
  }
}

// By hand rather than __fp16, so x86 ABIs build the same way as ARM.
inline uint16_t hdr_float_to_half(float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);

  uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127;
  uint32_t mantissa = bits & 0x7fffffu;

  if (exponent == 128) {
    return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
  }

  // Saturate rather than go infinite, so a downstream blend stays a number.
  if (exponent > 15) {
    return (uint16_t)(sign | 0x7bffu);
  }

  if (exponent < -14) { // Subnormal, or too small to represent at all.
    if (exponent < -25) {
      return (uint16_t)sign;
    }
    uint32_t shift = (uint32_t)(-exponent - 14);
    uint32_t full = mantissa | 0x800000u;
    uint32_t sub = full >> (shift + 13);
    uint32_t round = (full >> (shift + 12)) & 1u;
    return (uint16_t)(sign | (sub + round));
  }

  uint32_t half = (uint32_t)((exponent + 15) << 10) | (mantissa >> 13);
  if (mantissa & 0x1000u) {
    half++;
  }
  // Rounding can carry into the exponent; the largest finite value is kept.
  if (half >= 0x7c00u) {
    half = 0x7bffu;
  }

  return (uint16_t)(sign | half);
}

// NaN is stopped here rather than passed to the GPU.
inline uint16_t hdr_encode_half(float v) {
  return hdr_float_to_half(isfinite(v) ? hdr_srgb_encode(v) : 0.0f);
}

// Alpha is clamped, not encoded; opaque if not finite.
inline uint16_t hdr_encode_alpha(float a) {
  if (!isfinite(a)) {
    return hdr_float_to_half(1.0f);
  }
  return hdr_float_to_half(a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a));
}

} // namespace imagedecoder
