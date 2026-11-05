#ifndef X3_FRANGES_H
#define X3_FRANGES_H

#include <stdbool.h>
#include <stdint.h>

#ifndef FR_CMP_EQ
#define FR_CMP_EQ 0u
#define FR_CMP_NE 1u
#define FR_CMP_GT 2u
#define FR_CMP_LT 3u
#endif

typedef struct {
  uint32_t lo[2], hi[2];
  int n;
  bool invert;
} Ranges;

static inline uint32_t bits_of(float f) {
  union { float f; uint32_t u; } x = {f};
  return x.u;
}

static inline void float_span(Ranges *r, float a, float b) {
  r->n = 0;
  if (a >= 0.0f) {
    r->lo[0] = bits_of(a), r->hi[0] = bits_of(b), r->n = 1;
  } else if (b <= 0.0f) {
    r->lo[0] = bits_of(b) | 0x80000000u, r->hi[0] = bits_of(a), r->n = 1;
    if (b == 0.0f)
      r->lo[0] = 0x80000000u;
  } else {
    r->lo[0] = 0, r->hi[0] = bits_of(b);
    r->lo[1] = 0x80000000u, r->hi[1] = bits_of(a), r->n = 2;
  }
}

static inline bool float_ranges(Ranges *r, uint32_t cmp, uint32_t value) {
  union { uint32_t u; float f; } vv = {value};
  float v = vv.f;
  r->invert = false;
  if (v != v)
    return false;
  float tol = v < 0 ? -v * 0.00001f : v * 0.00001f;
  if (tol < 0.001f)
    tol = 0.001f;
  switch (cmp) {
  case FR_CMP_EQ:
  case FR_CMP_NE:
    float_span(r, v - tol, v + tol);
    r->invert = cmp == FR_CMP_NE;
    return true;
  case FR_CMP_GT:
    if (v >= 0.0f) {
      r->lo[0] = (bits_of(v) & 0x7FFFFFFFu) + 1u, r->hi[0] = 0x7F800000u, r->n = 1;
    } else {
      r->lo[0] = 0x80000000u, r->hi[0] = bits_of(v) - 1u;
      r->lo[1] = 0, r->hi[1] = 0x7F800000u, r->n = 2;
    }
    return true;
  case FR_CMP_LT:
    if (v > 0.0f) {
      r->lo[0] = 0, r->hi[0] = bits_of(v) - 1u;
      r->lo[1] = 0x80000000u, r->hi[1] = 0xFF800000u, r->n = 2;
    } else {
      r->lo[0] = (bits_of(v) | 0x80000000u) + 1u, r->hi[0] = 0xFF800000u, r->n = 1;
      if (v == 0.0f) r->lo[0] = 0x80000001u;
    }
    return true;
  }
  return false;
}

static inline bool in_ranges(const Ranges *r, uint32_t b) {
  bool in = (b >= r->lo[0] && b <= r->hi[0]) || (r->n > 1 && b >= r->lo[1] && b <= r->hi[1]);
  return in != r->invert;
}

#endif
