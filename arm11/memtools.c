#include "cheats.h"
#include "sup.h"

#define TOOL_ALIAS 0x1D800000u
#define TOOL_SLOT  0x00400000u
static u32 lent_size[2], n_lent;

/* The memory tools look at game RAM through private, temporary aliases and
 * unmap only those aliases. Same lesson as the font, learned twice. */
static void *lend(u32 pa,u32 size) {
  if(!pa || !size || pa<0x24000000u || (u64)pa+size>0x25000000u || n_lent>=2)
    return NULL;
  u32 offset=pa&0xFFFu, bytes=(offset+size+0xFFFu)&~0xFFFu;
  if(bytes>TOOL_SLOT) return NULL;
  u32 va=TOOL_ALIAS+n_lent*TOOL_SLOT;
  lent_size[n_lent++]=bytes;
  if(!mmu_map(va,pa&~0xFFFu,bytes,MAP_PRIV)) return NULL;
  return (void *)(uintptr_t)(va+offset);
}
static void lend_end(void) {
  for(u32 i=0;i<n_lent;i++) mmu_unmap(TOOL_ALIAS+i*TOOL_SLOT,lent_size[i]);
  n_lent=0;
}
static bool changed;
static u32 fault_mask;

static void code_changed(void) {
  cache_clean_all();
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0\n\t"
                   "mcr p15, 0, %0, c7, c5, 6" ::"r"(0)
                   : "memory");
}

#define CHEAT_WORDS 8192u
static u32 prog[CHEAT_WORDS];
static u32 prog_words;

static bool g_read(u32 va, void *out, u32 n) {
  if (!mmu_user_ok(va, n))
    return false;
  memcpy(out, (const void *)(uintptr_t)va, n);
  return true;
}

static bool g_write(u32 va, const void *in, u32 n) {
  if (!mmu_user_ok(va, n))
    return false;
  if(memcmp((void *)(uintptr_t)va,in,n)) {
    memcpy((void *)(uintptr_t)va,in,n); changed=true;
  }
  return true;
}

void cheats_take(void) {
  prog_words = 0;
  P->cheat_failed = 0; fault_mask=0;
  u32 bytes = P->cheat_bytes;
  if (!P->cheat_pa || bytes < 4) {
    P->mem.result = 0;
    return;
  }
  const void *source=NULL;
  if(bytes>sizeof prog || (bytes&3u) || !(source=lend(P->cheat_pa,bytes))) {
    P->mem.result=(s32)RES_INVALID_ADDR; lend_end(); return;
  }
  memcpy(prog,source,bytes);
  lend_end();
  u32 n=prog[0],at=1,words=bytes/4;
  if(n>32) { P->mem.result=(s32)RES_INVALID_ADDR; return; }
  for(u32 k=0;k<n;k++) {
    if(at>=words || prog[at]>(words-at-1u)/2u) {
      P->mem.result=(s32)RES_INVALID_ADDR; return;
    }
    at+=1u+prog[at]*2u;
  }
  if(at!=words) { P->mem.result=(s32)RES_INVALID_ADDR; return; }
  prog_words = words;
  P->mem.result = 0;
}

void cheats_frame(void) {
  if (!P->cheats_on || prog_words < 1)
    return;
  CheatEnv env = {g_read, g_write, P->held, P->touch_x, P->touch_y, false};
  u32 n = prog[0], at = 1, failed = fault_mask, budget=16384;
  changed=false;
  for (u32 k = 0; k < n && at < prog_words; k++) {
    u32 lines = prog[at];
    if(!(fault_mask&(1u<<k))) {
      if(!budget) failed|=1u<<k;
      else if(cheat_run_budget(&prog[at+1],lines,&env,&budget)) {
        failed|=1u<<k;

        if(!budget) fault_mask|=1u<<k;
      }
    }
    at += 1 + lines * 2;
  }
  P->cheat_failed = failed;
  P->cheat_runs++;
  if (changed)
    code_changed();
}

static u32 width(u32 type) { return type == MT_U8 ? 1u : type == MT_U16 ? 2u : 4u; }

static u32 get(u32 va, u32 type) {
  switch (type) {
  case MT_U8:
    return *(const volatile u8 *)(uintptr_t)va;
  case MT_U16:
    return *(const volatile u16 *)(uintptr_t)va;
  default:
    return *(const volatile u32 *)(uintptr_t)va;
  }
}

static float as_float(u32 bits) {
  union { u32 u; float f; } x = {bits};
  return x.f;
}

static bool near(float a, float b) {
  float d = a - b;
  if (d < 0)
    d = -d;
  float m = b < 0 ? -b : b;
  return d <= 0.001f || d <= m * 0.00001f;
}

static bool match(u32 type, u32 cmp, u32 now, u32 old, u32 value) {
  if (type == MT_FLOAT) {
    float f = as_float(now), o = as_float(old), v = as_float(value);
    if (f != f)
      return cmp == CMP_CHANGED && now != old;
    switch (cmp) {
    case CMP_EQ: return near(f, v);
    case CMP_NE: return !near(f, v);
    case CMP_GT: return f > v;
    case CMP_LT: return f < v;
    case CMP_CHANGED: return now != old;
    case CMP_UNCHANGED: return now == old;
    case CMP_INC: return f > o;
    case CMP_DEC: return f < o;
    case CMP_INC_BY: return near(f, o + v);
    case CMP_DEC_BY: return near(f, o - v);
    }
    return false;
  }
  u32 mask = type == MT_U8 ? 0xFFu : type == MT_U16 ? 0xFFFFu : 0xFFFFFFFFu;
  value &= mask;
  switch (cmp) {
  case CMP_EQ: return now == value;
  case CMP_NE: return now != value;
  case CMP_GT: return now > value;
  case CMP_LT: return now < value;
  case CMP_CHANGED: return now != old;
  case CMP_UNCHANGED: return now == old;
  case CMP_INC: return now > old;
  case CMP_DEC: return now < old;
  case CMP_INC_BY: return now == ((old + value) & mask);
  case CMP_DEC_BY: return now == ((old - value) & mask);
  }
  return false;
}

extern u32 image_seg_va[3], image_seg_size[3];
u32 heap_in_use(void);
u32 linear_in_use(void);

static void regions(JobMem *m) {
  static const char *names[3] = {"code", "rodata", "data"};
  m->n_regions = 0;
  for (int i = 0; i < 3; i++) {
    JobRegionInfo *r = &m->regions[m->n_regions++];
    r->va = image_seg_va[i];
    r->size = image_seg_size[i];
    strncpy(r->name, names[i], sizeof r->name);
  }
  const struct { u32 va, size; const char *name; } more[] = {
      {VA_HEAP, heap_in_use(), "heap"},
      {linear_va, linear_in_use(), "linear heap"},
      {VA_STACK_TOP - 0x10000u, 0x10000u, "first stack"},
  };
  for (unsigned i = 0; i < sizeof more / sizeof more[0]; i++)
    if (more[i].size && m->n_regions < JOB_MEM_REGIONS) {
      JobRegionInfo *r = &m->regions[m->n_regions++];
      r->va = more[i].va;
      r->size = more[i].size;
      strncpy(r->name, more[i].name, sizeof r->name);
    }
}

#include "franges.h"

#define SEARCH_CHUNK 0x00040000u

static void search(JobMem *m) {
  JobHit *hits = (JobHit *)(uintptr_t)m->hits;
  u32 w = width(m->type), count = m->va ? m->count : 0, cap = m->cap, budget = SEARCH_CHUNK;
  u32 mask = w == 1 ? 0xFFu : w == 2 ? 0xFFFFu : 0xFFFFFFFFu, v = m->value & mask;
  Ranges fr = {{0, 0}, {0, 0}, 0, false};
  bool franges = m->type == MT_FLOAT && float_ranges(&fr, m->cmp, m->value);
  u32 resume = m->va, next = 0;
  regions(m);
  for (u32 r = 0; r < m->n_regions && !next; r++) {
    const JobRegionInfo *g = &m->regions[r];
    if (r < 2 && !(m->flags & 1u))
      continue;
    u32 end = g->va + g->size;
    if (resume >= end)
      continue;
    u32 page = (resume > g->va ? resume : g->va) & ~0xFFFu;
    for (; page < end; page += 0x1000u) {
      job_check_cancel();
      if (!budget) {
        next = page;
        break;
      }
      budget -= 0x1000u;
      if (!mmu_user_ok(page, 0x1000u))
        continue;
      u32 lo = page < g->va ? g->va : page, hi = page + 0x1000u > end ? end : page + 0x1000u;
      lo = (lo + w - 1) & ~(w - 1);
#define HIT(a, val)                                    \
  do {                                                 \
    if (count < cap) hits[count].va = (a), hits[count].old = (val); \
    count++;                                           \
  } while (0)
      if (m->cmp == CMP_EQ && m->type != MT_FLOAT) {
        if (w == 4) {
          const u32 *p = (const u32 *)(uintptr_t)lo;
          for (u32 n = (hi - lo) / 4, i = 0; i < n; i++)
            if (p[i] == v) HIT(lo + i * 4, v);
        } else if (w == 2) {
          const u16 *p = (const u16 *)(uintptr_t)lo;
          for (u32 n = (hi - lo) / 2, i = 0; i < n; i++)
            if (p[i] == v) HIT(lo + i * 2, v);
        } else {
          const u8 *p = (const u8 *)(uintptr_t)lo;
          for (u32 n = hi - lo, i = 0; i < n; i++)
            if (p[i] == v) HIT(lo + i, v);
        }
        continue;
      }
      if (franges) {
        const u32 *p = (const u32 *)(uintptr_t)lo;
        for (u32 n = (hi - lo) / 4, i = 0; i < n; i++)
          if (in_ranges(&fr, p[i])) HIT(lo + i * 4, p[i]);
        continue;
      }
      for (u32 a = lo; a + w <= hi; a += w) {
        u32 now = get(a, m->type);
        if (match(m->type, m->cmp, now, now, m->value)) HIT(a, now);
      }
#undef HIT
    }
  }
  m->overflow = count > cap;
  m->count = count;
  m->va = next;
}

static void refine(JobMem *m) {
  JobHit *hits = (JobHit *)(uintptr_t)m->hits;
  u32 w=width(m->type), kept=m->first?m->n:0, n=m->count<m->cap?m->count:m->cap;
  u32 end=m->first+8192u; if(end>n) end=n;
  for (u32 i = m->first; i < end; i++) {
    u32 a = hits[i].va;
    if (!mmu_user_ok(a, w))
      continue;
    u32 now = get(a, m->type);
    if (match(m->type, m->cmp, now, hits[i].old, m->value)) {
      hits[kept].va = a;
      hits[kept].old = now;
      kept++;
    }
  }
  m->n=kept; m->first=end<n?end:0;
  if(!m->first) m->count=kept;
  m->overflow = 0;
}

void mem_do(void) {
  JobMem *m = &P->mem;
  m->result = 0;
  u32 saved_hits=m->hits,saved_buf=m->buf;
  void *hits=NULL,*buf=NULL;
  bool ok=m->type<=MT_FLOAT && m->cmp<=CMP_DEC_BY;
  switch(m->op) {
  case MEM_READ: case MEM_WRITE:
    ok=ok && m->size && m->size<=4096 && (buf=lend(m->buf,m->size)); break;
  case MEM_SEARCH: case MEM_REFINE:
    ok=ok && m->cap && m->cap<=262144 && m->first<=m->cap && m->n<=m->first &&
      (hits=lend(m->hits,m->cap*(u32)sizeof(JobHit))); break;
  case MEM_VALUES:
    ok=ok && m->n && m->n<=1024 && m->first<=262144-m->n &&
      (hits=lend(m->hits,(m->first+m->n)*(u32)sizeof(JobHit))) &&
      (buf=lend(m->buf,m->n*8u)); break;
  case MEM_REGIONS: break;
  default: ok=false;
  }
  if (!ok) {
    m->result = (s32)RES_INVALID_ADDR;
    lend_end();
    return;
  }
  if(hits) m->hits=(u32)(uintptr_t)hits;
  if(buf) m->buf=(u32)(uintptr_t)buf;
  switch (m->op) {
  case MEM_READ:
    if (!mmu_user_ok(m->va, m->size))
      m->result = (s32)RES_INVALID_ADDR;
    else
      memcpy((void *)(uintptr_t)m->buf, (const void *)(uintptr_t)m->va, m->size);
    break;
  case MEM_WRITE:
    if (!mmu_user_ok(m->va, m->size)) {
      m->result = (s32)RES_INVALID_ADDR;
      break;
    }
    memcpy((void *)(uintptr_t)m->va, (const void *)(uintptr_t)m->buf, m->size);
    code_changed();
    break;
  case MEM_SEARCH:
    search(m);
    break;
  case MEM_REFINE:
    refine(m);
    break;
  case MEM_VALUES: {
    const JobHit *hits = (const JobHit *)(uintptr_t)m->hits;
    u32 *out = (u32 *)(uintptr_t)m->buf, w = width(m->type);
    for (u32 i = 0; i < m->n; i++) {
      u32 a = hits[m->first + i].va;
      bool there = mmu_user_ok(a, w);
      out[i * 2] = there ? get(a, m->type) : 0;
      out[i * 2 + 1] = there;
    }
    break;
  }
  case MEM_REGIONS:
    regions(m);
    break;
  default:
    m->result = (s32)RES_INVALID_ENUM;
  }
  m->hits=saved_hits; m->buf=saved_buf;
  lend_end();
}

void memtools_reset(void) {
  prog_words = 0;
  n_lent = 0;
}
