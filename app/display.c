#include "aurora.h"
#include "gpu.h"
#include "glue.h"

void glue_clear_display(void) {
  static const uint32_t top[] = {
    GPU_FB_TOP_A, GPU_FB_TOP_B, GPU_FB_TOP_C,
    GPU_FB_TOP_RA, GPU_FB_TOP_RB, GPU_FB_TOP_RC
  };
  static const uint32_t bottom[] = {GPU_FB_BOT_A, GPU_FB_BOT_B, GPU_FB_BOT_C};
  extern void os_cache_sync(void);
  Color black = {0,0,0};
  gpu_wait_idle();
  os_cache_sync();
  for (unsigned i=0; i<sizeof top/sizeof top[0]; i++)
    clear_screen((volatile uint8_t *)(uintptr_t)top[i],TOP_FB_SIZE,black);
  for (unsigned i=0; i<sizeof bottom/sizeof bottom[0]; i++)
    clear_screen((volatile uint8_t *)(uintptr_t)bottom[i],BOT_FB_SIZE,black);
  clear_screen(VRAM_TOP_BACK,TOP_FB_SIZE,black);
  clear_screen(VRAM_BOT_BACK,BOT_FB_SIZE,black);
  os_cache_sync();
}
