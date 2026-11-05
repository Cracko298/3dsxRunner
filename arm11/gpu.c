#include "sup.h"

#define GPU_BASE 0x10400000u
#define GX(off)  IO(GPU_BASE + (off))
#define P3D(reg) IO(GPU_BASE + 0x1000u + (reg) * 4u)

#define GX_CLOCK    0x0004u
#define GX_PSC0     0x0010u
#define GX_PSC1     0x0020u
#define GX_IRQ_STAT 0x0034u
#define STAT_P3D    (1u << 31)
#define PSC_GO      (1u << 0)
#define PSC_DONE    (1u << 1)
#define PPF_IN      0x0C00u
#define PPF_OUT     0x0C04u
#define PPF_OUTDIM  0x0C08u
#define PPF_INDIM   0x0C0Cu
#define PPF_FLAGS   0x0C10u
#define PPF_UNK     0x0C14u
#define PPF_CNT     0x0C18u
#define PPF_LEN     0x0C20u
#define PPF_INLINE  0x0C24u
#define PPF_OUTLINE 0x0C28u
#define PPF_GO      (1u << 0)
#define PPF_DONE    (1u << 8)

#define CFG11_GPUPROT 0x10140140u
#define CFG11_GPU_N3DS_CNT 0x10140400u
#define CFG11_SOCMODE 0x10141300u
#define IO8(pa) (*(volatile u8 *)(uintptr_t)((pa)-PA_IO+VA_IO))
#define IO16(pa) (*(volatile u16 *)(uintptr_t)((pa)-PA_IO+VA_IO))

#define REG_IRQ_ACK      0x000u
#define REG_IRQ_CMP      0x020u
#define REG_IRQ_MASK_LO  0x030u
#define REG_IRQ_MASK_HI  0x031u
#define REG_IRQ_AUTOSTOP 0x034u
#define REG_CMDBUF_SIZE0 0x238u
#define REG_CMDBUF_ADDR0 0x23Au
#define REG_CMDBUF_JUMP0 0x23Cu
#define REG_START_DRAW   0x245u

#define POLLS 8000000u

static inline void dsb(void) { __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory"); }

static bool ready;
static bool config_owned;
static u16 saved_protection;
static u8 saved_n3ds;

/* CPU access to upper FCRAM and GPU access to it are two different switches.
 * Aurora's clock code skips setting the GPU one if the clock was already
 * fast, so we set it ourselves instead of trusting whatever we inherited. */
void gpu_init(void) {
  if (ready)
    return;

  saved_protection = IO16(CFG11_GPUPROT);
  saved_n3ds = P->new3ds ? IO8(CFG11_GPU_N3DS_CNT) : 0;
  config_owned = true;
  if (P->new3ds) {
    u32 mode = (IO(CFG11_SOCMODE) >> 16) & 7u;
    if (!mode) sup_fail("New 3DS extended memory mode was not enabled.");
    IO8(CFG11_GPU_N3DS_CNT) = saved_n3ds | 3u;
    dsb();
    if ((IO8(CFG11_GPU_N3DS_CNT) & 3u) != 3u)
      sup_fail("GPU extended-memory configuration did not apply.");
    log_printf("GPU memory: mode=%u N3DS=%02X->%02X prot=%04X->0000\n",
               mode,saved_n3ds,IO8(CFG11_GPU_N3DS_CNT),saved_protection);
  } else log_printf("GPU memory: Old 3DS prot=%04X->0000\n",saved_protection);
  IO16(CFG11_GPUPROT) = 0;
  GX(GX_CLOCK) = 0x00070100u;
  GX(0x0050u) = 0x22221200u;
  GX(0x0054u) = (GX(0x0054u) & ~0xFFFFu) | 0xFF2u;
  P3D(REG_IRQ_ACK) = 0;
  P3D(REG_IRQ_CMP) = 0x12345678u;
  P3D(REG_IRQ_MASK_LO) = 0xFFFFFFF0u;
  P3D(REG_IRQ_MASK_HI) = 0xFFFFFFFFu;
  P3D(REG_IRQ_AUTOSTOP) = 1;
  /* The very first command list hangs the GPU unless it starts in config
   * mode. Ask me how I know. (Don't.) */
  P3D(REG_START_DRAW) = 1;
  dsb();
  ready = true;
}

void gpu_restore_config(void) {
  if (!config_owned || P->display_pending) return;
  IO16(CFG11_GPUPROT) = saved_protection;
  if (P->new3ds) IO8(CFG11_GPU_N3DS_CNT) = saved_n3ds;
  dsb();
  config_owned = false;
  ready = false;
}

void gpu_report(void) {
  log_printf("GPU config: pdn=%08X clock=%08X vram=%08X N3DS=%02X inherited=%02X prot=%04X\n",
             IO(0x10141200u),GX(GX_CLOCK),GX(0x8),
             P->new3ds?IO8(CFG11_GPU_N3DS_CNT):0,saved_n3ds,IO16(CFG11_GPUPROT));
  log_printf("GPU regs: irq=%08X fill=%08X/%08X ppf=%08X list=%08X/%X\n",
             GX(GX_IRQ_STAT),GX(GX_PSC0+0xC),GX(GX_PSC1+0xC),GX(PPF_CNT),
             P3D(REG_CMDBUF_ADDR0)<<3,P3D(REG_CMDBUF_SIZE0)<<3);
}

/* The game owns all of VRAM, and Aurora's normal present writes right into
 * it. So we never let the LCD look at game memory directly: every frame is
 * copied into our own buffers in FCRAM and *those* get scanned out. */
static bool display_owned;
static const u32 display_offsets[] = {0x68,0x6C,0x70,0x78,0x90,0x94,0x98};
static u32 display_saved[2][7];

#define PDC_FMT_COLOR_MASK  0x007u
#define PDC_FMT_OUTPUT_MASK 0x070u
#define PDC_FMT_BURST_MASK  0x300u
#define PDC_FMT_BGR8        0x001u
#define PDC_FMT_TOP_2D      0x040u
#define PDC_BURST_FCRAM     0x100u
#define PDC_BURST_VRAM      0x300u

/* Reading the address register back does NOT mean the LCD switched to it.
 * It can take a full refresh to latch, so on exit we keep game memory alive
 * until a whole frame has gone by. Freeing it early = bus lockups. Fun. */
static bool lcd_blank(u32 base, bool next) {
  if (!(GX(base+0x74)&1u)) return true;
  u32 end=GX(base+0x64)>>16, total=GX(base+0x24);
  if (!end || end>total || total>0xFFFFu) return false;
  bool active=!next;
  for (u32 polls=0;polls<2000000u;polls++) {
    u32 line=GX(base+0x54);
    if (line<end) active=true;
    else if (line<=total && active) return true;
    if (!(polls&1023u) && P->cmd!=JOB_STOP) job_check_cancel();
  }
  return false;
}

/* Pixel format alone isn't enough when you move scanout from VRAM to FCRAM:
 * the burst mode has to change too, because the LCD can't do VRAM-sized
 * bursts out of FCRAM. r3 learned that one the hard way. */
static u32 scanout_format(u32 saved, u32 pa, u32 screen) {
  u32 burst = pa >= PA_VRAM && pa < PA_VRAM + VRAM_SIZE ? PDC_BURST_VRAM : PDC_BURST_FCRAM;
  return (saved & ~(PDC_FMT_COLOR_MASK | PDC_FMT_OUTPUT_MASK | PDC_FMT_BURST_MASK)) |
         PDC_FMT_BGR8 | (screen ? 0u : PDC_FMT_TOP_2D) | burst;
}

/* Triple buffering is only safe if every commit has a full active scan
 * between it and the previous one. r13 proved that by sitting there and
 * watching an entire refresh go by, every single frame, on the core the game
 * needs. Most of the time we already know a frame has started since the last
 * commit, either from the ARM9's clock or because we spotted an active line
 * during a kernel call, and then we can commit right away.
 *
 * The time we compare against is when the commit's job was seen to *finish*,
 * which is never earlier than the commit itself, so we can only ever
 * underestimate. 20 ms covers anything refreshing at 50 Hz or faster. */
#define LCD_MAX_FRAME_US 20000u
static bool stamp_pending[2], stamp_ok[2], active_seen[2];
static u64 commit_post_us[2], commit_done_us[2];
static u32 fast_commits, strict_commits;

static bool lcd_timing(u32 base, u32 *end, u32 *total) {
  *end=GX(base+0x64)>>16; *total=GX(base+0x24);
  return *end && *end<=*total && *total<=0xFFFFu;
}

bool gpu_lcd_watch;

void gpu_lcd_sample(void) {
  bool watch=false;
  if (!display_owned) { gpu_lcd_watch=false; return; }
  for (u32 s=0;s<2;s++) {
    u32 base=0x400u+s*0x100u, end, total;
    if (!active_seen[s] && (!(GX(base+0x74)&1u) ||
                            (lcd_timing(base,&end,&total) && GX(base+0x54)<end)))
      active_seen[s]=true;
    if (!active_seen[s]) watch=true;
  }
  gpu_lcd_watch=watch;
}

void gpu_job_entry(void) {
  if (!display_owned) return;
  for (u32 s=0;s<2;s++) if (stamp_pending[s]) {
    stamp_pending[s]=false;
    u64 done=P->prev_done_us;
    stamp_ok[s]=done && done>=commit_post_us[s];
    commit_done_us[s]=done;
  }
  gpu_lcd_sample();
}

static bool frame_started_since_commit(u32 s, u32 base) {
  if (active_seen[s]) return true;
  u32 end, total;
  if (!stamp_ok[s] || !P->wall_us || P->wall_us<commit_done_us[s] || !lcd_timing(base,&end,&total))
    return false;
  u64 blank_us=(u64)LCD_MAX_FRAME_US*(total+1u-end)/(total+1u)+1000u;
  return P->wall_us-commit_done_us[s]>blank_us;
}

static void committed(u32 s, bool fast) {
  stamp_pending[s]=true;
  stamp_ok[s]=false;
  active_seen[s]=false;
  gpu_lcd_watch=true;
  commit_post_us[s]=P->wall_us;
  if (fast) fast_commits++; else strict_commits++;
}

void gpu_scanout_report(void) {
  log_printf("LCD commits: fast=%u strict=%u\n",fast_commits,strict_commits);
}

void gpu_scanout(u32 top, u32 bottom) {
  bool first=!display_owned;
  if (!display_owned) {
    for (u32 s=0;s<2;s++) for (u32 i=0;i<(s?5u:7u);i++)
      display_saved[s][i]=GX(0x400u+s*0x100u+display_offsets[i]);
    log_printf("LCD: saved top=%08X bottom=%08X; RAM burst mode enabled\n",
               display_saved[0][2],display_saved[1][2]);
    display_owned=true;
    P->display_pending=1;
  }
  cache_clean_all();

  bool active[2]={false,false}, known[2]={false,false};
  for (u32 s=0;s<2 && !first;s++)
    known[s]=active[s]=frame_started_since_commit(s,0x400u+s*0x100u);
  u32 pending=3u;
  for (u32 polls=0;pending && polls<2000000u;polls++) {
    if (!(polls&1023u)) job_check_cancel();
    for (u32 s=0;s<2;s++) {
      if (!(pending&(1u<<s))) continue;
      u32 base=0x400u+s*0x100u, pa=s?bottom:top;
      if (!pa) { pending&=~(1u<<s); continue; }
      if (GX(base+0x74)&1u) {
        u32 end=GX(base+0x64)>>16, total=GX(base+0x24), line=GX(base+0x54);
        if (!end || end>total || total>0xFFFFu)
          sup_fail("LCD timing is not usable for presentation.");
        if (line<end) active[s]=true;
        if (!active[s] || line<end || line>total) continue;
      }
      u32 format=scanout_format(display_saved[s][2],pa,s);
      GX(base+0x68)=pa; GX(base+0x6C)=pa;
      if (!s) { GX(base+0x94)=pa; GX(base+0x98)=pa; }
      GX(base+0x90)=720;

      GX(base+0x70)=format;
      GX(base+0x78)=((GX(base+0x78)&1u)^1u)|(7u<<16);
      pending&=~(1u<<s);
      committed(s,known[s]);
    }
  }
  if (pending) sup_fail("LCD did not reach a fresh blank for presentation.");
  dsb();
}

void gpu_scanout_restore(void) {
  if (!display_owned) return;
  for (u32 s=0;s<2;s++) {
    u32 base=0x400u+s*0x100u;
    if (!lcd_blank(base,false)) {
      log_printf("LCD %u: restore waiting for blank; retaining guest memory\n",s);
      return;
    }
    for (u32 i=0;i<(s?5u:7u);i++) if (i!=3)
      GX(base+display_offsets[i])=display_saved[s][i];
    GX(base+0x78)=(display_saved[s][3]&1u)|(7u<<16);
  }
  dsb();

  for (u32 s=0;s<2;s++) if (!lcd_blank(0x400u+s*0x100u,true)) {
    log_printf("LCD %u: handoff not confirmed; retaining guest memory\n",s);
    return;
  }
  log_printf("LCD: Aurora buffers restored across a complete refresh\n");
  P->display_pending=0;
  display_owned=false;
}

/* Stop the fill and copy engines only. Stopping the LCD in the middle of an
 * FCRAM fetch can hang the whole bus, and there's no coming back from that
 * without holding the power button. */
void gpu_abort_transfers(void) {

  GX(GX_PSC0+0xC)=0;
  GX(GX_PSC1+0xC)=0;
  GX(PPF_CNT)=0;
  dsb();
  gsp_gx_forget();
}

/* If the GPU never finishes, we fail loudly. Pretending it finished just
 * shows a half-written framebuffer and sends everyone on a wild goose chase. */
static void timed_out(const char *what) __attribute__((noreturn));
static void timed_out(const char *what) {
  char reason[128];
  snprintf(reason,sizeof reason,"GPU timeout during %s.",what);
  log_printf("%s\n",reason);
  gpu_report();
  gpu_abort_transfers();
  dsp_stop();
  sup_fail(reason);
}

static void poll_job(u32 *polls, const char *what) {
  if (((++*polls)&1023u)==0) job_check_cancel();
  if (*polls>=POLLS) timed_out(what);
}

/* Every engine operation is split into start / done? / finish so GSP can
 * hand control back to the game while the GPU works. The plain versions
 * further down are the old "wait right here" behaviour built from the same
 * three pieces. */
void gpu_fill_start(int n, u32 start, u32 end, u32 value, u32 control) {
  u32 base = n ? GX_PSC1 : GX_PSC0;
  cache_clean_all();
  GX(base + 0xC) = GX(base + 0xC) & ~0xFFu;
  GX(base + 0x0) = start >> 3;
  GX(base + 0x4) = end >> 3;
  GX(base + 0x8) = value;
  dsb();
  GX(base + 0xC) = (control & 0xFFFFu) | PSC_GO;
  dsb();
}

bool gpu_fill_done(int n) {
  u32 cnt = GX((n ? GX_PSC1 : GX_PSC0) + 0xC);
  return (cnt & PSC_DONE) || !(cnt & PSC_GO);
}

void gpu_fill_finish(int n) {
  u32 base = n ? GX_PSC1 : GX_PSC0;
  GX(base + 0xC) = GX(base + 0xC) & ~0xFFu;
  dsb();
}

void gpu_fill(int n, u32 start, u32 end, u32 value, u32 control) {
  gpu_fill_start(n, start, end, value, control);
  u32 w = 0;
  while (!gpu_fill_done(n))
    poll_job(&w,"a memory fill");
  gpu_fill_finish(n);
}

static void ppf_go(void) {
  dsb();
  GX(PPF_CNT) = PPF_GO;
  dsb();
}

bool gpu_ppf_done(void) {
  u32 cnt = GX(PPF_CNT);
  return (cnt & PPF_DONE) || !(cnt & PPF_GO);
}

void gpu_ppf_finish(void) {
  GX(PPF_CNT) = GX(PPF_CNT) & ~PPF_DONE;
  dsb();
}

static void ppf_wait(const char *what) {
  u32 w = 0;
  while (!gpu_ppf_done())
    poll_job(&w,what);
  gpu_ppf_finish();
}

void gpu_transfer_start(u32 in, u32 out, u32 indim, u32 outdim, u32 flags) {
  cache_clean_all();
  GX(PPF_CNT) = GX(PPF_CNT) & ~0xFF00u;
  GX(PPF_IN) = in >> 3;
  GX(PPF_OUT) = out >> 3;
  GX(PPF_OUTDIM) = outdim;
  GX(PPF_INDIM) = indim;
  GX(PPF_FLAGS) = flags;
  GX(PPF_UNK) = 0;
  ppf_go();
}

void gpu_transfer(u32 in, u32 out, u32 indim, u32 outdim, u32 flags) {
  gpu_transfer_start(in, out, indim, outdim, flags);
  ppf_wait("a display transfer");
}

void gpu_texcopy_start(u32 in, u32 out, u32 size, u32 inline_, u32 outline, u32 flags) {
  cache_clean_all();
  GX(PPF_CNT) = GX(PPF_CNT) & ~0xFF00u;
  GX(PPF_IN) = in >> 3;
  GX(PPF_OUT) = out >> 3;
  GX(PPF_LEN) = size;
  GX(PPF_INLINE) = inline_;
  GX(PPF_OUTLINE) = outline;
  GX(PPF_FLAGS) = flags | (1u << 3);
  GX(PPF_UNK) = 0;
  ppf_go();
}

void gpu_texcopy(u32 in, u32 out, u32 size, u32 inline_, u32 outline, u32 flags) {
  gpu_texcopy_start(in, out, size, inline_, outline, flags);
  ppf_wait("a texture copy");
}

void gpu_cmdlist_start(u32 pa, u32 size) {
  if (!ready)
    gpu_init();
  cache_clean_all();
  P3D(REG_IRQ_ACK) = 0;
  dsb();
  u32 w = 0;
  while (GX(GX_IRQ_STAT) & STAT_P3D)
    poll_job(&w,"acknowledging a command list");
  P3D(REG_CMDBUF_SIZE0) = size >> 3;
  P3D(REG_CMDBUF_ADDR0) = pa >> 3;
  dsb();
  P3D(REG_CMDBUF_JUMP0) = 1;
  dsb();
}

bool gpu_cmdlist_done(void) { return (GX(GX_IRQ_STAT) & STAT_P3D) != 0; }

void gpu_cmdlist_finish(void) {
  P3D(REG_IRQ_ACK) = 0;
  dsb();
  (void)GX(GX_IRQ_STAT);
}

void gpu_cmdlist(u32 pa, u32 size) {
  gpu_cmdlist_start(pa, size);
  u32 w = 0;
  while (!gpu_cmdlist_done())
    poll_job(&w,"a command list");
  gpu_cmdlist_finish();
}

void gpu_async_timeout(const char *what) { timed_out(what); }
