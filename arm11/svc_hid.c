#include "sup.h"

static u32 *hid_mem, *irrst_mem;
static Object *hid_block, *irrst_block;
static Object *hid_events[5], *irrst_event;
static bool accel_on, gyro_on;
static void section_tick(u32 *s, u64 now);

static void motion_section(u32 *sec, u64 now, s16 x, s16 y, s16 z) {
  u32 idx = (sec[4] + 1u) & 7u;
  s16 *e = (s16 *)(void *)&sec[6];
  e[idx * 3] = x, e[idx * 3 + 1] = y, e[idx * 3 + 2] = z;
  section_tick(sec, now);
  sec[4] = idx;
}

#define KEY_TOUCH      (1u << 20)
#define KEY_CPAD_RIGHT (1u << 28)
#define KEY_CPAD_LEFT  (1u << 29)
#define KEY_CPAD_UP    (1u << 30)
#define KEY_CPAD_DOWN  (1u << 31)

static Object *block_for(u32 **mem) {
  if (!*mem) {
    *mem = page_alloc();
    if (!*mem)
      return NULL;
  }
  Object *o = obj_new(OBJ_MEMBLOCK);
  if (!o)
    return NULL;
  o->u.mb.pa = sup_pa(*mem);
  o->u.mb.size = 0x1000u;
  o->refs = 1;
  return o;
}

static void section_tick(u32 *s, u64 now) {
  s[2] = s[0], s[3] = s[1];
  s[0] = (u32)now, s[1] = (u32)(now >> 32) & 0x7FFFFFFFu;
}

void hid_update(void) {
  u64 now = ticks();
  s32 x = P->cpad_x, y = P->cpad_y;
  u32 held = P->held & ~(KEY_CPAD_RIGHT | KEY_CPAD_LEFT | KEY_CPAD_UP | KEY_CPAD_DOWN);
  if (x >= 41) held |= KEY_CPAD_RIGHT;
  if (x <= -41) held |= KEY_CPAD_LEFT;
  if (y >= 41) held |= KEY_CPAD_UP;
  if (y <= -41) held |= KEY_CPAD_DOWN;
  if (hid_mem) {
    u32 *pad = hid_mem;
    u32 idx = (pad[4] + 1u) & 7u, prev = pad[10 + pad[4] * 4];
    u32 *e = &pad[10 + idx * 4];
    e[0] = held & ~KEY_TOUCH;
    e[1] = e[0] & ~prev;
    e[2] = prev & ~e[0];
    e[3] = (u32)(u16)(s16)x | (u32)(u16)(s16)y << 16;
    pad[5] = e[3];
    section_tick(pad, now);
    pad[4] = idx;

    u32 *touch = &hid_mem[42];
    idx = (touch[4] + 1u) & 7u;
    u32 *t = &touch[8 + idx * 2];
    bool down = (P->held & KEY_TOUCH) != 0;
    t[0] = down ? ((P->touch_x & 0xFFFFu) | (P->touch_y & 0xFFFFu) << 16) : 0;
    t[1] = down ? 1u : 0u;
    section_tick(touch, now);
    touch[4] = idx;

    for (int i = 0; i < 2; i++)
      if (hid_events[i])
        event_signal(hid_events[i]);
    if (accel_on) {
      motion_section(&hid_mem[66], now, 0, 0, -512);
      if (hid_events[2]) event_signal(hid_events[2]);
    }
    if (gyro_on) {
      motion_section(&hid_mem[86], now, 0, 0, 0);
      if (hid_events[3]) event_signal(hid_events[3]);
    }
  }
  if (irrst_mem) {
    u32 idx = (irrst_mem[4] + 1u) & 7u, prev = irrst_mem[6 + irrst_mem[4] * 4];
    u32 *e = &irrst_mem[6 + idx * 4];
    s32 cx = P->new3ds ? P->cstick_x : 0, cy = P->new3ds ? P->cstick_y : 0;
    u32 keys = 0;
    if (P->new3ds) {
      if (P->zlzr & 1u) keys |= 1u << 14;
      if (P->zlzr & 2u) keys |= 1u << 15;
      if (cx >= 41) keys |= 1u << 24;
      if (cx <= -41) keys |= 1u << 25;
      if (cy >= 41) keys |= 1u << 26;
      if (cy <= -41) keys |= 1u << 27;
    }
    e[0] = keys;
    e[1] = keys & ~prev;
    e[2] = prev & ~keys;
    e[3] = (u32)(u16)(s16)cx | (u32)(u16)(s16)cy << 16;
    section_tick(irrst_mem, now);
    irrst_mem[4] = idx;
    if (irrst_event)
      event_signal(irrst_event);
  }
}

static void hid_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0xA: {
    if (!hid_block) {
      hid_block = block_for(&hid_mem);
      for (int i = 0; i < 5; i++)
        if ((hid_events[i] = obj_new(OBJ_EVENT)))
          hid_events[i]->refs = 1, hid_events[i]->who = "HID";
    }
    if (!hid_block) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_OUT_OF_MEMORY;
      return;
    }
    cmd[0] = ipc_header(id, 1, 7);
    cmd[1] = 0;
    cmd[2] = 5u << 26;
    cmd[3] = handle_new(hid_block);
    for (int i = 0; i < 5; i++)
      cmd[4 + i] = handle_new(hid_events[i]);
    hid_update();
    return;
  }
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
    if (id == 0x11 || id == 0x12) accel_on = id == 0x11;
    else gyro_on = id == 0x13;
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  case 0x15: {
    union { float f; u32 u; } c = {14.375f};
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = c.u;
    return;
  }
  case 0x16: {
    cmd[0] = ipc_header(id, 6, 0);
    cmd[1] = 0;
    for (int i = 2; i < 7; i++) cmd[i] = 0;
    return;
  }
  case 0x17:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 0x3F;
    return;
  default:
    ipc_unknown("hid:USER", cmd);
  }
}

static void irrst_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x1:
    if (!irrst_block) {
      irrst_block = block_for(&irrst_mem);
      if ((irrst_event = obj_new(OBJ_EVENT)))
        irrst_event->refs = 1, irrst_event->who = "ir:rst";
    }
    if (!irrst_block) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_OUT_OF_MEMORY;
      return;
    }
    cmd[0] = ipc_header(id, 1, 3);
    cmd[1] = 0;
    cmd[2] = 1u << 26;
    cmd[3] = handle_new(irrst_block);
    cmd[4] = handle_new(irrst_event);
    return;
  case 0x2:
  case 0x3:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  default:
    ipc_unknown("ir:rst", cmd);
  }
}

const Service hid_service = {"hid:USER", hid_request, NULL};
const Service irrst_service = {"ir:rst", irrst_request, NULL};

void hid_reset(void) {
  hid_mem = irrst_mem = NULL;
  hid_block = irrst_block = NULL;
  for (int i = 0; i < 5; i++)
    hid_events[i] = NULL;
  irrst_event = NULL;
  accel_on = gyro_on = false;
}
