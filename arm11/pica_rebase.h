#ifndef X3_PICA_REBASE_H
#define X3_PICA_REBASE_H

#include <stdint.h>

#define PICA_REBASE_TARGET 0x28000000u
#define PICA_REG_ATTR_LOC  0x200u
#define PICA_REG_ATTR_OFF0 0x203u
#define PICA_REG_ATTR_OFFN 0x224u
#define PICA_REG_INDEX_CFG 0x227u
#define PICA_LOC_ADDR_MASK 0x1FFFFFFEu

typedef struct {
  uint32_t delta;
  uint32_t guest_base;
  uint32_t bases, offsets, indices;
  uint32_t below;
  uint32_t partial;
  uint32_t lists;
} PicaRebase;

static inline int pica_is_attr_offset(uint32_t r) {
  return r >= PICA_REG_ATTR_OFF0 && r <= PICA_REG_ATTR_OFFN && (r - PICA_REG_ATTR_OFF0) % 3u == 0;
}

static inline void pica_rebase_word(PicaRebase *s, uint32_t reg, uint32_t mask, uint32_t *v) {
  if (reg == PICA_REG_ATTR_LOC) {
    if (mask != 0xFu) {
      s->partial++;
      s->delta = 0;
      return;
    }
    uint32_t base = (*v & PICA_LOC_ADDR_MASK) << 3;
    s->guest_base = base;
    if (base < PICA_REBASE_TARGET) {
      s->delta = PICA_REBASE_TARGET - base;
      *v = (*v & ~PICA_LOC_ADDR_MASK) | (PICA_REBASE_TARGET >> 3);
      s->bases++;
    } else {
      s->delta = 0;
    }
    return;
  }
  if (!s->delta)
    return;
  if (pica_is_attr_offset(reg)) {
    if (mask != 0xFu) { s->partial++; return; }
    if (*v >= s->delta) { *v -= s->delta; s->offsets++; }
    else if (*v) s->below++;
  } else if (reg == PICA_REG_INDEX_CFG) {
    if (mask != 0xFu) { s->partial++; return; }
    uint32_t off = *v & 0x7FFFFFFFu;
    if (off >= s->delta) { *v = (*v & 0x80000000u) | (off - s->delta); s->indices++; }
    else s->below++;
  }
}

static inline void pica_rebase_list(PicaRebase *s, uint32_t *w, uint32_t words) {
  s->lists++;
  for (uint32_t i = 0; i + 1 < words;) {
    uint32_t header = w[i + 1];
    uint32_t extra = (header >> 20) & 0xFFu, reg = header & 0x3FFu, mask = (header >> 16) & 0xFu;
    int seq = (int)(header >> 31);
    if (i + 2 + extra > words)
      break;
    uint32_t last = seq ? reg + extra : reg;
    if (last >= PICA_REG_ATTR_LOC && reg <= PICA_REG_INDEX_CFG) {
      for (uint32_t k = 0; k <= extra; k++) {
        uint32_t r = seq ? reg + k : reg;
        if (r < PICA_REG_ATTR_LOC || r > PICA_REG_INDEX_CFG)
          continue;
        pica_rebase_word(s, r, mask, k ? &w[i + 1 + k] : &w[i]);
      }
    }
    i += (2 + extra + 1) & ~1u;
  }
}

#endif
