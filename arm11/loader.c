#include "sup.h"

typedef struct {
  u32 magic;
  u16 header_size, reloc_hdr_size;
  u32 format_ver, flags;
  u32 code_size, rodata_size, data_size, bss_size;
} Hdr3dsx;

#define STACK_SIZE 0x10000u
#define VA_STACK_BASE (VA_STACK_TOP - STACK_SIZE)

static bool fail(const char *why) {
  snprintf(P->error, sizeof P->error, "%s", why);
  return false;
}

static void ctru_split(u32 remaining, u32 *heap, u32 *linear) {
  u32 lin = (remaining / 2) & ~0xFFFu, hp = remaining - lin;
  if (hp > 24u << 20) {
    hp = 24u << 20;
    lin = remaining - hp;
    if (lin > 32u << 20) {
      lin = 32u << 20;
      hp = remaining - lin;
    }
  }
  if (hp > 0x08000000u)
    lin += hp - 0x08000000u, hp = 0x08000000u;
  *heap = hp, *linear = lin;
}

static u32 choose_total(u32 h, u32 l) {
  u32 best = 0;
  for (u32 r = (h + l) & ~0xFFFu; r >= 0x200000u; r -= 0x10000u) {
    u32 hp, lin;
    ctru_split(r, &hp, &lin);
    if (hp <= h && lin <= l) {
      best = r;
      break;
    }
  }
  return best;
}

u32 image_seg_va[3], image_seg_size[3];

static u32 translate(u32 off, const u32 seg_va[3], const u32 seg_size[3]) {
  if (off < seg_size[0])
    return seg_va[0] + off;
  if (off < seg_size[0] + seg_size[1])
    return seg_va[1] + off - seg_size[0];
  return seg_va[2] + off - seg_size[0] - seg_size[1];
}

static void *map_page(u32 va, u32 flags) {
  void *p = page_alloc();
  if (p && !mmu_map(va, sup_pa(p), 0x1000u, flags))
    return NULL;
  return p;
}

static void config_pages(void) {
  u8 *cfg = map_page(VA_CONFIG, MAP_USER | MAP_RO);
  u8 *shr = map_page(VA_SHARED, MAP_USER);
  if (!cfg || !shr)
    return;
  u32 *c = (u32 *)(void *)cfg;
  c[0x00 / 4] = 0x023A0000u;
  c[0x08 / 4] = 0x00008002u;
  c[0x0C / 4] = 0x00040130u;
  c[0x10 / 4] = 2;
  cfg[0x14] = 1;
  c[0x18 / 4] = 0x0000F297u;
  c[0x30 / 4] = P->new3ds ? 6u : 0u;
  c[0x40 / 4] = heap_size + linear_size;
  c[0x44 / 4] = 0x02C00000u;
  c[0x48 / 4] = 0x01400000u;
  c[0x60 / 4] = 0x023A0000u;
  c[0x64 / 4] = 2;
  c[0x68 / 4] = 0x0000F297u;

  u32 *s = (u32 *)(void *)shr;
  s[0] = 1;
  shr[4] = 1;
  shr[5] = P->new3ds ? 1 : 0;
  u64 ms_1900 = P->unix_ms + 2208988800000ull;
  for (int i = 0; i < 2; i++) {
    u64 *tr = (u64 *)(void *)(shr + 0x20 + i * 0x20);
    tr[0] = ms_1900;
    tr[1] = 0;
    tr[2] = 268111856ull;
    tr[3] = 0;
  }
  shr[0x60] = 0;
  shr[0x61] = 0;
  *(float *)(void *)(shr + 0x80) = 0.0f;
}

void shared_page_update(void) {
  u32 pa = mmu_pa(VA_SHARED);
  if (!pa)
    return;
  u8 *shr = (u8 *)(uintptr_t)VA_SHARED;
  u32 v = P->slider > 256u ? 256u : P->slider;
  *(float *)(void *)(shr + 0x80) = (float)v / 256.0f;
  shr[0x84] = v ? 1 : 0;
}

bool load_3dsx(void) {
  const u8 *file = (const u8 *)(uintptr_t)P->file_pa;
  u32 file_size = P->file_size;
  Hdr3dsx h;
  if (file_size < sizeof h)
    return fail("The file is too short to be a .3dsx.");
  memcpy(&h, file, sizeof h);
  if (h.magic != 0x58534433u)
    return fail("This is not a .3dsx file.");
  if (h.header_size < 0x20 || h.reloc_hdr_size < 8 || h.reloc_hdr_size > 0x40 || (h.reloc_hdr_size & 3u) ||
      h.header_size > file_size || h.code_size < 4 ||
      h.code_size > P->code.size || h.rodata_size > P->code.size || h.data_size > P->code.size ||
      h.data_size < h.bss_size)
    return fail("The .3dsx header is damaged.");

  u32 seg_size[3] = {(h.code_size + 0xFFFu) & ~0xFFFu, (h.rodata_size + 0xFFFu) & ~0xFFFu,
                     (h.data_size + 0xFFFu) & ~0xFFFu};
  u64 required = (u64)seg_size[0] + seg_size[1] + seg_size[2] + 0x1000u;
  if (required + STACK_SIZE + 0x200000u > P->code.size)
    return fail("The program does not fit in the memory there is.");

  u32 image = (u32)required;

  heap_pa = P->code.pa;
  heap_avail = (P->code.size - image - STACK_SIZE) & ~0xFFFu;
  u32 image_pa = P->code.pa + P->code.size - image - STACK_SIZE;
  linear_pa = P->linear.pa;
  linear_avail = P->linear.size;
  linear_va = linear_pa + LINEAR_DELTA;
  font_pa = font_va = 0;
  if (P->font_head_pa && P->font_glyphs_pa && P->font_atlas_pa && linear_avail > 4 * FONT_RESERVE) {
    linear_avail -= FONT_RESERVE;
    /* The system font sits at the very end of the linear heap so the GPU can
     * read it like any other texture. */
    font_pa = linear_pa + linear_avail;
    font_va = font_pa + LINEAR_DELTA;
  }
  u32 total = choose_total(heap_avail, linear_avail);
  if (!total)
    return fail("There is not enough memory for the program's heaps.");
  ctru_split(total, &heap_size, &linear_size);

  if (!mmu_map(VA_CODE, image_pa, image, MAP_USER) ||
      !mmu_map(VA_STACK_BASE, image_pa + image, STACK_SIZE, MAP_USER))
    return fail("The guest's memory could not be mapped.");
  u32 *stub = map_page(VA_STACK, MAP_USER | MAP_RO);
  if (!stub)
    return fail("Out of supervisor pages.");
  /* main() returns here: an svc ExitProcess (and ExitThread for threads). */
  stub[0] = 0xEF000003u;
  stub[1] = 0xEAFFFFFEu;
  stub[2] = 0xEF000009u;
  stub[3] = 0xEAFFFFFEu;
  for (u32 off = 0; off < MAX_THREADS * TLS_SIZE; off += 0x1000u)
    if (!map_page(VA_TLS + off, MAP_USER))
      return fail("Out of supervisor pages.");
  /* VRAM is uncached for the game so its CPU writes actually reach the GPU. */
  if (!mmu_map(VA_VRAM, PA_VRAM, VRAM_SIZE, MAP_USER | MAP_UNCACHED))
    return fail("VRAM could not be mapped.");

  u32 seg_va[3] = {VA_CODE, VA_CODE + seg_size[0], VA_CODE + seg_size[0] + seg_size[1]};
  for(unsigned i=0;i<3;i++) { image_seg_va[i]=seg_va[i]; image_seg_size[i]=seg_size[i]; }
  u8 *seg[3] = {(u8 *)(uintptr_t)seg_va[0], (u8 *)(uintptr_t)seg_va[1], (u8 *)(uintptr_t)seg_va[2]};
  memset(seg[0], 0, image);
  u32 tables = h.reloc_hdr_size / 4;
  u32 rel[3][16];
  u32 off = h.header_size;
  for (int i = 0; i < 3; i++) {
    if (off + h.reloc_hdr_size > file_size)
      return fail("The .3dsx is cut short (relocation headers).");
    memcpy(rel[i], file + off, h.reloc_hdr_size);
    off += h.reloc_hdr_size;
  }
  u32 load[3] = {h.code_size, h.rodata_size, h.data_size - h.bss_size};
  for (int i = 0; i < 3; i++) {
    if (off + load[i] > file_size || off + load[i] < off)
      return fail("The .3dsx is cut short (segments).");
    memcpy(seg[i], file + off, load[i]);
    off += load[i];
  }
  for (int i = 0; i < 3; i++) {
    for (u32 j = 0; j < tables; j++) {
      u32 *pos = (u32 *)(void *)seg[i], *end = pos + seg_size[i] / 4;
      u32 n = rel[i][j];
      if (off > file_size || n > (file_size - off) / 4u)
        return fail("The .3dsx is cut short (relocations).");
      if (j >= 2) {
        if (n) return fail("The .3dsx uses an unsupported relocation table.");
        continue;
      }
      const u16 *r = (const u16 *)(const void *)(file + off);
      off += n * 4u;
      for (u32 k = 0; k < n; k++) {
        u32 skip = r[k * 2], count = r[k * 2 + 1];
        u32 remaining = (u32)(end - pos);
        if (skip > remaining || count > remaining - skip)
          return fail("The .3dsx relocation leaves its segment.");
        pos += skip;
        for (u32 m = count; m; m--, pos++) {
          u32 at = seg_va[i] + (u32)((u8 *)pos - seg[i]);
          u32 orig = *pos, sub = orig >> 28;
          u32 target = orig & 0x0FFFFFFFu;
          if (target > seg_size[0] + seg_size[1] + seg_size[2])
            return fail("The .3dsx relocation target is outside the image.");
          u32 addr = translate(target, seg_va, seg_size);
          if (j == 0) {
            if (sub)
              return fail("The .3dsx has a relocation this loader does not know.");
            *pos = addr;
          } else {
            u32 d = addr - at;
            if (sub == 0)
              *pos = d;
            else if (sub == 1)
              *pos = d & 0x7FFFFFFFu;
            else
              return fail("The .3dsx has a relocation this loader does not know.");
          }
        }
      }
    }
  }

  u8 *args = seg[2] + seg_size[2];
  u32 args_va = seg_va[2] + seg_size[2];
  *(u32 *)(void *)args = P->path[0] && !(P->launch_flags & 1u) ? 1u : 0u;
  strncpy((char *)args + 4, P->path, 0xDF0u);
  args[0xFF8] = 0;
  u32 *services = (u32 *)(void *)(args + 0xE00);
  u32 dsp_handle = dsp_resource();
  services[0] = dsp_handle ? 1 : 0;
  memcpy(services + 1, "hb:ndsp", 8);
  services[3] = dsp_handle;

  u32 *prm = (u32 *)(void *)(seg[0] + 4);
  if (h.code_size >= 32 && prm[0] == 0x6D72705Fu) {
    prm[1] = args_va + 0xE00;
    prm[3] = heap_size;
    prm[4] = linear_size;
    prm[5] = args_va;
    prm[6] |= 1u;
  } else {
    log_printf("no _prm block: not a libctru program?\n");
  }

  config_pages();
  shared_page_update();
  cache_clean_all();
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0) : "memory");

  Thread *t = thread_new(VA_CODE, 0, VA_STACK_TOP, 0x30, 0);
  if (!t)
    return fail("The main thread could not be made.");
  t->ctx.lr = VA_STACK;
  log_printf("loaded: code %uK rodata %uK data %uK; heap %uK, linear %uK\n", h.code_size >> 10,
             h.rodata_size >> 10, h.data_size >> 10, heap_size >> 10, linear_size >> 10);
  return true;
}
