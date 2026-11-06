#include "sup.h"

static Object *event;
static u8 gain = 0x28, power, clamp, sampling;

void mic_reset(void) {
  event = NULL;
  gain = 0x28; power = clamp = sampling = 0;
}

static void reply(u32 *cmd, u32 id, u32 words, u32 value) {
  cmd[0] = ipc_header(id, words, 0);
  cmd[1] = 0;
  cmd[2] = value;
}

static void mic_request(Thread *t, Object *session, u32 *cmd) {
  (void)t; (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x3:
    sampling = 1;
    if (event) event_signal(event);
    reply(cmd, id, 1, 0); return;
  case 0x5: sampling = 0; reply(cmd, id, 1, 0); return;
  case 0x6: reply(cmd, id, 2, sampling); return;
  case 0x7: {
    if (!event) { event = obj_new(OBJ_EVENT); if (event) event->refs = 1, event->who = "mic"; }
    u32 h = event ? handle_new(event) : 0;
    cmd[0] = ipc_header(id, 1, 2); cmd[1] = h ? 0 : RES_OUT_OF_HANDLES; cmd[2] = 0; cmd[3] = h;
    return;
  }
  case 0x8: gain = (u8)cmd[1]; reply(cmd, id, 1, 0); return;
  case 0x9: reply(cmd, id, 2, gain); return;
  case 0xA: power = (u8)cmd[1]; reply(cmd, id, 1, 0); return;
  case 0xB: reply(cmd, id, 2, power); return;
  case 0xD: clamp = (u8)cmd[1]; reply(cmd, id, 1, 0); return;
  case 0xE: reply(cmd, id, 2, clamp); return;
  case 0x1: case 0x2: case 0x4: case 0xC: case 0xF: case 0x10:
    reply(cmd, id, 1, 0); return;
  default:
    ipc_unknown("mic:u", cmd); return;
  }
}
const Service mic_service = {"mic:u", mic_request, NULL};

static Object *ir_events[3];

void iru_reset(void) {
  ir_events[0] = ir_events[1] = ir_events[2] = NULL;
}

static void iru_request(Thread *t, Object *session, u32 *cmd) {
  (void)t; (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0xA: case 0xB: case 0xC: {
    Object **e = &ir_events[id - 0xA];
    if (!*e) { *e = obj_new(OBJ_EVENT); if (*e) (*e)->refs = 1, (*e)->who = "ir:USER"; }
    u32 h = *e ? handle_new(*e) : 0;
    cmd[0] = ipc_header(id, 1, 2); cmd[1] = h ? 0 : RES_OUT_OF_HANDLES; cmd[2] = 0; cmd[3] = h;
    return;
  }
  case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: case 0x9:
  case 0xD: case 0xE: case 0xF: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
  case 0x18: case 0x19:
    reply(cmd, id, 1, 0); return;
  default:
    ipc_unknown("ir:USER", cmd); return;
  }
}
const Service iru_service = {"ir:USER", iru_request, NULL};
