#include <aurora_app.h>
#include "job.h"
#include "ui.h"
#include "glue.h"
#include <stdio.h>
#include <string.h>

extern void os_cache_sync(void);
extern bool runner_job(uint32_t cmd);
extern bool runner_overlay_frame(void);

static bool alive;
static void present(void) { if (alive && !runner_overlay_frame()) alive = false; }
static uint32_t read_input(void) {
  if (!alive) return KEY_START;
  uint32_t down = hid_keys_down();
  if (down & KEY_CPAD_UP) down |= KEY_UP;
  if (down & KEY_CPAD_DOWN) down |= KEY_DOWN;
  if (down & KEY_CPAD_LEFT) down |= KEY_LEFT;
  if (down & KEY_CPAD_RIGHT) down |= KEY_RIGHT;
  return down;
}
static const UiIo game_io = {read_input, present};

bool applet_show(JobParams *job) {
  JobApplet *a = (JobApplet *)(uintptr_t)job->applet_pa;
  os_cache_sync();
  if (!a || !runner_job(JOB_OVERLAY_BEGIN)) return false;
  glue_overlay_begin(job->top.pa, job->bottom.pa);
  alive = true;
  gfx_rect_blend(SCREEN_TOP, 0, 0, 400, 240, RGB(0, 0, 0), 120);
  if (a->kind == APPLET_KEYBOARD) {
    UiKeyboard k = {
      .title = a->numpad ? "Enter a number" : "Enter text",
      .hint = a->hint,
      .button = {a->button[0], a->button[1], a->button[2]},
      .buttons = (int)a->buttons,
      .max_chars = a->max_len ? a->max_len : 255,
      .valid = a->valid,
      .password = a->password != 0,
      .digits = a->numpad != 0,
      .dim_top = true,
    };
    char text[APPLET_TEXT];
    snprintf(text, sizeof text, "%s", a->text);
    a->pressed = ui_keyboard_run(&k, text, sizeof text, &game_io);
    snprintf(a->text, sizeof a->text, "%s", text);
  } else {
    char message[APPLET_TEXT + 64];
    if (a->text[0]) snprintf(message, sizeof message, "%s", a->text);
    else snprintf(message, sizeof message, "Error code %08lX", (unsigned long)a->error_code);
    ui_message_run("The game reported an error", message, &game_io);
    a->pressed = 0;
  }
  os_cache_sync();
  bool ok = alive && runner_job(JOB_OVERLAY_END) && job->yield != YIELD_ERROR && job->yield != YIELD_CRASH;
  os_cache_sync();
  return ok;
}
