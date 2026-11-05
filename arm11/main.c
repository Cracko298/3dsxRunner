#include "sup.h"

extern u8 __bss_start[], __bss_end[];
static bool loaded;

/* The .3dsx file is mapped by whole megabytes, and that megabyte can be
 * shared with other ARM9 allocations. So the display mapping goes in only
 * after this one is gone. Order matters; I'm not happy about it either. */
static void map_file(bool on) {
  u32 base = P->file_pa & ~0xFFFFFu;
  u32 size = ((P->file_pa + P->file_size + 0xFFFFFu) & ~0xFFFFFu) - base;
  if (on)
    mmu_map(base, base, size, MAP_PRIV | MAP_RO);
  else
    mmu_unmap(base, size);
}

static bool region_ok(const JobRegion *r) {
  return r->pa >= 0x20000000u && r->size >= 0x100000u && !((r->pa | r->size) & 0xFFFFFu) &&
         r->pa + r->size <= 0x30000000u && r->pa + r->size > r->pa;
}

static bool overlaps(u32 a,u32 size,u32 b,u32 bytes) {
  return (u64)a+size>b && (u64)b+bytes>a;
}

static bool display_ok(void) {
  const JobRegion *r=&P->display;
  return r->pa>=0x20000000u && r->size==DISPLAY_POOL_SIZE && !(r->pa&0xFFFu) &&
    (u64)r->pa+r->size<=0x28000000u &&
    !overlaps(r->pa,r->size,JOB_PARAMS,SUP_END-JOB_PARAMS) &&
    !overlaps(r->pa,r->size,P->code.pa,P->code.size) &&
    !overlaps(r->pa,r->size,P->linear.pa,P->linear.size) &&
    !overlaps(r->pa,r->size,P->file_pa,P->file_size);
}

static void job_load(void) {
  mmu_leave();
  memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
  P->yield = YIELD_NONE;
  P->error[0] = 0;
  P->log_head = 0;
  P->svc_count = P->ipc_count = P->gx_count = P->threads = 0;
  P->top.pa = P->bottom.pa = P->top_right_pa = 0;
  P->close_files = P->close_dirs = 0;
  if (!region_ok(&P->code) || !region_ok(&P->linear) || !display_ok() ||
      P->file_pa < 0x20000000u || P->file_pa + P->file_size > 0x28000000u) {
    snprintf(P->error, sizeof P->error, "The memory given to the program is not usable.");
    P->yield = YIELD_ERROR;
    return;
  }
  mmu_reset();
  if (!mmu_base()) {
    snprintf(P->error, sizeof P->error, "The supervisor's tables could not be made.");
    P->yield = YIELD_ERROR;
    return;
  }
  map_file(true);
  mmu_tables_on();
  kernel_init();
  ipc_reset();
  gsp_reset();
  hid_reset();
  fs_reset();
  misc_reset();
  dsp_reset();
  gpu_init();
  bool ok = load_3dsx();
  map_file(false);

  if (ok && !mmu_map(P->display.pa,P->display.pa,P->display.size,MAP_PRIV)) {
    snprintf(P->error,sizeof P->error,"The display buffers could not be mapped.");
    ok=false;
  }
  loaded = ok;
  P->yield = ok ? YIELD_NONE : YIELD_ERROR;
}

void job_main(u32 arg) {
  (void)arg;
  if (P->magic != JOB_MAGIC || P->version != JOB_VERSION)
    return;

  P->done = P->cmd;
  if (P->cmd != JOB_LOAD) gpu_job_entry();

  if (loaded && P->cmd != JOB_STOP && P->cmd != JOB_LOAD) {
    if (P->cmd == JOB_RUN || P->cmd == JOB_PRESENT) gsp_gx_poll();
    else gsp_gx_drain();
  }
  switch (P->cmd) {
  case JOB_LOAD:
    job_load();
    break;
  case JOB_RUN:
    if (!loaded || P->yield == YIELD_EXIT || P->yield == YIELD_CRASH || P->yield == YIELD_ERROR) {
      if (!loaded && P->yield != YIELD_ERROR) {
        snprintf(P->error, sizeof P->error, "Nothing is loaded.");
        P->yield = YIELD_ERROR;
      }
      break;
    }
    shared_page_update();
    job_run();
    break;
  case JOB_STOP:
    gsp_gx_settle();
    gsp_report();
    gpu_report();
    gpu_scanout_report();
    dsp_stop();
    gpu_abort_transfers();
    gpu_scanout_restore();
    gpu_restore_config();
    loaded = false;
    break;
  case JOB_CHEATS:
    if(loaded && P->plugin_enabled) cheats_take();
    else P->mem.result=(s32)RES_UNIMPL;
    break;
  case JOB_MEM:
    if(loaded && P->plugin_enabled) mem_do();
    else P->mem.result=(s32)RES_UNIMPL;
    break;
  case JOB_OVERLAY_BEGIN:
    if(loaded && P->plugin_enabled) dsp_overlay_pause(true);
    break;
  case JOB_OVERLAY_FRAME:
    if(loaded && P->plugin_enabled) gsp_overlay_frame();
    break;
  case JOB_OVERLAY_END:
    if(loaded && P->plugin_enabled) {
      gsp_overlay_end();
      dsp_overlay_pause(false);
    }
    break;
  case JOB_PRESENT:
    if (loaded) gsp_present();
    break;
  }
  P->done = P->cmd;
}
