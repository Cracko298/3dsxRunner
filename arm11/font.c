#include "sup.h"

typedef struct {
  s8 left, top;
  u8 w, h, adv, pad;
  u16 ax, ay;
} __attribute__((packed)) AGlyph;

typedef struct {
  u16 first, count;
  s16 ascent, descent, line_height;
  u16 atlas_w, atlas_h, pad;
} AHead;

#define LINE       30
#define SHEET_W    256
#define SHEET_H    512
#define SHEET_SIZE (SHEET_W * SHEET_H)
#define MAX_CELL_W 40
#define PUA_FIRST  0xE000u
#define PUA_COUNT  7u

static void w8(u8 *p, u32 off, u32 v) { p[off] = (u8)v; }
static void w16(u8 *p, u32 off, u32 v) { p[off] = (u8)v, p[off + 1] = (u8)(v >> 8); }
static void w32(u8 *p, u32 off, u32 v) {
  p[off] = (u8)v, p[off + 1] = (u8)(v >> 8), p[off + 2] = (u8)(v >> 16), p[off + 3] = (u8)(v >> 24);
}

static const AHead *head;
static const AGlyph *glyphs;
static const u8 *atlas;
static u32 scale;
static s64 inv;

static s32 sc(s32 v) {
  s64 x = (s64)v * scale;
  return (s32)((x >= 0 ? x + 0x8000 : x - 0x8000) / 0x10000);
}

static u32 src_at(const AGlyph *g, s32 x, s32 y) {
  if (x < 0 || y < 0 || x >= g->w || y >= g->h)
    return 0;
  return atlas[(u32)(g->ay + y) * head->atlas_w + g->ax + (u32)x];
}

static u32 sample(const AGlyph *g, s32 dx, s32 dy) {
  s32 sx = (s32)(((s64)dx * 0x10000 + 0x8000) * inv >> 16) - 0x8000;
  s32 sy = (s32)(((s64)dy * 0x10000 + 0x8000) * inv >> 16) - 0x8000;
  s32 x0 = sx >> 16, y0 = sy >> 16;
  u32 fx = (u32)sx & 0xFFFFu, fy = (u32)sy & 0xFFFFu;
  u32 a = src_at(g, x0, y0), b = src_at(g, x0 + 1, y0), c = src_at(g, x0, y0 + 1), d = src_at(g, x0 + 1, y0 + 1);
  u32 top = a * (0x10000u - fx) + b * fx, bot = c * (0x10000u - fx) + d * fx;
  u64 v = ((u64)top * (0x10000u - fy) + (u64)bot * fy) >> 32;
  return v > 255 ? 255 : (u32)v;
}

static u8 *sheets;
static u32 cell_w, cell_h, per_row, per_line;

static u8 *cell_byte(u32 glyph, s32 x, s32 y) {
  if (x < 0 || y < 0 || (u32)x >= cell_w || (u32)y >= cell_h)
    return NULL;
  u32 per_sheet = per_row * per_line, in = glyph % per_sheet;
  u8 *sheet = sheets + (glyph / per_sheet) * SHEET_SIZE;
  u32 px = (in % per_row) * (cell_w + 1) + 1 + (u32)x;
  u32 py = (in / per_row) * (cell_h + 1) + 1 + (u32)y;
  u32 m = (px & 1) | (py & 1) << 1 | (px & 2) << 1 | (py & 2) << 2 | (px & 4) << 2 | (py & 4) << 3;
  return sheet + ((py >> 3) * (SHEET_W >> 3) + (px >> 3)) * 64 + m;
}

static void put(u32 glyph, s32 x, s32 y, u32 a) {
  u8 *b = cell_byte(glyph, x, y);
  if (b && a > *b)
    *b = (u8)a;
}

static void draw_scaled(u32 index, const AGlyph *g, s32 ox, s32 oy, bool knock_out) {
  s32 w = sc(g->w), h = sc(g->h);
  for (s32 y = 0; y < h; y++)
    for (s32 x = 0; x < w; x++) {
      u32 a = sample(g, x, y);
      if (!knock_out) {
        put(index, ox + x, oy + y, a);
        continue;
      }
      u8 *b = cell_byte(index, ox + x, oy + y);
      if (b)
        *b = (u8)(*b * (255u - a) / 255u);
    }
}

static const AGlyph *glyph_for(u32 cp) {
  if (cp < head->first || cp >= (u32)head->first + head->count)
    return NULL;
  return &glyphs[cp - head->first];
}

static void draw_badge(u32 index, s32 size, s32 top, bool cross) {
  s32 c = size * 4, r = size * 4, arm = r / 3;
  for (s32 y = 0; y < size; y++)
    for (s32 x = 0; x < size; x++) {
      u32 hits = 0;
      for (s32 sy = 0; sy < 4; sy++)
        for (s32 sx = 0; sx < 4; sx++) {
          s32 qx = x * 8 + sx * 2 + 1 - c, qy = y * 8 + sy * 2 + 1 - c;
          if (cross)
            hits += (qx > -arm && qx < arm && qy > -r && qy < r) || (qy > -arm && qy < arm && qx > -r && qx < r);
          else
            hits += qx * qx + qy * qy < r * r;
        }
      put(index, x, top + y, hits * 255u / 16u);
    }
}

static const char badge_letters[PUA_COUNT] = {'A', 'B', 'X', 'Y', 'L', 'R', 0};

u32 font_build(u8 *dst, u32 dst_va, u32 cap, const u32 source[3]) {
  head = (const AHead *)(uintptr_t)source[0];
  glyphs = (const AGlyph *)(uintptr_t)source[1];
  atlas = (const u8 *)(uintptr_t)source[2];
  if (!head->count || head->count > 1024 || head->line_height <= 0 || !head->atlas_w)
    return 0;
  scale = (u32)(((u32)LINE << 16) / (u32)head->line_height);
  inv = ((s64)1 << 32) / scale;

  u32 n_src = head->count, n = n_src + PUA_COUNT;
  s32 ascent = sc(head->ascent);
  u32 max_w = 1, max_adv = 1;
  for (u32 i = 0; i < n_src; i++) {
    u32 w = (u32)sc(glyphs[i].w), adv = (u32)sc(glyphs[i].adv);
    if (w > max_w) max_w = w;
    if (adv > max_adv) max_adv = adv;
  }
  s32 badge = ascent * 9 / 10;
  if ((u32)badge + 1 > max_w) max_w = (u32)badge + 1;
  cell_w = max_w > MAX_CELL_W ? MAX_CELL_W : max_w;
  cell_h = LINE;
  per_row = (SHEET_W - 1) / (cell_w + 1);
  per_line = (SHEET_H - 1) / (cell_h + 1);
  u32 n_sheets = (n + per_row * per_line - 1) / (per_row * per_line);

  u32 cfnt = 0x80, finf = cfnt + 0x14, tglp = finf + 0x20, cwdh = tglp + 8 + 0x18;
  u32 cwdh_end = cwdh + 8 + 8 + n * 3;
  u32 cmap1 = (cwdh_end + 3) & ~3u, cmap2 = cmap1 + 8 + 0x10;
  u32 end_hdr = cmap2 + 8 + 0x10;
  u32 sheet_off = 0x1000;
  u32 total = sheet_off + n_sheets * SHEET_SIZE;
  if (end_hdr > sheet_off || total > cap)
    return 0;
  memset(dst, 0, total);
  sheets = dst + sheet_off;

  w32(dst, 0, 2);
  u8 *p = dst;
  memcpy(p + cfnt, "CFNU", 4);
  w16(p, cfnt + 4, 0xFEFF);
  w16(p, cfnt + 6, 0x14);
  w32(p, cfnt + 8, 0x03000000);
  w32(p, cfnt + 12, total - cfnt);
  w32(p, cfnt + 16, 5);
  u32 question = '?' >= head->first && '?' < head->first + head->count ? '?' - head->first : 0;
  memcpy(p + finf, "FINF", 4);
  w32(p, finf + 4, 0x20);
  w8(p, finf + 8, 1);
  w8(p, finf + 9, LINE);
  w16(p, finf + 10, question);
  w8(p, finf + 12, 0);
  w8(p, finf + 13, (u32)sc(glyphs[question].w));
  w8(p, finf + 14, (u32)sc(glyphs[question].adv));
  w8(p, finf + 15, 1);
  w32(p, finf + 16, dst_va + tglp + 8);
  w32(p, finf + 20, dst_va + cwdh + 8);
  w32(p, finf + 24, dst_va + cmap1 + 8);
  w8(p, finf + 28, LINE);
  w8(p, finf + 29, max_adv);
  w8(p, finf + 30, (u32)ascent);
  memcpy(p + tglp, "TGLP", 4);
  w32(p, tglp + 4, 0x20);
  w8(p, tglp + 8, cell_w);
  w8(p, tglp + 9, cell_h);
  w8(p, tglp + 10, (u32)ascent);
  w8(p, tglp + 11, max_adv);
  w32(p, tglp + 12, SHEET_SIZE);
  w16(p, tglp + 16, n_sheets);
  w16(p, tglp + 18, 0x8);
  w16(p, tglp + 20, per_row);
  w16(p, tglp + 22, per_line);
  w16(p, tglp + 24, SHEET_W);
  w16(p, tglp + 26, SHEET_H);
  w32(p, tglp + 28, dst_va + sheet_off);
  memcpy(p + cwdh, "CWDH", 4);
  w32(p, cwdh + 4, cwdh_end - cwdh);
  w16(p, cwdh + 8, 0);
  w16(p, cwdh + 10, n - 1);
  w32(p, cwdh + 12, 0);
  for (u32 i = 0; i < n; i++) {
    u32 o = cwdh + 16 + i * 3;
    if (i < n_src) {
      const AGlyph *g = &glyphs[i];
      u32 w = (u32)sc(g->w);
      w8(p, o, (u32)(u8)(s8)sc(g->left));
      w8(p, o + 1, w > cell_w ? cell_w : w);
      w8(p, o + 2, (u32)sc(g->adv));
    } else {
      w8(p, o, 1);
      w8(p, o + 1, (u32)badge + 1);
      w8(p, o + 2, (u32)badge + 3);
    }
  }
  memcpy(p + cmap1, "CMAP", 4);
  w32(p, cmap1 + 4, 0x18);
  w16(p, cmap1 + 8, head->first);
  w16(p, cmap1 + 10, head->first + head->count - 1);
  w16(p, cmap1 + 12, 0);
  w32(p, cmap1 + 16, dst_va + cmap2 + 8);
  w16(p, cmap1 + 20, 0);
  memcpy(p + cmap2, "CMAP", 4);
  w32(p, cmap2 + 4, 0x18);
  w16(p, cmap2 + 8, PUA_FIRST);
  w16(p, cmap2 + 10, PUA_FIRST + PUA_COUNT - 1);
  w16(p, cmap2 + 12, 0);
  w32(p, cmap2 + 16, 0);
  w16(p, cmap2 + 20, n_src);

  for (u32 i = 0; i < n_src; i++) {
    const AGlyph *g = &glyphs[i];
    if (g->w && g->h)
      draw_scaled(i, g, 0, ascent - sc(g->top), false);
  }
  for (u32 k = 0; k < PUA_COUNT; k++) {
    u32 index = n_src + k;
    s32 top = ascent - badge;
    draw_badge(index, badge, top, badge_letters[k] == 0);
    const AGlyph *g = badge_letters[k] ? glyph_for((u32)badge_letters[k]) : NULL;
    if (g && g->w) {
      s32 gw = sc(g->w), gh = sc(g->h);
      draw_scaled(index, g, (badge - gw) / 2, top + (badge - gh) / 2, true);
    }
  }
  log_printf("font: Aurora's, %u glyphs in %u sheet%s (cells %ux%u)\n", n, n_sheets, n_sheets == 1 ? "" : "s",
             cell_w, cell_h);
  return total;
}
