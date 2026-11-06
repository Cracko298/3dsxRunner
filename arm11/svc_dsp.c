#include "sup.h"

#define DSP_VA 0x1FF00000u
#define BANK 0x10000u
#define FRAME 160u
#define RING 3200u
#define FRAME_TICKS 1310720ull
static u8 dram[2 * BANK] __attribute__((aligned(4096)));
static s16 output[2][RING] __attribute__((aligned(32)));
static const u16 vars[7] = {0, 0x100, 0xC00, 0xD00, 0xF00, 0x1000, 0x1100};
typedef struct { u16 index; s16 h0, h1; } Adpcm;
typedef struct {
  u32 pa, count; Adpcm adpcm; u8 has_adpcm, loop; u16 seq, pad;
} Buffer;
typedef struct {
  u32 flags; float mix[12], rate; u8 rim[2]; u16 filter, mono[2], biquad[5], active;
  Buffer queue[4]; u32 pad0; u16 play, sync; u32 unknown, pad1;
  u32 pa, count; u16 format; Adpcm adpcm; u16 more, seq;
} Channel;
typedef struct { u16 flags, sync; u32 pos; u16 seq, last; } Status;
_Static_assert(sizeof(Channel) == 192, "NDSP channel ABI");
_Static_assert(sizeof(Buffer) == 20, "NDSP buffer ABI");
_Static_assert(sizeof(Status) == 12, "NDSP status ABI");
typedef struct {
  Buffer current, queue[4]; u32 pos, fraction, step, va, decoded;
  s32 left, right; s16 coefs[16], h0, h1, sample;
  u16 format, sync, last; u8 qhead, qcount, playing, valid;
} Voice;
static Voice voices[24];
static Object *irq, *semaphore;
static bool loaded, running, pending, mapped, hardware;
static u32 pipe_pos, pending_bank, write_pos;
static u64 next_tick;
static s32 master = 32768;

static u32 rotate(u32 x) { return x << 16 | x >> 16; }
static void *var(u32 bank, u32 id) { return dram + bank * BANK + vars[id] * 2u; }
static s16 clip(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (s16)v; }
static s32 gain(float v) { return !(v > 0) ? 0 : v >= 4 ? 131072 : (s32)(v * 32768.0f); }

static u32 samples_va(u32 pa, u32 bytes) {
  u32 va;
  if (!bytes || pa + bytes < pa) return 0;
  if (pa >= linear_pa && pa - linear_pa < linear_avail && bytes <= linear_avail - (pa - linear_pa)) {
    va = linear_va + (pa - linear_pa);
    if (mmu_user_ok(va, bytes) && mmu_pa(va) == pa) return va;
  }
  if (pa >= heap_pa && pa - heap_pa < heap_avail && bytes <= heap_avail - (pa - heap_pa)) {
    va = VA_HEAP + pa - heap_pa;
    if (mmu_user_ok(va, bytes) && mmu_pa(va) == pa) return va;
  }
  return 0;
}

static void begin_buffer(Voice *v, Buffer b) {
  v->current = b; v->pos = v->fraction = v->decoded = 0; v->sample = 0;
  if (b.has_adpcm) { v->h0 = b.adpcm.h0; v->h1 = b.adpcm.h1; }
  u32 channels = v->format & 3u, encoding = (v->format >> 2) & 3u;
  u64 bytes = encoding == 2 ? ((u64)b.count + 13) / 14 * 8 : (u64)b.count * channels * (encoding + 1);
  v->va = (channels == 1 || channels == 2) && encoding <= 2 &&
          !(encoding == 2 && channels != 1) && bytes <= 0xFFFFFFFFu ? samples_va(b.pa, (u32)bytes) : 0;
  v->valid = b.seq && b.count && v->va;
  if (b.seq && !v->valid) {
    log_printf("DSP: rejected sample buffer %08X (%u samples, format %u)\n", b.pa, b.count, v->format);
    v->last = b.seq;
    v->current.seq = 0;
  }
}

static void advance(Voice *v) {
  v->last = v->current.seq;
  if (v->current.loop && v->valid) { begin_buffer(v, v->current); return; }
  if (v->qcount) {
    Buffer b = v->queue[v->qhead]; v->qhead = (v->qhead + 1) & 3; v->qcount--;
    begin_buffer(v, b);
  } else { v->valid = 0; v->current.seq = 0; v->pos = 0; }
}

static void sample(Voice *v, s32 *l, s32 *r) {
  u32 channels = v->format & 3u, encoding = (v->format >> 2) & 3u;
  if (encoding == 0) {
    const s8 *p = (const s8 *)(uintptr_t)v->va + v->pos * channels;
    *l = (s32)p[0] * 256; *r = (s32)p[channels - 1] * 256;
  } else if (encoding == 1) {
    const s16 *p = (const s16 *)(uintptr_t)v->va + v->pos * channels;
    *l = p[0]; *r = p[channels - 1];
  } else {
    const u8 *p = (const u8 *)(uintptr_t)v->va;
    while (v->decoded <= v->pos) {
      u32 i = v->decoded++, f = (i / 14) * 8, n = i % 14;
      u8 head = p[f]; int nib = (p[f + 1 + n / 2] >> ((n & 1) ? 0 : 4)) & 15;
      if (nib >= 8) nib -= 16;
      int k = ((head >> 4) & 7) * 2;
      s32 val = (s32)(((s64)nib * (1 << (head & 15)) * 2048 + 1024 +
                       (s64)v->coefs[k] * v->h0 + (s64)v->coefs[k+1] * v->h1) >> 11);
      v->sample = clip(val); v->h1 = v->h0; v->h0 = v->sample;
    }
    *l = *r = v->sample;
  }
}

static void accept(u32 bank) {
  Channel *c = var(bank, 1);
  u32 *m = var(bank, 4);
  if (m[0] & 0x10000) { float vol; memcpy(&vol, m + 1, 4); master = gain(vol); }
  m[0] = 0;
  for (u32 i = 0; i < 24; i++) {
    Voice *v = &voices[i]; u32 f = c[i].flags;
    if (f & 0x30000000) {
      v->qhead = v->qcount = v->valid = 0; v->current.seq = v->last = 0;
      v->sync = c[i].sync;
    }
    if (f & 0x40000) {
      float rate = c[i].rate;
      v->step = !(rate > 0) ? 0 : rate >= 16 ? 16u * 65536 : (u32)(rate * 65536.0f);
    }
    if (f & 0xE000000) { v->left = gain(c[i].mix[0]); v->right = gain(c[i].mix[1]); }
    if (f & 4) memcpy(v->coefs, (s16 *)var(bank, 3) + 16*i, sizeof v->coefs);
    if (f & 0x10000) v->playing = (c[i].play & 255) == 1;
    if (f & 0x10) {
      v->qhead = v->qcount = 0;
      v->format = c[i].format;
      Buffer b = {rotate(c[i].pa), rotate(c[i].count), c[i].adpcm,
                  c[i].more & 1, (c[i].more >> 1) & 1, c[i].seq, 0};
      begin_buffer(v, b);
    }
    if (f & 0x80000) {
      for (u32 k = 0; k < 4; k++) {
        u32 slot = (v->qhead + v->qcount) & 3;
        if (!(c[i].active & (1u << slot)) || v->qcount == 4) break;
        Buffer b = c[i].queue[slot]; b.pa = rotate(b.pa); b.count = rotate(b.count);
        v->queue[slot] = b; v->qcount++; c[i].active &= ~(1u << slot);
      }
    }
    while (!v->valid && v->qcount) advance(v);
    c[i].flags = 0;
  }
}

#ifndef DSP_HOST_TEST
#define REG16(pa) (*(volatile u16 *)(uintptr_t)((pa) - PA_IO + VA_IO))
static void hardware_stop(void) {
  if (!hardware) return;
  REG16(0x10103420u) = REG16(0x10103440u) = 0;
  hardware = false;
}
static void hardware_start(void) {
  cache_clean_all();
  for (u32 side = 0; side < 2; side++) {
    u32 ch = 0x10103420u + side * 0x20u;
    REG16(ch) = 0;
    IO(ch + 4) = side ? 0x00008000u : 0x80000000u;
    IO(ch + 0xC) = sup_pa(output[side]);
    IO(ch + 0x10) = RING * 2;
    IO(ch + 0x14) = sup_pa(output[side]);
    REG16(ch + 2) = (u16)(0x10000u - 2048u);
    REG16(ch) = 0xD440u;
  }
  hardware = true;
}
#else
static void hardware_stop(void) { hardware = false; }
static void hardware_start(void) { hardware = true; }
#endif

static void mix_frame(void) {
  for (u32 s = 0; s < FRAME; s++) {
    s32 left = 0, right = 0;
    for (u32 i = 0; i < 24; i++) {
      Voice *v = &voices[i];
      if (!v->playing || !v->valid) continue;
      s32 l, r; sample(v, &l, &r);
      left += (s32)(((s64)l * v->left) >> 15); right += (s32)(((s64)r * v->right) >> 15);
      u32 phase = v->fraction + v->step;
      v->pos += phase >> 16; v->fraction = phase & 65535;
      while (v->valid && v->pos >= v->current.count) {
        u32 rest = v->pos - v->current.count, fraction = v->fraction;
        advance(v);
        if (v->valid) { v->pos = rest; v->fraction = fraction; }
      }
    }
    output[0][write_pos] = clip((s32)(((s64)left * master) >> 15));
    output[1][write_pos] = clip((s32)(((s64)right * master) >> 15));
    write_pos = (write_pos + 1) % RING;
  }
}

void dsp_tick(u64 now) {
  if (!running || now < next_tick) return;
  if (now - next_tick > FRAME_TICKS * 10) {
    hardware_stop(); memset(output, 0, sizeof output); write_pos = 960;
    next_tick = now; hardware_start();
  }
  while (now >= next_tick) {
    u32 bank = pending_bank;
    bool notify = pending;
    if (notify) { accept(bank); pending = false; }
    mix_frame();
    if (notify) {
      Status *s = var(bank ^ 1, 2);
      for (u32 i = 0; i < 24; i++) {
        Voice *v = &voices[i];
        s[i] = (Status){0x100u | (v->playing && v->valid ? 1u : 0u), v->sync,
                       rotate(v->pos), v->current.seq, v->last};
      }
      if (irq) event_signal(irq);
    }
    next_tick += FRAME_TICKS;
  }
  cache_clean_all();
}
u64 dsp_deadline(void) { return running ? next_tick : NO_DEADLINE; }
bool dsp_active(void) { return running; }

bool dsp_signal(Object *o) {
  if (o != semaphore || !running) return false;
  u16 a = *(u16 *)var(0, 0), b = *(u16 *)var(1, 0);
  pending_bank = (s16)(a - b) > 0 ? 0 : 1;
  pending = true;
  return true;
}
void dsp_overlay_pause(bool paused) {
  if(paused) hardware_stop();
  else if(running) {
    memset(output,0,sizeof output); write_pos=960;
    cache_clean_all(); hardware_start();
  }
}
void dsp_stop(void) {
  running = pending = false; hardware_stop();
}
void dsp_reset(void) {
  irq = semaphore = NULL; loaded = running = pending = mapped = hardware = false;
  memset(dram, 0, sizeof dram); memset(voices, 0, sizeof voices); memset(output, 0, sizeof output);
  master = 32768; pipe_pos = 0;
}

u32 dsp_resource(void) {
  u8 *page = page_alloc();
  Object *o = page ? obj_new(OBJ_MEMBLOCK) : NULL;
  if (!o) return 0;
  memcpy(page, "Aurora runner NDSP adapter", 25);
  *(u32 *)(void *)(page + 0x104) = 0x108;
  o->u.mb.pa = sup_pa(page); o->u.mb.size = 4096; o->u.mb.kind = 2;
  return handle_new(o);
}

static void dsp_request(Thread *t, Object *session, u32 *cmd) {
  (void)session; u32 id = cmd[0] >> 16, result = 0, value = 0;
  switch (id) {
  case 0x11:
    if (!mapped) mapped = mmu_map(DSP_VA, sup_pa(dram), sizeof dram, MAP_USER);
    loaded = mapped; value = loaded;
    if (!loaded) result = RES_OUT_OF_MEMORY;
    else log_printf("DSP: software NDSP mixer ready (PCM8/16, mono ADPCM)\n");
    break;
  case 0x12: dsp_stop(); loaded = false; break;
  case 0x15: {
    Object *o = cmd[4] ? handle_obj(cmd[4]) : NULL;
    if (cmd[4] && (!o || o->type != OBJ_EVENT)) { result = RES_INVALID_HANDLE; break; }
    if (o) obj_ref(o), o->who = "DSP interrupt";
    if (irq) obj_unref(irq);
    irq = o; break;
  }
  case 0x16: {
    if (!semaphore) { semaphore = obj_new(OBJ_EVENT); if (semaphore) semaphore->refs = 1, semaphore->who = "DSP semaphore"; }
    u32 h = handle_new(semaphore);
    cmd[0] = ipc_header(id, 1, 2); cmd[1] = h ? 0 : RES_OUT_OF_HANDLES; cmd[2] = 0; cmd[3] = h; return;
  }
  case 0xD: {
    if (cmd[1] != 2 || cmd[2] < 2 || !mmu_user_ok(cmd[4], cmd[2])) { result = RES_INVALID_ADDR; break; }
    u16 mode; memcpy(&mode, (void *)(uintptr_t)cmd[4], 2);
    if (mode == 0 || mode == 2) {
      if (!loaded) { result = RES_UNIMPL; break; }
      dsp_stop(); memset(dram, 0, sizeof dram); memset(voices, 0, sizeof voices);
      memset(output, 0, sizeof output); master = 32768; pipe_pos = 0; write_pos = 960;
      running = true; next_tick = ticks() + FRAME_TICKS; hardware_start();
      if (irq) event_signal(irq);
    } else if (mode == 1 || mode == 3) dsp_stop();
    else result = RES_UNIMPL;
    break;
  }
  case 0x10: {
    u32 cap; u8 *out = (u8 *)static_buffer(t, 0, &cap);
    u16 response[16] = {7, vars[0], vars[1], vars[2], vars[3], vars[4], vars[5], vars[6],
                        0x2000, 0x2800, 0x3000, 0x3800, 0x4000, 0x4800, 0x5000, 0x5800};
    u32 total = 16;
    if (P->launch_flags & 4u) response[0] = 15;
    else total = 8;
    if (!out || cmd[1] != 2) { result = RES_INVALID_ADDR; break; }
    u32 n = cmd[3]; if (n > cap) n = cap; if (pipe_pos > total * 2) pipe_pos = total * 2;
    if (n > total * 2 - pipe_pos) n = total * 2 - pipe_pos;
    memcpy(out, (u8 *)response + pipe_pos, n); pipe_pos += n; value = n; break;
  }
  case 0xC: {
    u32 addr = cmd[1];
    if ((addr & ~0x1FFFFu) || (addr & 0xFFFFu) >= BANK/2) result = RES_INVALID_ADDR;
    else value = DSP_VA + ((addr >> 16) & 1) * BANK + (addr & 0xFFFF) * 2;
    break;
  }
  case 1: case 2: case 4: value = 1; break;
  case 7: case 0x17: case 3: break;
  case 0x13: case 0x14:
    if (!mmu_user_ok(cmd[1], cmd[2])) result = RES_INVALID_ADDR;
    else cache_clean_all();
    break;
  case 0x1F: value = 0; break;
  default: result = RES_UNIMPL; log_printf("DSP: unsupported command %X\n", id); break;
  }
  cmd[0] = ipc_header(id, 2, 0); cmd[1] = result; cmd[2] = value;
}
const Service dsp_service = {"dsp::DSP", dsp_request, NULL};
