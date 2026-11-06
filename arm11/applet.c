#include "sup.h"
#include "applet.h"

#define APT_REQUEST         2u
#define APT_RESPONSE        3u
#define APT_SYSAPPLET_REQ   16u
#define APT_WAKEUP_EXIT     10u

#define APPID_SWKBD 0x401u
#define APPID_ERROR 0x406u

#include "applet_layout.h"

static JobApplet io __attribute__((aligned(64)));
static u8 param[0x1000] __attribute__((aligned(8)));
static u32 param_size, param_appid;
static Object *shared;

static struct { bool full; u32 sender, command, size; u8 data[0x1000]; } queued;
static Object *resume_event;

void applet_reset(void) {
  memset(&queued, 0, sizeof queued);
  resume_event = NULL;
  shared = NULL;
}

void applet_set_resume_event(Object *ev) { resume_event = ev; if (ev) ev->who = "APT parameter"; }

static void post(u32 sender, u32 command, const void *data, u32 size) {
  queued.full = true;
  queued.sender = sender;
  queued.command = command;
  queued.size = size > sizeof queued.data ? sizeof queued.data : size;
  if (data && queued.size) memcpy(queued.data, data, queued.size);
  if (resume_event) event_signal(resume_event);
}

void applet_launch_wakeup(void) { post(0x101u, 1u, NULL, 0); }

bool applet_is_library(u32 appid) { return appid >= 0x400u && appid < 0x500u; }

void applet_send(u32 dest, u32 command) {
  if (applet_is_library(dest) && (command == APT_REQUEST || command == APT_SYSAPPLET_REQ))
    post(dest, APT_RESPONSE, NULL, 0);
}

bool applet_take(bool remove, u32 *sender, u32 *command, const u8 **data, u32 *size) {
  if (!queued.full) return false;
  *sender = queued.sender; *command = queued.command; *data = queued.data; *size = queued.size;
  if (remove) queued.full = false;
  return true;
}

static void to_utf8(char *out, u32 cap, const u16 *in, u32 units) {
  u32 n = 0;
  for (u32 i = 0; i < units && in[i]; i++) {
    u32 c = in[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < units && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
      c = 0x10000u + ((c - 0xD800u) << 10) + (in[++i] - 0xDC00u);
    char b[4]; u32 k;
    if (c < 0x80) b[0] = (char)c, k = 1;
    else if (c < 0x800) b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), k = 2;
    else if (c < 0x10000) b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 63)), b[2] = (char)(0x80 | (c & 63)), k = 3;
    else b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 63)), b[2] = (char)(0x80 | ((c >> 6) & 63)), b[3] = (char)(0x80 | (c & 63)), k = 4;
    if (n + k >= cap) break;
    for (u32 j = 0; j < k; j++) out[n++] = b[j];
  }
  out[n] = 0;
}

static u32 to_utf16(u16 *out, u32 units, const char *in) {
  u32 n = 0;
  const u8 *s = (const u8 *)in;
  while (*s && n + 1 < units) {
    u32 c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) c = (c & 7) << 18 | (s[0] & 63u) << 12 | (s[1] & 63u) << 6 | (s[2] & 63u), s += 3;
    else if (c >= 0xE0 && s[0] && s[1]) c = (c & 15) << 12 | (s[0] & 63u) << 6 | (s[1] & 63u), s += 2;
    else if (c >= 0xC0 && s[0]) c = (c & 31) << 6 | (s[0] & 63u), s += 1;
    if (c >= 0x10000) {
      if (n + 2 >= units) break;
      c -= 0x10000;
      out[n++] = (u16)(0xD800 + (c >> 10));
      out[n++] = (u16)(0xDC00 + (c & 0x3FF));
    } else out[n++] = (u16)c;
  }
  out[n] = 0;
  return n;
}

static u8 *shared_bytes(u32 *size) {
  *size = 0;
  if (!shared || shared->type != OBJ_MEMBLOCK || !shared->u.mb.cva) return NULL;
  if (!mmu_user_write_ok(shared->u.mb.cva, shared->u.mb.size)) return NULL;
  *size = shared->u.mb.size;
  return (u8 *)(uintptr_t)shared->u.mb.cva;
}

static void finish(Thread *t);

bool applet_start(Thread *t, u32 appid, const void *data, u32 size, Object *mem) {
  param_appid = appid;
  param_size = size > sizeof param ? sizeof param : size;
  memset(param, 0, sizeof param);
  if (data) memcpy(param, data, param_size);
  if (shared) obj_unref(shared);
  shared = mem;
  if (shared) obj_ref(shared);
  memset(&io, 0, sizeof io);
  io.pressed = -1;

  if (appid == APPID_SWKBD && param_size >= sizeof(Swkbd)) {
    const Swkbd *k = (const Swkbd *)(void *)param;
    io.kind = APPLET_KEYBOARD;
    io.max_len = k->max_text_len ? k->max_text_len : 255u;
    io.buttons = (u32)k->num_buttons_m1 + 1u;
    if (io.buttons < 1 || io.buttons > 3) io.buttons = 2;
    io.password = k->password_mode != 0;
    io.numpad = k->type == 2;
    io.valid = (u32)k->valid_input;
    to_utf8(io.hint, sizeof io.hint, k->hint_text, 65);
    for (u32 b = 0; b < 3; b++) to_utf8(io.button[b], sizeof io.button[b], k->button_text[b], 17);
    u32 mem_size;
    u8 *mem_bytes = shared_bytes(&mem_size);
    if (mem_bytes && k->initial_text_offset >= 0 && (u32)k->initial_text_offset + 2u <= mem_size)
      to_utf8(io.text, sizeof io.text, (const u16 *)(void *)(mem_bytes + k->initial_text_offset),
              (mem_size - (u32)k->initial_text_offset) / 2u);
  } else if (appid == APPID_ERROR && param_size >= sizeof(ErrConf)) {
    const ErrConf *e = (const ErrConf *)(void *)param;
    io.kind = APPLET_ERROR;
    io.buttons = 1;
    io.error_code = (u32)e->error_code;
    if ((e->type & 0xFF) != ET_CODE) to_utf8(io.text, sizeof io.text, e->text, 1900);
  } else {
    log_printf("APT: library applet 0x%X not provided; closed at once\n", appid);
    post(appid, APT_WAKEUP_EXIT, param, param_size);
    return false;
  }
  P->applet_pa = (u32)(uintptr_t)&io;
  cache_clean_all();
  arm9_request(t, finish);
  P->yield = YIELD_APPLET;
  return true;
}

static void finish(Thread *t) {
  u32 *cmd = (u32 *)(uintptr_t)(t->ctx.tls + 0x80u);
  cache_clean_all();
  if (io.kind == APPLET_KEYBOARD) {
    Swkbd *k = (Swkbd *)(void *)param;
    u32 n = (u32)(io.buttons > 0 ? io.buttons : 1);
    int b = io.pressed < 0 ? 0 : io.pressed >= (int)n ? (int)n - 1 : io.pressed;
    k->result = n == 1 ? SR_D0_CLICK : n == 2 ? (SwkbdResult)(SR_D1_CLICK0 + b) : (SwkbdResult)(SR_D2_CLICK0 + b);
    u32 mem_size;
    u8 *mem_bytes = shared_bytes(&mem_size);
    u32 units = k->max_text_len ? (u32)k->max_text_len + 1u : 256u;
    if (mem_bytes && units * 2u > mem_size) units = mem_size / 2u;
    if (mem_bytes && units) {
      k->text_length = (u16)to_utf16((u16 *)(void *)mem_bytes, units, io.text);
      k->text_offset = 0;
    } else {
      k->text_length = 0;
      k->text_offset = 0;
    }
  } else if (io.kind == APPLET_ERROR) {
    ((ErrConf *)(void *)param)->return_code = ER_SUCCESS;
  }
  post(param_appid, APT_WAKEUP_EXIT, param, param_size);
  if (shared) { obj_unref(shared); shared = NULL; }
  cmd[0] = ipc_header(0x1E, 1, 0);
  cmd[1] = 0;
}
