#include "aurora.h"
#include "assets.h"
#include "gpu.h"
#include "system_menu.h"

#include "glue.h"
#include <string.h>

bool glue_gpu(void) { return gpu_alive() != 0; }

void glue_loading(bool active) {
  extern void app_gfx_direct(bool on);
  app_gfx_direct(active);
  glue_guest_display(active);
}

void glue_loading_present(void) {
  screen_present_top();
  screen_present_bottom();
}

void glue_guest_display(bool owned) {
  static uint32_t saved_active;
  static bool was_owned;
  if (owned == was_owned) return;
  was_owned = owned;
  volatile AuroraSystemMenuShared *menu=aurora_system_menu_shared();
  extern void os_cache_sync(void);
  os_cache_sync();
  if (owned) { saved_active=menu->app_active; menu->app_active=0; }
  else menu->app_active=saved_active;
  os_cache_sync();
}

bool glue_copy(uint32_t src, uint32_t dst, uint32_t bytes) { return gpu_texcopy(src, dst, bytes) != 0; }

bool glue_font(uint32_t *head, uint32_t *glyphs, uint32_t *atlas) {
  Font f;
  if (!assets_ok() && !assets_read())
    return false;
  if (!asset_font(ASSET_FONT_UI, &f) || !f.head || !f.glyphs || !f.atlas)
    return false;
  *head = (uint32_t)f.head;
  *glyphs = (uint32_t)f.glyphs;
  *atlas = (uint32_t)f.atlas;
  return true;
}

void glue_overlay_begin(uint32_t top,uint32_t bottom) {
  extern void os_cache_sync(void);
  gpu_wait_idle(); os_cache_sync();
  g_fb_top=VRAM_TOP_BACK; g_fb_bot=VRAM_BOT_BACK;
  g_blit_src=0;
  memcpy((void *)VRAM_TOP_BACK,(const void *)(uintptr_t)top,TOP_FB_SIZE);
  memcpy((void *)VRAM_BOT_BACK,(const void *)(uintptr_t)bottom,BOT_FB_SIZE);
}
