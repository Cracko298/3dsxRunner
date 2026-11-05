#include <stdarg.h>

#include "sup.h"

/* A tiny stand-in for the bits of Horizon's kernel that libctru actually uses.
 * Scheduling copies Horizon's app core: highest priority ready thread runs
 * until it waits, sleeps, or wakes someone more important. No time slicing.
 * When everyone is waiting, the job ends and the ARM9 shows the frame. */
Thread threads[MAX_THREADS];
Thread *cur;
static u32 next_tid, order_counter;

#define MAX_HANDLES 512
#define HANDLE_BASE 0x1000u
static Object *htab[MAX_HANDLES];
#define MAX_OBJECTS 640
static Object objs[MAX_OBJECTS];
static Object *process_obj, *reslimit_obj;

#define CUR_THREAD  0xFFFF8000u
#define CUR_PROCESS 0xFFFF8001u

#define SYSCLOCK 268111856ull
#define PTIMER_LOAD  0x17E00600u
#define PTIMER_COUNT 0x17E00604u
#define PTIMER_CNT   0x17E00608u
#define PTIMER_STAT  0x17E0060Cu
#define MPC(a) (*(volatile u32 *)(a))

static u64 tick_now;
static u32 ptimer_last;
static u64 job_start_tick;
#define BUDGET_TICKS (SYSCLOCK / 25)

static void preempt_start(void);
static void preempt_stop(void);

static void clock_start(void) {
  MPC(PTIMER_CNT) = 0;
  MPC(PTIMER_STAT) = 1;
  MPC(PTIMER_LOAD) = 0xFFFFFFFFu;
  MPC(PTIMER_CNT) = 3;
  ptimer_last = MPC(PTIMER_COUNT);
  u64 base = P->elapsed_us * SYSCLOCK / 1000000ull;
  if (base > tick_now)
    tick_now = base;
  job_start_tick = tick_now;
  preempt_start();
}

#define GICC_CTRL    0x17E00100u
#define GICC_PMR     0x17E00104u
#define GICC_IAR     0x17E0010Cu
#define GICC_EOI     0x17E00110u
#define GICD_CTRL    0x17E01000u
#define GICD_ENA_SET 0x17E01100u
#define GICD_ENA_CLR 0x17E01180u
#define GICD_PEN_CLR 0x17E01280u
#define GICD_PRI     0x17E01400u
#define WDT_LOAD     0x17E00620u
#define WDT_CTRL     0x17E00628u
#define WDT_ISR      0x17E0062Cu
#define WDT_DISABLE  0x17E00634u
#define IRQ_WDT      30u
#define SLICE_MS     20u

static struct {
  u32 gicc_ctrl, gicc_pmr, gicd_ctrl, ena[4], pri, wdt_ctrl, wdt_load;
  bool taken;
} gic;

static void ack_all(void) {
  for (int i = 0; i < 32; i++) {
    u32 iar = MPC(GICC_IAR);
    if ((iar & 0x3FFu) == 1023u)
      break;
    MPC(GICC_EOI) = iar;
  }
}

static void preempt_start(void) {
  u32 mhz = P->cpu_mhz ? P->cpu_mhz : 268u;
  gic.gicc_ctrl = MPC(GICC_CTRL);
  gic.gicc_pmr = MPC(GICC_PMR);
  gic.gicd_ctrl = MPC(GICD_CTRL);
  for (u32 i = 0; i < 4; i++)
    gic.ena[i] = MPC(GICD_ENA_SET + 4u * i);
  gic.pri = MPC(GICD_PRI + (IRQ_WDT & ~3u));
  gic.wdt_ctrl = MPC(WDT_CTRL);
  gic.wdt_load = MPC(WDT_LOAD);
  gic.taken = true;
  for (u32 i = 0; i < 4; i++)
    MPC(GICD_ENA_CLR + 4u * i) = 0xFFFFFFFFu;
  MPC(WDT_DISABLE) = 0x12345678u;
  MPC(WDT_DISABLE) = 0x87654321u;
  MPC(WDT_CTRL) = 0;
  MPC(WDT_ISR) = 1;
  MPC(GICD_PEN_CLR) = 1u << IRQ_WDT;
  *(volatile u8 *)(uintptr_t)(GICD_PRI + IRQ_WDT) = 0;
  MPC(GICD_CTRL) = 1;
  MPC(GICC_PMR) = 0xF0u;
  MPC(GICC_CTRL) = 1;
  ack_all();
  MPC(GICD_ENA_SET) = 1u << IRQ_WDT;
  MPC(WDT_LOAD) = mhz * (500u * (dsp_active() ? 4u : SLICE_MS));
  MPC(WDT_CTRL) = 1u | 4u;
}

static void preempt_stop(void) {
  if (!gic.taken)
    return;
  gic.taken = false;
  MPC(WDT_CTRL) = 0;
  MPC(WDT_ISR) = 1;
  MPC(GICD_ENA_CLR) = 1u << IRQ_WDT;
  ack_all();
  MPC(GICD_PEN_CLR) = 1u << IRQ_WDT;
  MPC(GICD_PRI + (IRQ_WDT & ~3u)) = gic.pri;
  MPC(GICC_PMR) = gic.gicc_pmr;
  MPC(GICC_CTRL) = gic.gicc_ctrl;
  MPC(GICD_CTRL) = gic.gicd_ctrl;
  for (u32 i = 0; i < 4; i++)
    MPC(GICD_ENA_SET + 4u * i) = gic.ena[i];
  if (gic.wdt_ctrl & 1u) {
    MPC(WDT_LOAD) = gic.wdt_load;
    MPC(WDT_CTRL) = gic.wdt_ctrl & ~4u;
  }
}

static void clock_stop(void) {
  MPC(PTIMER_CNT) = 0;
  preempt_stop();
}

/* The private timer runs at half the CPU clock, whatever the CPU clock is. */
u64 ticks(void) {
  u32 now = MPC(PTIMER_COUNT), delta = ptimer_last - now;
  ptimer_last = now;
  u32 mhz = P->cpu_mhz ? P->cpu_mhz : 268u;
  tick_now += (u64)delta * (SYSCLOCK * 2u) / ((u64)mhz * 1000000u);
  return tick_now;
}

u64 ns_to_ticks(s64 ns) {
  if (ns <= 0)
    return 0;
  u64 n = (u64)ns;
  return (n / 1000000u) * (SYSCLOCK / 1000u) + (n % 1000000u) * SYSCLOCK / 1000000000u;
}

void log_printf(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof buf - 1)
    n = sizeof buf - 1;
  for (int i = 0; i < n; i++)
    P->log[(P->log_head + (u32)i) % JOB_LOG_SIZE] = buf[i];
  P->log_head += (u32)n;
}

void sup_fail(const char *why) {
  snprintf(P->error, sizeof P->error, "%s", why);
  P->yield = YIELD_ERROR;
  clock_stop();
  sup_yield();
}

/* The ARM9 writes the cancel word, and the caches here are not coherent with
 * the ARM9 at all. A plain volatile read can keep returning the old value
 * forever on real hardware, so we invalidate that one line first. */
void job_check_cancel(void) {

  u32 address = JOB_CANCEL_ADDR;
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1\n"
                   "mcr p15, 0, %0, c7, c10, 4" :: "r"(address) : "memory");
  u32 reason = *(volatile u32 *)address;
  if (reason != JOB_CANCEL_HOME && reason != JOB_CANCEL_TIMEOUT) return;
  gpu_abort_transfers();
  dsp_stop();
  log_printf("ARM9 cancelled job: %s\n",reason==JOB_CANCEL_HOME?"HOME":"timeout");
  if (reason == JOB_CANCEL_TIMEOUT) sup_fail("The ARM11 job exceeded its time limit.");
  P->exit_code = 0;
  P->yield = YIELD_EXIT;
  clock_stop();
  sup_yield();
}

Object *obj_new(int type) {
  for (int i = 0; i < MAX_OBJECTS; i++)
    if (!objs[i].used) {
      memset(&objs[i], 0, sizeof objs[i]);
      objs[i].used = 1;
      objs[i].type = (u8)type;
      return &objs[i];
    }
  return NULL;
}

void obj_ref(Object *o) {
  if (o)
    o->refs++;
}

static void obj_destroy(Object *o) {
  if (o->type == OBJ_SESSION && o->u.se.svc && o->u.se.svc->closed)
    o->u.se.svc->closed(o);
  if (o->type == OBJ_THREAD && o->u.th.t && o->u.th.t->state == TS_DEAD)
    o->u.th.t->state = TS_FREE;
  o->used = 0;
}

void obj_unref(Object *o) {
  if (o && o->refs && !--o->refs)
    obj_destroy(o);
}

u32 handle_new(Object *o) {
  if (!o)
    return 0;
  for (u32 i = 0; i < MAX_HANDLES; i++)
    if (!htab[i]) {
      htab[i] = o;
      obj_ref(o);
      return HANDLE_BASE + i;
    }
  return 0;
}

Object *handle_obj(u32 h) {
  if (h == CUR_THREAD)
    return cur ? cur->obj : NULL;
  if (h == CUR_PROCESS)
    return process_obj;
  if (h < HANDLE_BASE || h >= HANDLE_BASE + MAX_HANDLES)
    return NULL;
  return htab[h - HANDLE_BASE];
}

static s32 handle_close(u32 h) {
  if (h < HANDLE_BASE || h >= HANDLE_BASE + MAX_HANDLES || !htab[h - HANDLE_BASE])
    return (s32)RES_INVALID_HANDLE;
  Object *o = htab[h - HANDLE_BASE];
  htab[h - HANDLE_BASE] = NULL;
  obj_unref(o);
  return 0;
}

Thread *thread_new(u32 entry, u32 arg, u32 sp, s32 prio, s32 core) {
  for (int i = 0; i < MAX_THREADS; i++) {
    Thread *t = &threads[i];
    if (t->state != TS_FREE)
      continue;
    Object *o = obj_new(OBJ_THREAD);
    if (!o)
      return NULL;
    memset(t, 0, sizeof *t);
    t->ctx.r[0] = arg;
    t->ctx.pc = entry & ~1u;
    t->ctx.sp = sp;
    t->ctx.lr = VA_STACK + 8;
    t->ctx.cpsr = 0x10u | 0x40u | ((entry & 1u) ? 0x20u : 0u);
    t->ctx.fpscr = 0x03C00000u;
    t->ctx.tls = VA_TLS + (u32)i * TLS_SIZE;
    memset((void *)(uintptr_t)t->ctx.tls, 0, TLS_SIZE);
    t->prio = prio;
    t->core = core;
    t->id = ++next_tid;
    t->order = ++order_counter;
    t->state = TS_READY;
    t->deadline = NO_DEADLINE;
    o->u.th.t = t;
    o->refs = 1;
    t->obj = o;
    return t;
  }
  return NULL;
}

void thread_block(Thread *t, int kind, u64 deadline) {
  t->state = TS_WAIT;
  t->wait = (u8)kind;
  t->deadline = deadline;
}

void thread_wake(Thread *t, s32 result) {
  t->ctx.r[0] = (u32)result;
  t->state = TS_READY;
  t->wait = WAIT_NONE;
  t->nw = 0;
  t->deadline = NO_DEADLINE;
  t->order = ++order_counter;
}

static int live_threads(void) {
  int n = 0;
  for (int i = 0; i < MAX_THREADS; i++)
    n += threads[i].state == TS_READY || threads[i].state == TS_WAIT;
  return n;
}

static bool obj_ready(Object *o, Thread *t) {
  switch (o->type) {
  case OBJ_EVENT:
    return o->u.ev.signaled;
  case OBJ_TIMER:
    return o->u.tm.signaled;
  case OBJ_MUTEX:
    return !o->u.mx.owner || o->u.mx.owner == t;
  case OBJ_SEMAPHORE:
    return o->u.sem.count > 0;
  case OBJ_THREAD:
    return o->u.th.t && o->u.th.t->state != TS_READY && o->u.th.t->state != TS_WAIT;
  default:
    return false;
  }
}

static void obj_acquire(Object *o, Thread *t) {
  switch (o->type) {
  case OBJ_EVENT:
    if (o->u.ev.reset == RESET_ONESHOT)
      o->u.ev.signaled = 0;
    break;
  case OBJ_TIMER:
    if (o->u.tm.reset == RESET_ONESHOT)
      o->u.tm.signaled = 0;
    break;
  case OBJ_MUTEX:
    o->u.mx.owner = t;
    o->u.mx.count++;
    break;
  case OBJ_SEMAPHORE:
    o->u.sem.count--;
    break;
  }
}

static bool try_satisfy(Thread *t) {
  if (t->wall) {
    for (int i = 0; i < t->nw; i++)
      if (!obj_ready(t->wobj[i], t))
        return false;
    for (int i = 0; i < t->nw; i++)
      obj_acquire(t->wobj[i], t);
    thread_wake(t, 0);
    return true;
  }
  for (int i = 0; i < t->nw; i++)
    if (obj_ready(t->wobj[i], t)) {
      obj_acquire(t->wobj[i], t);
      thread_wake(t, 0);
      t->ctx.r[1] = (u32)i;
      return true;
    }
  return false;
}

static Thread *best_waiter(Object *o) {
  Thread *best = NULL;
  for (int i = 0; i < MAX_THREADS; i++) {
    Thread *t = &threads[i];
    if (t->state != TS_WAIT || t->wait != WAIT_SYNC)
      continue;
    for (int k = 0; k < t->nw; k++)
      if (t->wobj[k] == o && (!best || t->prio < best->prio || (t->prio == best->prio && t->order < best->order)))
        best = t;
  }
  return best;
}

static void wake_waiters(Object *o) {
  for (int guard = 0; guard < MAX_THREADS; guard++) {
    Thread *t = best_waiter(o);
    if (!t)
      break;
    if (!try_satisfy(t)) {
      bool any = false;
      for (int i = 0; i < MAX_THREADS; i++) {
        Thread *u = &threads[i];
        if (u != t && u->state == TS_WAIT && u->wait == WAIT_SYNC) {
          for (int k = 0; k < u->nw; k++)
            if (u->wobj[k] == o) {
              any |= try_satisfy(u);
              break;
            }
        }
      }
      if (!any)
        break;
    }
    if (o->type == OBJ_EVENT && o->u.ev.reset == RESET_STICKY)
      continue;
    if (!obj_ready(o, NULL) && o->type != OBJ_MUTEX)
      break;
    if (o->type == OBJ_MUTEX && o->u.mx.owner)
      break;
  }
  if (o->type == OBJ_EVENT && o->u.ev.reset == RESET_PULSE)
    o->u.ev.signaled = 0;
  if (o->type == OBJ_TIMER && o->u.tm.reset == RESET_PULSE)
    o->u.tm.signaled = 0;
}

void event_signal(Object *ev) {
  if (!ev)
    return;
  if (ev->type == OBJ_EVENT)
    ev->u.ev.signaled = 1;
  wake_waiters(ev);
}

void kernel_tick(void) {
  u64 now = ticks();
  dsp_tick(now);
  for (int i = 0; i < MAX_OBJECTS; i++) {
    Object *o = &objs[i];
    if (!o->used || o->type != OBJ_TIMER || !o->u.tm.armed || o->u.tm.next > now)
      continue;
    o->u.tm.signaled = 1;
    if (o->u.tm.interval)
      while (o->u.tm.next <= now)
        o->u.tm.next += o->u.tm.interval;
    else
      o->u.tm.armed = 0;
    wake_waiters(o);
  }
  for (int i = 0; i < MAX_THREADS; i++) {
    Thread *t = &threads[i];
    if (t->state == TS_WAIT && t->deadline <= now) {
      int kind = t->wait;
      thread_wake(t, kind == WAIT_SLEEP ? 0 : (s32)RES_TIMEOUT);
      if (kind == WAIT_SYNC)
        t->ctx.r[1] = 0xFFFFFFFFu;
    }
  }
}

static Thread *pick(void) {
  Thread *best = NULL;
  for (int i = 0; i < MAX_THREADS; i++) {
    Thread *t = &threads[i];
    if (t->state == TS_READY &&
        (!best || t->prio < best->prio || (t->prio == best->prio && t->order < best->order)))
      best = t;
  }
  return best;
}

static void publish(void) {
  gsp_framebuffers();
  P->threads = (u32)live_threads();
}

Ctx *schedule(void) {
  for (;;) {
  job_check_cancel();
  if (gpu_lcd_watch)
    gpu_lcd_sample();
  if (gsp_gx_inflight)
    gsp_gx_poll();
  if (P->yield == YIELD_FS || P->yield == YIELD_HTTP) {
    publish();
    clock_stop();
    sup_yield();
  }
  kernel_tick();
  Thread *t = pick();
  if (t) {
    cur = t;
    return &t->ctx;
  }

  /* Nobody can run, but the GPU is still busy. Whatever they're waiting on
   * is probably that GPU interrupt, so wait for it right here. */
  if (gsp_gx_inflight) {
    gsp_gx_wait();
    continue;
  }
  break;
  }
  publish();
  if (!live_threads()) {
    P->yield = YIELD_EXIT;
    clock_stop();
    sup_yield();
  }
  u64 soonest = dsp_deadline();
  for (int i = 0; i < MAX_THREADS; i++)
    if (threads[i].state == TS_WAIT && threads[i].deadline < soonest)
      soonest = threads[i].deadline;
  P->wake_us = soonest == NO_DEADLINE ? 0 : soonest * 1000000ull / SYSCLOCK;
  P->yield = YIELD_FRAME;
  clock_stop();
  sup_yield();
}

s32 arm9_request(Thread *t, void (*done)(Thread *t)) {
  t->arm9_done = done;
  thread_block(t, WAIT_ARM9, NO_DEADLINE);
  P->fs.result = 0;
  P->yield = YIELD_FS;
  return 0;
}

static bool user_ok(u32 va, u32 size) { return mmu_user_ok(va, size); }

static u32 rd32(u32 va) { return *(volatile u32 *)(uintptr_t)va; }
static void wr32(u32 va, u32 v) { *(volatile u32 *)(uintptr_t)va = v; }

u32 heap_size, linear_size, linear_va;
u32 heap_pa, heap_avail, linear_pa, linear_avail;
u32 font_pa, font_va;
static u32 heap_mapped, linear_mapped;
u32 heap_in_use(void) { return heap_mapped; }
u32 linear_in_use(void) { return linear_mapped; }

static s32 control_memory(u32 op, u32 a0, u32 a1, u32 size, u32 *out) {
  u32 kind = op & 0xFFu;
  bool linear = (op & 0x10000u) != 0;
  (void)a1;
  if (kind == 3) {
    if (linear) {
      if (size > linear_avail - linear_mapped)
        return (s32)RES_OUT_OF_MEMORY;
      u32 va = linear_va + linear_mapped;
      if (!mmu_map(va, linear_pa + linear_mapped, (size + 0xFFFu) & ~0xFFFu, MAP_USER))
        return (s32)RES_OUT_OF_MEMORY;
      linear_mapped += (size + 0xFFFu) & ~0xFFFu;
      *out = va;
      return 0;
    }
    if (a0 < VA_HEAP || a0 + size > VA_HEAP + heap_avail || a0 != VA_HEAP + heap_mapped)
      return (s32)RES_OUT_OF_MEMORY;
    if (!mmu_map(a0, heap_pa + heap_mapped, (size + 0xFFFu) & ~0xFFFu, MAP_USER))
      return (s32)RES_OUT_OF_MEMORY;
    heap_mapped += (size + 0xFFFu) & ~0xFFFu;
    *out = a0;
    return 0;
  }
  if (kind == 1) {
    mmu_unmap(a0, (size + 0xFFFu) & ~0xFFFu);
    if (a0 >= VA_HEAP && a0 + size == VA_HEAP + heap_mapped)
      heap_mapped -= (size + 0xFFFu) & ~0xFFFu;
    if (a0 >= linear_va && a0 + size == linear_va + linear_mapped)
      linear_mapped -= (size + 0xFFFu) & ~0xFFFu;
    *out = a0;
    return 0;
  }
  if (kind == 4) {
    for (u32 off = 0; off < size; off += 0x1000u) {
      u32 pa = mmu_pa(a1 + off);
      if (!pa || !mmu_map(a0 + off, pa & ~0xFFFu, 0x1000u, MAP_USER))
        return (s32)RES_INVALID_ADDR;
    }
    *out = a0;
    return 0;
  }
  if (kind == 5) {
    mmu_unmap(a0, size);
    *out = a0;
    return 0;
  }
  if (kind == 6) {
    *out = a0;
    return 0;
  }
  return (s32)RES_INVALID_ENUM;
}

static void query_memory(Ctx *c, u32 addr) {
  struct { u32 base, size, perm, state; } r = {addr & ~0xFFFu, 0x1000u, 0, 0};
  bool page;
  (void)page;
  if (mmu_user_ok(addr, 1)) {
    u32 lo = addr & ~0xFFFu, hi = lo + 0x1000u;
    while (lo > 0x1000u && mmu_user_ok(lo - 0x1000u, 1) && addr - lo < 0x10000000u)
      lo -= 0x1000u;
    while (mmu_user_ok(hi, 1) && hi - addr < 0x10000000u)
      hi += 0x1000u;
    r.base = lo;
    r.size = hi - lo;
    r.perm = addr < VA_HEAP && addr >= VA_CODE ? 7u : 3u;
    r.state = addr < VA_HEAP && addr >= VA_CODE ? 4u  : 5u ;
    if (addr >= linear_va && addr < linear_va + linear_avail)
      r.state = 7u;
  } else {
    u32 hi = (addr & ~0xFFFu) + 0x1000u;
    while (hi && !mmu_user_ok(hi, 1) && hi - addr < 0x10000000u)
      hi += 0x1000u;
    r.size = hi - r.base;
  }
  c->r[0] = 0;
  c->r[1] = r.base;
  c->r[2] = r.size;
  c->r[3] = r.perm;
  c->r[4] = r.state;
  c->r[5] = 0;
}

static Object *new_with_handle(int type, Ctx *c) {
  Object *o = obj_new(type);
  u32 h = o ? handle_new(o) : 0;
  if (!h) {
    c->r[0] = (u32)RES_OUT_OF_HANDLES;
    return NULL;
  }
  c->r[0] = 0;
  c->r[1] = h;
  return o;
}

static void wait_sync(Ctx *c, Object **list, int n, bool all, s64 ns) {
  for (int i = 0; i < n; i++)
    cur->wobj[i] = list[i];
  cur->nw = (u8)n;
  cur->wall = all;
  if (n && try_satisfy(cur))
    return;
  if (!n && !all && ns == 0) {
    c->r[0] = (u32)RES_TIMEOUT;
    return;
  }
  if (ns == 0) {
    cur->nw = 0;
    c->r[0] = (u32)RES_TIMEOUT;
    c->r[1] = 0xFFFFFFFFu;
    return;
  }
  thread_block(cur, WAIT_SYNC, ns < 0 ? NO_DEADLINE : ticks() + ns_to_ticks(ns));
}

static void arbitrate(Ctx *c) {
  u32 addr = c->r[1], type = c->r[2];
  s32 value = (s32)c->r[3];
  s64 ns = (s64)(((u64)c->r[5] << 32) | c->r[4]);
  Object *arb = handle_obj(c->r[0]);
  if (!arb || arb->type != OBJ_ARBITER) {
    c->r[0] = (u32)RES_INVALID_HANDLE;
    return;
  }
  if ((addr & 3u) || !user_ok(addr, 4)) {
    c->r[0] = (u32)RES_INVALID_ADDR;
    return;
  }
  c->r[0] = 0;
  switch (type) {
  case 0: {
    for (s32 woken = 0; value < 0 || woken < value; woken++) {
      Thread *best = NULL;
      for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (t->state == TS_WAIT && t->wait == WAIT_ARB && t->waddr == addr &&
            (!best || t->prio < best->prio || (t->prio == best->prio && t->order < best->order)))
          best = t;
      }
      if (!best)
        break;
      thread_wake(best, 0);
    }
    return;
  }
  case 1:
  case 3:
  case 2:
  case 4: {
    s32 v = (s32)rd32(addr);
    if (v >= value)
      return;
    if (type == 2 || type == 4)
      wr32(addr, (u32)(v - 1));
    cur->waddr = addr;
    thread_block(cur, WAIT_ARB, (type >= 3 && ns >= 0) ? ticks() + ns_to_ticks(ns) : NO_DEADLINE);
    return;
  }
  default:
    c->r[0] = (u32)RES_INVALID_ENUM;
  }
}

static void exit_thread(Thread *t) {
  t->state = TS_DEAD;
  for (int i = 0; i < MAX_OBJECTS; i++)
    if (objs[i].used && objs[i].type == OBJ_MUTEX && objs[i].u.mx.owner == t) {
      objs[i].u.mx.owner = NULL;
      objs[i].u.mx.count = 0;
      wake_waiters(&objs[i]);
    }
  wake_waiters(t->obj);
  obj_unref(t->obj);
}

__attribute__((noreturn)) static void svc_break(Ctx *c, u32 kind) {
  memcpy(P->crash.r, c->r, 13 * 4);
  P->crash.r[13] = c->sp, P->crash.r[14] = c->lr, P->crash.r[15] = c->pc;
  P->crash.cpsr = c->cpsr;
  P->crash.kind = kind;
  P->crash.far = c->r[0];
  P->crash.thread = cur ? cur->id : 0;
  P->crash.in_sup = 0;
  P->yield = YIELD_CRASH;
  clock_stop();
  sup_yield();
}

static u32 unknown_svcs[8];

Ctx *trap_svc(Ctx *c) {
  u32 num;
  if (c->cpsr & 0x20u)
    num = *(const u16 *)(uintptr_t)(c->pc - 2u) & 0xFFu;
  else
    num = *(const u32 *)(uintptr_t)(c->pc - 4u) & 0xFFFFFFu;
  P->svc_count++;
  switch (num) {
  case 0x01: {
    u32 out = 0;
    c->r[0] = (u32)control_memory(c->r[0], c->r[1], c->r[2], c->r[3], &out);
    c->r[1] = out;
    break;
  }
  case 0x02:
    query_memory(c, c->r[2]);
    break;
  case 0x03:
    guest_exit(0);
  case 0x08: {
    s32 prio = (s32)c->r[0];
    if (prio < 0x18 || prio > 0x3F) {
      c->r[0] = 0xE0E01BFD;
      break;
    }
    Thread *t = thread_new(c->r[1], c->r[2], c->r[3] & ~7u, prio, (s32)c->r[4]);
    u32 h = t ? handle_new(t->obj) : 0;
    if (!h) {
      if (t)
        exit_thread(t);
      c->r[0] = (u32)RES_OUT_OF_MEMORY;
      break;
    }
    c->r[0] = 0;
    c->r[1] = h;
    break;
  }
  case 0x09:
    exit_thread(cur);
    break;
  case 0x0A: {
    s64 ns = (s64)(((u64)c->r[1] << 32) | c->r[0]);
    if (ns <= 0)
      cur->order = ++order_counter;
    else
      thread_block(cur, WAIT_SLEEP, ticks() + ns_to_ticks(ns));
    c->r[0] = 0;
    break;
  }
  case 0x0B: {
    Object *o = handle_obj(c->r[1]);
    if (!o || o->type != OBJ_THREAD) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    c->r[0] = 0;
    c->r[1] = (u32)o->u.th.t->prio;
    break;
  }
  case 0x0C: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_THREAD) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    o->u.th.t->prio = (s32)c->r[1];
    c->r[0] = 0;
    break;
  }
  case 0x0D: {
    Object *o = handle_obj(c->r[1]);
    u32 count = c->r[2];
    if (!o || o->type != OBJ_THREAD) c->r[0] = RES_INVALID_HANDLE;
    else if (!count || count > (P->new3ds ? 4u : 2u)) c->r[0] = RES_INVALID_ENUM;
    else if (!user_ok(c->r[0], count)) c->r[0] = RES_INVALID_ADDR;
    else {
      u8 *mask = (u8 *)(uintptr_t)c->r[0];
      memset(mask, 0, count); mask[0] = 1;
      c->r[0] = 0;
    }
    break;
  }
  case 0x0E: {
    Object *o = handle_obj(c->r[0]);
    u32 count = c->r[2];
    if (!o || o->type != OBJ_THREAD) c->r[0] = RES_INVALID_HANDLE;
    else if (!count || count > (P->new3ds ? 4u : 2u)) c->r[0] = RES_INVALID_ENUM;
    else if (!user_ok(c->r[1], count)) c->r[0] = RES_INVALID_ADDR;
    else {
      const u8 *mask = (const u8 *)(uintptr_t)c->r[1];
      bool any = false;
      for (u32 i = 0; i < count; i++) any |= mask[i] != 0;
      c->r[0] = any ? 0 : RES_INVALID_ENUM;
    }
    break;
  }
  case 0x0F: {
    Object *o = handle_obj(c->r[1]);
    c->r[0] = o && o->type == OBJ_THREAD ? 0 : RES_INVALID_HANDLE;
    c->r[1] = 0;
    break;
  }
  case 0x10: {
    Object *o = handle_obj(c->r[0]);
    s32 core = (s32)c->r[1];
    c->r[0] = !o || o->type != OBJ_THREAD ? RES_INVALID_HANDLE :
              core < -2 || core >= (P->new3ds ? 4 : 2) ? RES_INVALID_ENUM : 0;
    break;
  }
  case 0x11:
    c->r[0] = 0;
    break;
  case 0x13: {
    bool locked = (c->r[1] & 0xFFu) != 0;
    Object *o = new_with_handle(OBJ_MUTEX, c);
    if (o && locked) {
      o->u.mx.owner = cur;
      o->u.mx.count = 1;
    }
    break;
  }
  case 0x14: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_MUTEX) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    if (o->u.mx.owner != cur) {
      c->r[0] = 0xD8E0041F;
      break;
    }
    if (!--o->u.mx.count) {
      o->u.mx.owner = NULL;
      wake_waiters(o);
    }
    c->r[0] = 0;
    break;
  }
  case 0x15: {
    s32 initial = (s32)c->r[1], max = (s32)c->r[2];
    Object *o = new_with_handle(OBJ_SEMAPHORE, c);
    if (o)
      o->u.sem.count = initial, o->u.sem.max = max;
    break;
  }
  case 0x16: {
    Object *o = handle_obj(c->r[1]);
    if (!o || o->type != OBJ_SEMAPHORE) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    s32 prev = o->u.sem.count;
    if (prev + (s32)c->r[2] > o->u.sem.max) {
      c->r[0] = 0xD8E007FD;
      break;
    }
    o->u.sem.count += (s32)c->r[2];
    wake_waiters(o);
    c->r[0] = 0;
    c->r[1] = (u32)prev;
    break;
  }
  case 0x17: {
    u32 reset = c->r[1];
    Object *o = new_with_handle(OBJ_EVENT, c);
    if (o)
      o->u.ev.reset = (u8)reset;
    break;
  }
  case 0x18: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_EVENT) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    if (!dsp_signal(o)) event_signal(o);
    c->r[0] = 0;
    break;
  }
  case 0x19: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_EVENT) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    o->u.ev.signaled = 0;
    c->r[0] = 0;
    break;
  }
  case 0x1A: {
    u32 reset = c->r[1];
    Object *o = new_with_handle(OBJ_TIMER, c);
    if (o)
      o->u.tm.reset = (u8)reset;
    break;
  }
  case 0x1B: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_TIMER) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    s64 initial = (s64)(((u64)c->r[3] << 32) | c->r[2]);
    s64 interval = (s64)(((u64)c->r[4] << 32) | c->r[1]);
    o->u.tm.next = ticks() + ns_to_ticks(initial);
    o->u.tm.interval = ns_to_ticks(interval);
    o->u.tm.armed = 1;
    c->r[0] = 0;
    break;
  }
  case 0x1C:
  case 0x1D: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_TIMER) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    if (num == 0x1C)
      o->u.tm.armed = 0;
    else
      o->u.tm.signaled = 0;
    c->r[0] = 0;
    break;
  }
  case 0x1E: {
    u32 addr = c->r[1], size = c->r[2];
    if (!addr || (addr & 0xFFFu) || (size & 0xFFFu) || !user_ok(addr, size)) {
      c->r[0] = (u32)RES_INVALID_ADDR;
      break;
    }
    u32 pa = mmu_pa(addr);
    Object *o = new_with_handle(OBJ_MEMBLOCK, c);
    if (o)
      o->u.mb.pa = pa, o->u.mb.size = size;
    break;
  }
  case 0x1F: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_MEMBLOCK) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    u32 addr = c->r[1], flags = MAP_USER;
    if (!addr && o->u.mb.va)
      addr = o->u.mb.va;
    else if ((addr & 0xFFFu) || addr + o->u.mb.size < addr ||
             !((addr >= VA_MAP_BEGIN && addr + o->u.mb.size <= VA_MAP_END) ||
               (o->u.mb.kind == 2 && addr >= VA_HEAP && addr + o->u.mb.size <= VA_STACK &&
                !mmu_user_ok(addr, 1)))) {
      c->r[0] = (u32)RES_INVALID_ADDR;
      break;
    }
    if ((c->r[2] & 3u) == 1u)
      flags |= MAP_RO;
    c->r[0] = mmu_map(addr, o->u.mb.pa, (o->u.mb.size + 0xFFFu) & ~0xFFFu, flags) ? 0 : (u32)RES_INVALID_ADDR;
    break;
  }
  case 0x20: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_MEMBLOCK) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    mmu_unmap(c->r[1], (o->u.mb.size + 0xFFFu) & ~0xFFFu);
    c->r[0] = 0;
    break;
  }
  case 0x21:
    new_with_handle(OBJ_ARBITER, c);
    break;
  case 0x22:
    arbitrate(c);
    break;
  case 0x23:
    c->r[0] = (u32)handle_close(c->r[0]);
    break;
  case 0x24: {
    Object *o = handle_obj(c->r[0]);
    if (!o) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    wait_sync(c, &o, 1, false, (s64)(((u64)c->r[3] << 32) | c->r[2]));
    break;
  }
  case 0x25: {
    s32 n = (s32)c->r[2];
    if (n < 0 || n > MAX_WAIT || !user_ok(c->r[1], (u32)n * 4u)) {
      c->r[0] = 0xE0E01BFD;
      break;
    }
    Object *list[MAX_WAIT];
    bool bad = false;
    for (s32 i = 0; i < n; i++)
      if (!(list[i] = handle_obj(rd32(c->r[1] + (u32)i * 4u))))
        bad = true;
    if (bad) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    wait_sync(c, list, n, c->r[3] != 0, (s64)(((u64)c->r[4] << 32) | c->r[0]));
    break;
  }
  case 0x27: {
    Object *o = handle_obj(c->r[1]);
    u32 h = o ? handle_new(o) : 0;
    c->r[0] = o ? (h ? 0 : (u32)RES_OUT_OF_HANDLES) : (u32)RES_INVALID_HANDLE;
    c->r[1] = h;
    break;
  }
  case 0x28: {
    u64 t = ticks();
    c->r[0] = (u32)t;
    c->r[1] = (u32)(t >> 32);
    break;
  }
  case 0x2A: {
    u64 v = 0;
    c->r[0] = 0;
    if (c->r[1] == 0)
      v = c->r[2] == 1 ? (u64)(heap_mapped + linear_mapped) : 0;
    else if (c->r[1] == 2)
      v = 5;
    else if (c->r[1] == 26)
      v = 5;
    else
      c->r[0] = (u32)RES_INVALID_ENUM;
    c->r[1] = (u32)v;
    c->r[2] = (u32)(v >> 32);
    break;
  }
  case 0x2B: {
    u64 v = 0;
    c->r[0] = 0;
    if (c->r[2] == 0 || c->r[2] == 2)
      v = heap_mapped + linear_mapped;
    else if (c->r[2] == 20)
      v = (u64)(s64)(s32)(0x20000000u - 0x30000000u);
    else
      c->r[0] = (u32)RES_INVALID_ENUM;
    c->r[1] = (u32)v;
    c->r[2] = (u32)(v >> 32);
    break;
  }
  case 0x2D: {
    char name[12] = {0};
    if (!user_ok(c->r[1], 1)) {
      c->r[0] = (u32)RES_INVALID_ADDR;
      break;
    }
    for (int i = 0; i < 11 && user_ok(c->r[1] + (u32)i, 1); i++)
      if (!(name[i] = *(const char *)(uintptr_t)(c->r[1] + (u32)i)))
        break;
    if (!strcmp(name, "srv:")) {
      c->r[1] = session_new(&srv_service, NULL);
      c->r[0] = c->r[1] ? 0 : (u32)RES_OUT_OF_HANDLES;
    } else {
      log_printf("port %s: not there\n", name);
      c->r[0] = 0xD88007FA;
      c->r[1] = 0;
    }
    break;
  }
  case 0x32: {
    Object *o = handle_obj(c->r[0]);
    if (!o || o->type != OBJ_SESSION) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    c->r[0] = 0;
    P->ipc_count++;
    ipc_session_request(cur, o);
    break;
  }
  case 0x35:
    c->r[0] = 0;
    c->r[1] = 0x30;
    break;
  case 0x37: {
    Object *o = handle_obj(c->r[1]);
    if (!o || o->type != OBJ_THREAD) {
      c->r[0] = (u32)RES_INVALID_HANDLE;
      break;
    }
    c->r[0] = 0;
    c->r[1] = o->u.th.t->id;
    break;
  }
  case 0x38: {
    u32 h = handle_new(reslimit_obj);
    c->r[0] = h ? 0 : (u32)RES_OUT_OF_HANDLES;
    c->r[1] = h;
    break;
  }
  case 0x39:
  case 0x3A: {
    u32 n = c->r[3];
    if (!user_ok(c->r[0], n * 8u) || !user_ok(c->r[2], n * 4u)) {
      c->r[0] = (u32)RES_INVALID_ADDR;
      break;
    }
    for (u32 i = 0; i < n; i++) {
      u32 name = rd32(c->r[2] + i * 4u);
      u64 v = 0;
      if (num == 0x39) {
        static const u64 limits[10] = {0x18, 0, 32, 64, 32, 32, 8, 8, 16, 1000};
        v = name == 1 ? (u64)(heap_size + linear_size) : (name < 10 ? limits[name] : 0);
      } else {
        v = name == 1 ? 0 : (name == 2 ? (u64)live_threads() : 0);
      }
      wr32(c->r[0] + i * 8u, (u32)v);
      wr32(c->r[0] + i * 8u + 4u, (u32)(v >> 32));
    }
    c->r[0] = 0;
    break;
  }
  case 0x3C:
    log_printf("svcBreak(%u)\n", c->r[0]);
    svc_break(c, 3);
  case 0x3D: {
    u32 n = c->r[1] > 256u ? 256u : c->r[1];
    if (user_ok(c->r[0], n)) {
      char buf[260];
      memcpy(buf, (const void *)(uintptr_t)c->r[0], n);
      buf[n] = 0;
      log_printf("%s", buf);
    }
    c->r[0] = 0;
    break;
  }
  case 0x52:
  case 0x53:
  case 0x54:
    cache_clean_all();
    c->r[0] = 0;
    break;
  default: {
    bool seen = false;
    for (int i = 0; i < 8; i++)
      if (unknown_svcs[i] == num + 1u)
        seen = true;
    if (!seen) {
      log_printf("svc 0x%02X: not implemented\n", num);
      for (int i = 0; i < 8; i++)
        if (!unknown_svcs[i]) {
          unknown_svcs[i] = num + 1u;
          break;
        }
    }
    c->r[0] = (u32)RES_UNIMPL;
  }
  }
  if (ticks() - job_start_tick > BUDGET_TICKS && cur && cur->state == TS_READY) {
    P->yield = YIELD_BUDGET;
    publish();
    clock_stop();
    sup_yield();
  }
  return schedule();
}

Ctx *trap_irq(Ctx *c) {
  (void)c;
  u32 iar = MPC(GICC_IAR);
  MPC(WDT_ISR) = 1;
  if ((iar & 0x3FFu) != 1023u)
    MPC(GICC_EOI) = iar;
  job_check_cancel();
  P->preempts++;
  P->yield = YIELD_BUDGET;
  publish();
  clock_stop();
  sup_yield();
}

void trap_fault(u32 kind, Ctx *frame) {
  u32 far = 0, fsr = 0;
  if (kind == 0) {
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(far));
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(fsr));
  } else if (kind == 1) {
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(far));
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(fsr));
  }
  if (kind == 2 && (frame->cpsr & 0x20u))
    frame->pc += 2;
  memcpy(P->crash.r, frame->r, 13 * 4);
  P->crash.r[13] = frame->sp, P->crash.r[14] = frame->lr, P->crash.r[15] = frame->pc;
  P->crash.cpsr = frame->cpsr;
  P->crash.kind = kind;
  P->crash.far = far;
  P->crash.fsr = fsr;
  P->crash.thread = cur ? cur->id : 0;
  P->crash.in_sup = (frame->cpsr & 0x1Fu) != 0x10u;
  P->yield = YIELD_CRASH;
  clock_stop();
  sup_yield();
}

void kernel_init(void) {
  memset(threads, 0, sizeof threads);
  memset(htab, 0, sizeof htab);
  memset(objs, 0, sizeof objs);
  cur = NULL;
  next_tid = 0;
  order_counter = 0;
  tick_now = 0;
  memset(unknown_svcs, 0, sizeof unknown_svcs);
  process_obj = obj_new(OBJ_PROCESS);
  process_obj->refs = 1;
  reslimit_obj = obj_new(OBJ_RESLIMIT);
  reslimit_obj->refs = 1;
}

static Thread *arm9_waiter(void) {
  for (int i = 0; i < MAX_THREADS; i++)
    if (threads[i].state == TS_WAIT && threads[i].wait == WAIT_ARM9)
      return &threads[i];
  return NULL;
}

void job_run(void) {
  P->done = P->cmd;
  clock_start();
  job_check_cancel();
  P->yield = YIELD_NONE;
  Thread *w = arm9_waiter();
  if (w) {
    void (*done)(Thread *) = w->arm9_done;
    w->arm9_done = NULL;
    cur = w;
    thread_wake(w, 0);
    if (done)
      done(w);
  }
  if (P->vblanks) {
    if(P->plugin_enabled) cheats_frame();
    gsp_vblank();
    hid_update();
  }
  sup_resume(schedule());
}

void guest_exit(s32 code) {
  P->exit_code = code;
  P->yield = YIELD_EXIT;
  publish();
  clock_stop();
  sup_yield();
}
