#include "sup.h"
#include "pica_rebase.h"

enum { EV_PSC0, EV_PSC1, EV_VBLANK0, EV_VBLANK1, EV_PPF, EV_P3D, EV_DMA };

/* GSP's shared page: the interrupt queue at 0x000, framebuffer info at 0x200,
 * the GX command queue at 0x800. One client, thread index 0, always. */
static u8 *shmem;
static Object *shmem_block, *client_event;
static u32 shmem_pa;

typedef struct {
  u32 fb0, fb1, stride, format;
  bool valid;
} Shown;
static Shown shown[2];
static u32 serial[2];
static bool fb_logged[2];
static u32 submitted[6], completed[6], irq_overflows;
static u32 last_gx[8];
static struct { u32 va, hash, changes, samples; } output_samples[4];
static PicaRebase rebase;
static bool rebase_logged;
static u32 async_started, async_overlapped;
static bool gx_sync(void) { return (P->launch_flags & 2u) != 0; }

/* Only the New 3DS layout puts the linear heap past 0x28000000, so only it
 * needs the vertex rebase (see pica_rebase.h for the whole sad story). */
static bool rebase_wanted(void) {
  return (u64)P->linear.pa + P->linear.size > PICA_REBASE_TARGET;
}

static u32 sample_buffer(u32 va, u32 bytes) {
  if (bytes<4 || !mmu_user_ok(va,bytes)) return 0;
  u32 h=2166136261u, words=bytes/4u;
  for (u32 i=0;i<64;i++) {
    u32 at=(u32)(((u64)(words-1)*i)/63u);
    h=(h^((volatile u32 *)(uintptr_t)va)[at])*16777619u;
  }
  return h;
}

void gsp_report(void) {
  log_printf("GX done/submitted: DMA=%u/%u P3D=%u/%u fill=%u/%u PPF=%u/%u copy=%u/%u; IRQ-overflow=%u\n",
             completed[0],submitted[0],completed[1],submitted[1],completed[2],submitted[2],
             completed[3],submitted[3],completed[4],submitted[4],irq_overflows);
  log_printf("GX last: %08X %08X %08X %08X %08X %08X %08X %08X\n",
             last_gx[0],last_gx[1],last_gx[2],last_gx[3],last_gx[4],last_gx[5],last_gx[6],last_gx[7]);
  log_printf("GSP swaps=%u/%u IRQ-pending=%u\n",serial[0],serial[1],shmem?shmem[1]:0);
  log_printf("GX async: started=%u returned-to-guest=%u%s\n",async_started,async_overlapped,
             gx_sync()?" (sync-gx)":"");
  if (rebase.lists)
    log_printf("PICA rebase: lists=%u base=%u offsets=%u index=%u below=%u partial=%u guest-base=%08X\n",
               rebase.lists,rebase.bases,rebase.offsets,rebase.indices,rebase.below,rebase.partial,
               rebase.guest_base);
  for (u32 i=0;i<4;i++) if (output_samples[i].samples)
    log_printf("PPF output %08X sample=%08X changes=%u/%u\n",output_samples[i].va,
               output_samples[i].hash,output_samples[i].changes,output_samples[i].samples);
}

static void trace_first_list(const u32 *w,u32 bytes) {
  u32 color=0,depth=0,attrib=0,offset=0,texture=0,draws=0,code=0;
  for (u32 i=0,n=bytes/4;i+1<n;) {
    u32 value=w[i],header=w[i+1],extra=(header>>20)&255u,reg=header&0x3FFu;
    if (i+2+extra>n) break;
    for(u32 k=0;k<=extra;k++) {
      u32 r=reg+((header>>31)?k:0),v=k?w[i+1+k]:value;
      if (r==0x11C) depth=v<<3;
      if (r==0x11D) color=v<<3;
      if (r==0x200) attrib=v<<3;
      if (r==0x203) offset=v;
      if (r==0x085) texture=v<<3;
      if (r==0x22E || r==0x22F) draws++;
      if (r>=0x2CC && r<=0x2D3) code++;
    }
    i+=(2+extra+1)&~1u;
    if(reg==0x010) break;
  }
  log_printf("PICA first-list trace: draws=%u shader-words=%u color=%08X depth=%08X vertex0=%08X tex0=%08X\n",
             draws,code,color,depth,attrib+offset,texture);
}

#define TOP_BYTES (400u * 720u)
#define BOT_BYTES (320u * 720u)

/* Each screen cycles through its own three buffers, so if the game only
 * redraws the top screen we don't copy (or wait on) the bottom one. */
static u32 display_slot[2];
_Static_assert(TOP_BYTES+BOT_BYTES<=DISPLAY_SLOT_SIZE,"both panels fit a display slot");

static void interrupt(int ev) {
  if (!shmem)
    return;
  u8 *q = shmem;
  u8 cur = q[0], count = q[1];
  if (count >= 0x34) {
    irq_overflows++;
    q[2] = 1;
  } else {
    q[0xC + (cur + count) % 0x34] = (u8)ev;
    q[1] = (u8)(count + 1);
  }
  if (client_event)
    event_signal(client_event);
}

/* libctru flips the "update" byte when it presents; real GSP picks it up at
 * vblank. We pick it up whenever we get control, which is close enough. */
static void latch(int screen) {
  if (!shmem)
    return;
  u8 *hdr = shmem + 0x200 + screen * 0x40;
  if (!hdr[1])
    return;
  const u32 *info = (const u32 *)(void *)(hdr + 4 + (hdr[0] & 1u) * 0x1C);
  shown[screen].fb0 = info[1];
  shown[screen].fb1 = info[2];
  shown[screen].stride = info[3];
  shown[screen].format = info[4];
  shown[screen].valid = true;
  hdr[1] = 0;
  serial[screen]++;
}

static u32 bpp_of(u32 fmt) {
  switch (fmt & 7u) {
  case 0:
    return 4;
  case 1:
    return 3;
  default:
    return 2;
  }
}

static u32 contiguous(u32 va, u32 size) {
  if (!mmu_user_ok(va, size))
    return 0;
  u32 pa = mmu_pa(va);
  for (u32 off = 0x1000u - (va & 0xFFFu); off < size; off += 0x1000u)
    if (mmu_pa(va + off) != pa + off)
      return 0;
  return pa;
}

static void rgba8_column(u8 *d, const u8 *s) {
  const u32 *in=(const u32 *)(const void *)s;
  u32 *out=(u32 *)(void *)d;
  for (int y=0;y<240;y+=4,in+=4,out+=3) {
    u32 p0=in[0], p1=in[1], p2=in[2], p3=in[3];
    out[0]=(p0>>8)|((p1&0xFF00u)<<16);
    out[1]=(p1>>16)|((p2&0xFFFF00u)<<8);
    out[2]=(p2>>24)|(p3&0xFFFFFF00u);
  }
}

static void convert(u8 *dst, u32 src, u32 fmt, u32 stride, int columns, int step) {
  u32 bpp = bpp_of(fmt);
  u64 span=(u64)(columns*step-1)*stride+240u*bpp;
  if (fmt>4 || stride<240u*bpp || span>0xFFFFFFFFu || !mmu_user_ok(src,(u32)span)) {
    memset(dst, 0, (size_t)columns * 720u);
    return;
  }
  if ((fmt&7u)==1u && step==1 && stride==720u) {
    memcpy(dst,(const void *)(uintptr_t)src,(size_t)columns*720u);
    return;
  }
  bool rgba_words=(fmt&7u)==0u && !((src|stride|(u32)(uintptr_t)dst)&3u);
  for (int x = 0; x < columns; x++) {
    const u8 *s = (const u8 *)(uintptr_t)(src + (u32)(x * step) * stride);
    u8 *d = dst + x * 720;
    switch (fmt & 7u) {
    case 0:
      if (rgba_words) { rgba8_column(d, s); break; }
      for (int y = 0; y < 240; y++, s += 4, d += 3)
        d[0] = s[1], d[1] = s[2], d[2] = s[3];
      break;
    case 1:
      memcpy(d, s, 720);
      break;
    case 2:
      for (int y = 0; y < 240; y++, s += 2, d += 3) {
        u32 p = s[0] | (u32)s[1] << 8;
        u32 r = p >> 11, g = (p >> 5) & 63u, b = p & 31u;
        d[0] = (u8)(b << 3 | b >> 2), d[1] = (u8)(g << 2 | g >> 4), d[2] = (u8)(r << 3 | r >> 2);
      }
      break;
    case 3:
      for (int y = 0; y < 240; y++, s += 2, d += 3) {
        u32 p = s[0] | (u32)s[1] << 8;
        u32 r = p >> 11, g = (p >> 6) & 31u, b = (p >> 1) & 31u;
        d[0] = (u8)(b << 3 | b >> 2), d[1] = (u8)(g << 3 | g >> 2), d[2] = (u8)(r << 3 | r >> 2);
      }
      break;
    default:
      for (int y = 0; y < 240; y++, s += 2, d += 3) {
        u32 p = s[0] | (u32)s[1] << 8;
        d[0] = (u8)(((p >> 4) & 15u) * 17u), d[1] = (u8)(((p >> 8) & 15u) * 17u),
        d[2] = (u8)((p >> 12) * 17u);
      }
      break;
    }
  }
}

/* Yes, even native BGR8 gets copied. libctru can start reusing its front
 * buffer before the LCD is done reading it, and that's how you get tearing
 * and the "two frames glued together" look from r3-r5. */
static void picture(int screen, u8 *dst) {
  const Shown *s=&shown[screen];
  if (!s->valid) { memset(dst,0,screen?BOT_BYTES:TOP_BYTES); return; }
  if (!fb_logged[screen]) {
    fb_logged[screen]=true;
    log_printf("GSP %s: VA=%08X PA=%08X fmt=%X stride=%u\n",
               screen?"bottom":"top",s->fb0,mmu_pa(s->fb0),s->format,s->stride);
  }
  int columns=screen?320:400, step=(!screen && !(s->format&0x60u))?2:1;
  u32 fmt=s->format&7u;
  convert(dst,s->fb0,fmt,s->stride?s->stride:240u*bpp_of(fmt),columns,step);
}

void gsp_framebuffers(void) {

  latch(0);
  latch(1);
}

void gsp_vblank(void) {
  latch(0);
  latch(1);
  interrupt(EV_VBLANK0);
  interrupt(EV_VBLANK1);
}

static u32 panel_pa(int screen) {
  return P->display.pa+display_slot[screen]*DISPLAY_SLOT_SIZE+(screen?TOP_BYTES:0u);
}

void gsp_present(void) {
  gsp_framebuffers();

  bool changed[2]={!P->top.pa || P->top.serial!=serial[0],
                   !P->bottom.pa || P->bottom.serial!=serial[1]};
  if (!changed[0] && !changed[1])
    return;
  u32 pa[2]={0,0};
  for (int s=0;s<2;s++) if (changed[s]) {
    pa[s]=panel_pa(s);
    picture(s,(u8 *)(uintptr_t)pa[s]);
  }
  gpu_scanout(pa[0],pa[1]);
  if (changed[0]) {
    P->top=(JobFb){pa[0],1,720,serial[0]};
    display_slot[0]=(display_slot[0]+1u)%3u;
  }
  if (changed[1]) {
    P->bottom=(JobFb){pa[1],1,720,serial[1]};
    display_slot[1]=(display_slot[1]+1u)%3u;
  }
  P->top_right_pa=0;
}

/* A bad buffer kills the game with a message instead of faking success.
 * Faking it once cost me a week of staring at "frozen" screens. */
static u32 gpu_addr(u32 va, u32 size, bool output) {
  u32 pa = size ? contiguous(va, size) : 0;
  if (!pa || (pa&7u) || (output && !mmu_user_write_ok(va,size))) {
    char why[128];
    snprintf(why,sizeof why,"Invalid GPU %s buffer: %08X, size %X.",output?"output":"input",va,size);
    log_printf("%s\n",why);
    sup_fail(why);
  }
  return pa;
}

/* citro3d passes the unscaled size on both sides; the hardware halves the
 * output for anti-aliasing. Check the halved size or valid buffers at the end
 * of a region get rejected. */
static u32 dim_bytes(u32 dim, u32 flags, bool out) {
  u32 w = dim & 0xFFFFu, h = dim >> 16;
  u32 fmt = out ? (flags >> 12) & 7u : (flags >> 8) & 7u;
  u32 scale=(flags>>24)&3u;
  if (scale==3) return 0;

  if (out) { if (scale) w>>=1; if (scale==2) h>>=1; }
  u64 bytes=(u64)w*h*bpp_of(fmt);
  return fmt<=4 && bytes<=0xFFFFFFFFu ? (u32)bytes : 0;
}

static u32 copy_span(u32 size,u32 dim) {
  u32 width=(dim&0xFFFFu)*16u, gap=(dim>>16)*16u;
  if (!size || (size&15u) || (!width && gap)) return 0;
  u64 span=size;
  if (width) span+=(u64)((size-1)/width)*gap;
  return span<=0xFFFFFFFFu?(u32)span:0;
}

/* Asynchronous GX.
 *
 * We used to run each GPU command to completion inside the game's GSP call,
 * which meant the ARM11 sat there twiddling its thumbs while the GPU drew.
 * Now we start the command and let the game get on with its life (citro3d
 * happily builds the next frame meanwhile). We check on the GPU at every
 * kernel call and job entry, and only busy-wait when every game thread is
 * blocked anyway, which is exactly what we did before. So worst case it's
 * r14, best case the CPU and GPU finally work at the same time.
 *
 * One command at a time, in order, like the real GSP. If this ever breaks a
 * game, sync-gx.txt on the SD card turns it all off. */
enum { ENG_PSC0 = 1, ENG_PSC1 = 2, ENG_PPF = 4, ENG_P3D = 8 };
static struct { u32 c[8]; u32 engines; u64 start; } fly;
bool gsp_gx_inflight;

#define GX_ASYNC_TIMEOUT (268111856ull * 2u)

static void ppf_complete(const u32 *c, bool transfer) {
  cache_clean_all();
  if (!transfer) return;
  u32 h=sample_buffer(c[2],dim_bytes(c[4],c[5],true));
  for (u32 i=0;i<4;i++) if (!output_samples[i].va || output_samples[i].va==c[2]) {
    output_samples[i].va=c[2];
    if (output_samples[i].samples && output_samples[i].hash!=h) output_samples[i].changes++;
    output_samples[i].hash=h;
    output_samples[i].samples++;
    break;
  }
  if (submitted[3]<=2)
    log_printf("PPF: %08X->%08X dim=%08X/%08X flags=%08X sample=%08X\n",
               mmu_pa(c[1]),mmu_pa(c[2]),c[3],c[4],c[5],h);
}

static bool gx_begin(const u32 *c) {
  P->gx_count++;
  memcpy(last_gx,c,sizeof last_gx);
  u32 op=c[0]&0xFFu;
  if (op<6) submitted[op]++;
  memcpy(fly.c,c,sizeof fly.c);
  fly.engines=0;
  switch (op) {
  case 0: {
    if (!mmu_user_ok(c[1],c[3]) || !mmu_user_write_ok(c[2],c[3]))
      sup_fail("Invalid GPU DMA buffer.");
    memmove((void *)(uintptr_t)c[2], (const void *)(uintptr_t)c[1], c[3]);
    cache_clean_all();
    interrupt(EV_DMA);
    break;
  }
  case 1: {
    if (c[2]&7u) sup_fail("GPU command-list size is not 8-byte aligned.");
    if (!c[2]) { interrupt(EV_P3D); break; }
    u32 pa = gpu_addr(c[1], c[2], false);
    if (submitted[1]==1) {
      log_printf("P3D first: VA=%08X PA=%08X bytes=%X words=%08X/%08X\n",
                 c[1],pa,c[2],((const u32 *)(uintptr_t)c[1])[0],((const u32 *)(uintptr_t)c[1])[1]);
      trace_first_list((const u32 *)(uintptr_t)c[1],c[2]);
    }
    if (rebase_wanted() && mmu_user_write_ok(c[1],c[2])) {
      pica_rebase_list(&rebase,(u32 *)(uintptr_t)c[1],c[2]/4u);
      if (!rebase_logged && rebase.bases) {
        rebase_logged=true;
        log_printf("PICA: vertex arrays rebased %08X->%08X (linear heap %08X)\n",
                   rebase.guest_base,PICA_REBASE_TARGET,P->linear.pa);
      }
    }
    gpu_cmdlist_start(pa, c[2]);
    fly.engines=ENG_P3D;
    break;
  }
  case 2: {
    u32 pa0=c[1]?gpu_addr(c[1], c[3] - c[1], true):0;
    u32 pa1=c[4]?gpu_addr(c[4], c[6] - c[4], true):0;
    if (pa0) { gpu_fill_start(0, pa0, pa0 + (c[3] - c[1]), c[2], c[7] & 0xFFFFu); fly.engines|=ENG_PSC0; }
    if (pa1) { gpu_fill_start(1, pa1, pa1 + (c[6] - c[4]), c[5], c[7] >> 16); fly.engines|=ENG_PSC1; }
    break;
  }
  case 3: {
    u32 in = gpu_addr(c[1], dim_bytes(c[3], c[5], false), false);
    u32 out = gpu_addr(c[2], dim_bytes(c[4], c[5], true), true);
    gpu_transfer_start(in, out, c[3], c[4], c[5]);
    fly.engines=ENG_PPF;
    break;
  }
  case 4: {
    u32 in = gpu_addr(c[1], copy_span(c[3],c[4]), false), out = gpu_addr(c[2], copy_span(c[3],c[5]), true);
    gpu_texcopy_start(in, out, c[3], c[4], c[5], c[6]);
    fly.engines=ENG_PPF;
    break;
  }
  case 5:
    cache_clean_all();
    break;
  default:
    log_printf("GX command %u: unknown\n", op);
  }
  if (!fly.engines) {
    if (op<6) completed[op]++;
    return false;
  }
  fly.start=ticks();
  gsp_gx_inflight=true;
  async_started++;
  return true;
}

static void gx_check(void) {
  u32 op=fly.c[0]&0xFFu;
  if ((fly.engines&ENG_P3D) && gpu_cmdlist_done()) {
    gpu_cmdlist_finish(); fly.engines&=~ENG_P3D; interrupt(EV_P3D);
  }
  if ((fly.engines&ENG_PSC0) && gpu_fill_done(0)) {
    gpu_fill_finish(0); fly.engines&=~ENG_PSC0; interrupt(EV_PSC0);
  }
  if ((fly.engines&ENG_PSC1) && gpu_fill_done(1)) {
    gpu_fill_finish(1); fly.engines&=~ENG_PSC1; interrupt(EV_PSC1);
  }
  if ((fly.engines&ENG_PPF) && gpu_ppf_done()) {
    gpu_ppf_finish(); fly.engines&=~ENG_PPF;
    ppf_complete(fly.c, op==3);
    interrupt(EV_PPF);
  }
  if (!fly.engines) {
    gsp_gx_inflight=false;
    completed[op]++;
  }
}

static bool gx_take(u32 c[8]) {
  if (!shmem)
    return false;
  u32 *q = (u32 *)(void *)(shmem + 0x800);
  u32 hdr = q[0];
  u32 index = hdr & 0xFFu, total = (hdr >> 8) & 0xFFu;
  if (!total)
    return false;
  memcpy(c, &q[8 * (1 + index % 15u)], 8 * sizeof(u32));
  q[0] = (hdr & 0xFFFF0000u) | ((total - 1u) << 8) | ((index + 1u) % 15u);
  return true;
}

static void gx_advance(void) {
  u32 c[8];
  for (int guard = 0; guard < 64 && !gsp_gx_inflight && gx_take(c); guard++)
    gx_begin(c);
}

void gsp_gx_poll(void) {
  if (!gsp_gx_inflight) return;
  gx_check();
  if (gsp_gx_inflight) {
    if (ticks()-fly.start>GX_ASYNC_TIMEOUT)
      gpu_async_timeout((fly.c[0]&0xFFu)==1?"a command list":"a GPU transfer");
    return;
  }
  gx_advance();
}

void gsp_gx_wait(void) {
  u32 polls=0;
  while (gsp_gx_inflight) {
    gx_check();
    if (!gsp_gx_inflight) break;
    if (!((++polls)&1023u)) job_check_cancel();
    if (polls>=8000000u)
      gpu_async_timeout((fly.c[0]&0xFFu)==1?"a command list":"a GPU transfer");
  }
  gx_advance();
}

void gsp_gx_drain(void) {
  gx_advance();
  while (gsp_gx_inflight) gsp_gx_wait();
}

/* On STOP, give a running command a chance to finish before we hand the
 * display and GPU config back. No cancel checks here: STOP has to complete. */
void gsp_gx_settle(void) {
  for (u32 polls=0; gsp_gx_inflight && polls<8000000u; polls++)
    gx_check();
  if (gsp_gx_inflight) log_printf("GX: a command was still running at stop\n");
}

void gsp_gx_forget(void) {
  gsp_gx_inflight=false;
  fly.engines=0;
}

static void gx_queue(void) {
  if (gx_sync()) { gsp_gx_drain(); return; }
  if (gsp_gx_inflight) { gx_check(); if (gsp_gx_inflight) return; }
  gx_advance();
  if (gsp_gx_inflight) async_overlapped++;
}

static void reply(u32 *cmd, u32 id, s32 res) {
  cmd[0] = ipc_header(id, 1, 0);
  cmd[1] = (u32)res;
}

static bool ensure_shmem(void) {
  if (shmem)
    return true;
  shmem = page_alloc();
  if (!shmem)
    return false;
  shmem_pa = sup_pa(shmem);
  shmem_block = obj_new(OBJ_MEMBLOCK);
  if (!shmem_block)
    return false;
  shmem_block->u.mb.pa = shmem_pa;
  shmem_block->u.mb.size = 0x1000u;
  shmem_block->refs = 1;
  return true;
}

static void gsp_request(Thread *t, Object *session, u32 *cmd) {
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x16:
  case 0x17:
  case 0x0B:
  case 0x11:
  case 0x14:
  case 0x19:
  case 0x1A:
  case 0x1B:
  case 0x1C:
    if (id == 0x14 && client_event) {
      obj_unref(client_event);
      client_event = NULL;
    }
    reply(cmd, id, 0);
    return;
  case 0x13: {
    Object *ev = handle_obj(cmd[3]);
    if (!ensure_shmem() || !ev || ev->type != OBJ_EVENT) {
      reply(cmd, id, ev ? (s32)RES_OUT_OF_MEMORY : (s32)RES_INVALID_HANDLE);
      return;
    }
    if (client_event)
      obj_unref(client_event);
    client_event = ev;
    obj_ref(ev);
    u32 h = handle_new(shmem_block);
    cmd[0] = ipc_header(id, 2, 2);
    cmd[1] = 0;
    cmd[2] = 0;
    cmd[3] = 0;
    cmd[4] = h;
    return;
  }
  case 0x05: {
    u32 screen = cmd[1] & 1u;
    shown[screen].fb0 = cmd[3];
    shown[screen].fb1 = cmd[4];
    shown[screen].stride = cmd[5];
    shown[screen].format = cmd[6];
    shown[screen].valid = true;
    serial[screen]++;
    reply(cmd, id, 0);
    return;
  }
  case 0x08:
  case 0x09:
    cache_clean_all();
    reply(cmd, id, 0);
    return;
  case 0x0C:
    gx_queue();
    reply(cmd, id, 0);
    return;
  case 0x01:
  case 0x02:
  {
    static bool said;
    if (!said) {
      said = true;
      log_printf("GSP: register writes (0x%X) ignored\n", cmd[1]);
    }
    reply(cmd, id, 0);
    return;
  }
  case 0x04: {
    u32 size, *buf = static_buffer(t, 0, &size);
    if (buf)
      memset(buf, 0, cmd[2] < size ? cmd[2] : size);
    cmd[0] = ipc_header(id, 1, 2);
    cmd[1] = 0;
    cmd[2] = (cmd[2] << 14) | 2u;
    cmd[3] = (u32)(uintptr_t)buf;
    return;
  }
  case 0x18: {
    cmd[0] = ipc_header(id, 9, 0);
    cmd[1] = 0;
    for (int s = 0; s < 2; s++) {
      cmd[2 + s * 4] = shown[s].fb0;
      cmd[3 + s * 4] = shown[s].fb1 ? shown[s].fb1 : shown[s].fb0;
      cmd[4 + s * 4] = shown[s].format;
      cmd[5 + s * 4] = shown[s].stride;
    }
    return;
  }
  case 0x12:
    ipc_unknown("gsp::Gpu", cmd);
    return;
  default:
    ipc_unknown("gsp::Gpu", cmd);
  }
}

const Service gsp_service = {"gsp::Gpu", gsp_request, NULL};

void gsp_reset(void) {
  shmem = NULL;
  shmem_block = NULL;
  client_event = NULL;
  memset(shown, 0, sizeof shown);
  memset(submitted,0,sizeof submitted);
  memset(completed,0,sizeof completed);
  memset(last_gx,0,sizeof last_gx);
  memset(output_samples,0,sizeof output_samples);
  memset(fb_logged,0,sizeof fb_logged);
  memset(&rebase,0,sizeof rebase);
  rebase_logged=false;
  gsp_gx_forget();
  async_started=async_overlapped=0;
  irq_overflows=0;
  serial[0] = serial[1] = 0;
}

/* The cheat overlay draws into our private snapshot buffers with the CPU.
 * It never touches game VRAM, because the old overlay did, and that was bad. */
void gsp_overlay_frame(void) {
  const u32 alias=0x1D800000u;
  if(!mmu_map(alias,0x23E80000u,0x100000u,MAP_PRIV|MAP_RO))
    sup_fail("Overlay source mapping failed.");
  u32 top=panel_pa(0), bottom=panel_pa(1);
  memcpy((void *)(uintptr_t)top,(void *)(uintptr_t)alias,TOP_BYTES);
  memcpy((void *)(uintptr_t)bottom,(void *)(uintptr_t)(alias+0x80000u),BOT_BYTES);
  mmu_unmap(alias,0x100000u);
  gpu_scanout(top,bottom);
  display_slot[0]=(display_slot[0]+1u)%3u;
  display_slot[1]=(display_slot[1]+1u)%3u;
}
void gsp_overlay_end(void) {
  P->top.pa=P->bottom.pa=0;
  gsp_present();
}
