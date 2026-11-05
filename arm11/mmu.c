#include "sup.h"

#define L1_SECTION 0x2u
#define L1_COARSE  0x1u
#define L2_SMALL   0x2u

/* Page tables, ARMv6 format. 1 MB sections where we can, 4 KB pages where we
 * must. Table walks don't go through the cache, so every change is cleaned
 * out before the TLB flush, or the MMU reads yesterday's tables. */
static u32 l1[4096] __attribute__((aligned(16384)));
#define L2_TABLES 48
static u32 l2[L2_TABLES][256] __attribute__((aligned(1024)));
static u8 l2_used[L2_TABLES];

#define POOL_PAGES 160
static u8 pool[POOL_PAGES][4096] __attribute__((aligned(4096)));
static u8 pool_used[POOL_PAGES];

static u32 saved_sctlr, saved_ttbr0, saved_ttbr1, saved_ttbcr, saved_dacr, saved_tid0, saved_tid1;

u32 sup_pa(const void *p) { return (u32)(uintptr_t)p; }

void *page_alloc(void) {
  for (int i = 0; i < POOL_PAGES; i++)
    if (!pool_used[i]) {
      pool_used[i] = 1;
      memset(pool[i], 0, 4096);
      return pool[i];
    }
  return NULL;
}

void page_free(void *p) {
  for (int i = 0; i < POOL_PAGES; i++)
    if (pool[i] == p)
      pool_used[i] = 0;
}

void cache_clean_all(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 0\n\t"
                   "mcr p15, 0, %0, c7, c10, 4"
                   ::"r"(0)
                   : "memory");
}

void tlb_flush(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4\n\t"
                   "mcr p15, 0, %0, c8, c7, 0\n\t"
                   "mcr p15, 0, %0, c7, c5, 6\n\t"
                   "mcr p15, 0, %0, c7, c10, 4\n\t"
                   "mcr p15, 0, %0, c7, c5, 4"
                   ::"r"(0)
                   : "memory");
}

static u32 section_bits(u32 f) {
  u32 ap = (f & MAP_USER) ? ((f & MAP_RO) ? 2u : 3u) : 1u;
  u32 b = ap << 10;
  if (f & MAP_DEVICE)
    b |= (1u << 2) | (1u << 4);
  else if (f & MAP_UNCACHED)
    b |= (1u << 12);
  else
    b |= (1u << 12) | (1u << 3) | (1u << 2);
  return b;
}

static u32 page_bits(u32 f) {
  u32 ap = (f & MAP_USER) ? ((f & MAP_RO) ? 2u : 3u) : 1u;
  u32 b = ap << 4;
  if (f & MAP_DEVICE)
    b |= (1u << 2) | 1u;
  else if (f & MAP_UNCACHED)
    b |= (1u << 6);
  else
    b |= (1u << 6) | (1u << 3) | (1u << 2);
  return b;
}

static u32 *l2_for(u32 va) {
  u32 *e = &l1[va >> 20];
  if ((*e & 3u) == L1_COARSE)
    return (u32 *)(uintptr_t)(*e & ~0x3FFu);
  for (int i = 0; i < L2_TABLES; i++)
    if (!l2_used[i]) {
      l2_used[i] = 1;
      memset(l2[i], 0, sizeof l2[i]);
      if ((*e & 3u) == L1_SECTION) {
        u32 base = *e & 0xFFF00000u, bits = *e;
        u32 f = 0;
        u32 ap = (bits >> 10) & 3u;
        if (ap == 3u) f |= MAP_USER;
        if (ap == 2u) f |= MAP_USER | MAP_RO;
        if (bits & (1u << 4)) f |= MAP_DEVICE;
        else if (!(bits & (1u << 3))) f |= MAP_UNCACHED;
        for (int k = 0; k < 256; k++)
          l2[i][k] = (base + ((u32)k << 12)) | page_bits(f) | L2_SMALL;
      }
      *e = sup_pa(l2[i]) | L1_COARSE;
      return l2[i];
    }
  return NULL;
}

bool mmu_map(u32 va, u32 pa, u32 size, u32 flags) {
  if ((va | pa | size) & 0xFFFu)
    return false;
  while (size) {
    if (!((va | pa) & 0xFFFFFu) && size >= 0x100000u && (l1[va >> 20] & 3u) != L1_COARSE) {
      l1[va >> 20] = pa | section_bits(flags) | L1_SECTION;
      va += 0x100000u, pa += 0x100000u, size -= 0x100000u;
      continue;
    }
    u32 *t = l2_for(va);
    if (!t)
      return false;
    t[(va >> 12) & 0xFFu] = pa | page_bits(flags) | L2_SMALL;
    va += 0x1000u, pa += 0x1000u, size -= 0x1000u;
  }
  cache_clean_all();
  tlb_flush();
  return true;
}

void mmu_unmap(u32 va, u32 size) {
  while (size) {
    u32 *e = &l1[va >> 20];
    if ((*e & 3u) == L1_SECTION && !(va & 0xFFFFFu) && size >= 0x100000u) {
      *e = 0;
      va += 0x100000u, size -= 0x100000u;
      continue;
    }
    if ((*e & 3u) == L1_COARSE) {
      u32 *t = (u32 *)(uintptr_t)(*e & ~0x3FFu);
      t[(va >> 12) & 0xFFu] = 0;
    } else if ((*e & 3u) == L1_SECTION) {
      u32 *t = l2_for(va);
      if (t)
        t[(va >> 12) & 0xFFu] = 0;
    }
    va += 0x1000u, size = size > 0x1000u ? size - 0x1000u : 0;
  }
  cache_clean_all();
  tlb_flush();
}

/* Horizon allows unaligned single-word loads, and homebrew relies on it,
 * which is why mmu_enter clears the alignment check bit. */
static u32 lookup(u32 va, bool *is_page) {
  u32 e = l1[va >> 20];
  *is_page = false;
  if ((e & 3u) == L1_SECTION)
    return e;
  if ((e & 3u) == L1_COARSE) {
    u32 *t = (u32 *)(uintptr_t)(e & ~0x3FFu);
    *is_page = true;
    return t[(va >> 12) & 0xFFu];
  }
  return 0;
}

u32 mmu_pa(u32 va) {
  bool page;
  u32 d = lookup(va, &page);
  if (!d)
    return 0;
  if (page)
    return (d & 2u) ? (d & 0xFFFFF000u) | (va & 0xFFFu) : 0;
  return (d & 0xFFF00000u) | (va & 0xFFFFFu);
}

bool mmu_user_ok(u32 va, u32 size) {
  if (!size)
    return true;
  u32 end = va + size;
  if (end < va)
    return false;
  for (u32 a = va & ~0xFFFu; a < end; a += 0x1000u) {
    bool page;
    u32 d = lookup(a, &page);
    u32 ap = page ? (d >> 4) & 3u : (d >> 10) & 3u;
    if (!d || ap < 2u)
      return false;
    if (a + 0x1000u < a)
      break;
  }
  return true;
}

bool mmu_user_write_ok(u32 va,u32 size) {
  if (!mmu_user_ok(va,size)) return false;
  u32 end=va+size;
  for (u32 a=va&~0xFFFu; size && a<end; a+=0x1000u) {
    bool page; u32 d=lookup(a,&page);
    u32 ap=page?(d>>4)&3u:(d>>10)&3u;
    u32 apx=page?(d>>9)&1u:(d>>15)&1u;
    if (ap!=3u || apx) return false;
    if (a+0x1000u<a) break;
  }
  return true;
}

void mmu_reset(void) {
  memset(l1, 0, sizeof l1);
  memset(l2_used, 0, sizeof l2_used);
  memset(pool_used, 0, sizeof pool_used);
}

void mmu_enter(void) {
  u32 sctlr;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
  __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(saved_ttbr0));
  __asm__ volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(saved_ttbr1));
  __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(saved_ttbcr));
  __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(saved_dacr));
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(saved_tid0));
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(saved_tid1));
  saved_sctlr = sctlr;
  if (!(l1[JOB_CODE >> 20] & 3u))
    return;
  cache_clean_all();
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(0));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"(sup_pa(l1)));
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(0x55555555u));
  sctlr |= (1u << 0) | (1u << 13) | (1u << 22) | (1u << 23);
  sctlr &= ~(1u << 1);
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(sctlr));
  tlb_flush();
}

void mmu_tables_on(void) { mmu_enter(); }

void mmu_leave(void) {
  cache_clean_all();
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(saved_sctlr));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"(saved_ttbr0));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 1" ::"r"(saved_ttbr1));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(saved_ttbcr));
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(saved_dacr));
  __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" ::"r"(saved_tid0));
  __asm__ volatile("mcr p15, 0, %0, c13, c0, 3" ::"r"(saved_tid1));
  tlb_flush();
}

bool mmu_base(void) {
  extern const u32 vector_table[], vector_table_end[];
  if (!mmu_map(JOB_PARAMS & ~0xFFFFFu, JOB_PARAMS & ~0xFFFFFu,
               ((SUP_END - (JOB_PARAMS & ~0xFFFFFu)) + 0xFFFFFu) & ~0xFFFFFu, MAP_PRIV))
    return false;
  u8 *vec = page_alloc();
  if (!vec)
    return false;
  memcpy(vec, vector_table, (size_t)((const u8 *)vector_table_end - (const u8 *)vector_table));
  if (!mmu_map(VA_VECTORS, sup_pa(vec), 0x1000u, MAP_PRIV))
    return false;
  if (!mmu_map(VA_IO, PA_IO, IO_SIZE, MAP_PRIV | MAP_DEVICE))
    return false;
  return mmu_map(PA_MPCORE, PA_MPCORE, 0x100000u, MAP_PRIV | MAP_DEVICE);
}
