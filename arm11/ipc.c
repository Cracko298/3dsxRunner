#include "sup.h"

static u32 *cmd_buffer(Thread *t) { return (u32 *)(uintptr_t)(t->ctx.tls + 0x80u); }

u32 session_new(const Service *svc, Object **out) {
  Object *o = obj_new(OBJ_SESSION);
  if (!o)
    return 0;
  o->u.se.svc = svc;
  u32 h = handle_new(o);
  if (!h) {
    o->used = 0;
    return 0;
  }
  if (out)
    *out = o;
  return h;
}

u32 *static_buffer(Thread *t, u32 index, u32 *size) {
  u32 *sb = (u32 *)(uintptr_t)(t->ctx.tls + 0x180u + index * 8u);
  u32 desc = sb[0], addr = sb[1];
  if ((desc & 0xFu) != 2u || !mmu_user_ok(addr, desc >> 14)) {
    *size = 0;
    return NULL;
  }
  *size = desc >> 14;
  return (u32 *)(uintptr_t)addr;
}

static u32 unknown_logged[32];

void ipc_unknown(const char *svc, u32 *cmd) {
  u32 id = cmd[0] >> 16, key = id ^ ((u32)(uintptr_t)svc << 8);
  bool seen = false;
  for (int i = 0; i < 32 && unknown_logged[i]; i++)
    if (unknown_logged[i] == key)
      seen = true;
  if (!seen) {
    log_printf("%s 0x%X: not handled (ok, no outputs)\n", svc, id);
    for (int i = 0; i < 32; i++)
      if (!unknown_logged[i]) {
        unknown_logged[i] = key;
        break;
      }
  }
  for (int i = 2; i < 16; i++)
    cmd[i] = 0;
  cmd[0] = ipc_header(id, 8, 0);
  cmd[1] = 0;
}

void ipc_session_request(Thread *t, Object *session) {
  u32 *cmd = cmd_buffer(t);
  const Service *svc = session->u.se.svc;
  if (!svc || !svc->request) {
    cmd[0] = ipc_header(cmd[0] >> 16, 1, 0);
    cmd[1] = RES_UNIMPL;
    return;
  }
  svc->request(t, session, cmd);
}

static const Service *const known[] = {
    &apt_service, &gsp_service, &hid_service, &fs_service, &cfg_service, &ptm_service,
    &irrst_service, &ac_service, &dsp_service, &http_service,
};

static bool name_is(const char *a, const char *b) {
  for (int i = 0; i < 8; i++) {
    if (a[i] != b[i])
      return false;
    if (!a[i])
      return true;
  }
  return true;
}

const Service *service_find(const char *name) {
  static const char *const aliases[][2] = {
      {"APT:S", "APT:U"}, {"APT:A", "APT:U"}, {"hid:SPVR", "hid:USER"}, {"cfg:s", "cfg:u"},
      {"cfg:i", "cfg:u"}, {"ptm:sysm", "ptm:u"}, {"ptm:gets", "ptm:u"}, {"ptm:sets", "ptm:u"},
      {"ptm:s", "ptm:u"}, {"ptm:play", "ptm:u"}, {"ac:i", "ac:u"}, {"gsp::Lcd", "gsp::Lcd"},
  };
  for (unsigned i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
    if (name_is(name, aliases[i][0])) {
      name = aliases[i][1];
      break;
    }
  for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
    if (name_is(name, known[i]->name))
      return known[i];
  for (const Service *s = stub_services; s->name; s++)
    if (name_is(name, s->name))
      return s;
  return NULL;
}

static Object *notification_sem;

static void srv_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x1:
    cmd[0] = ipc_header(0x1, 1, 0);
    cmd[1] = 0;
    return;
  case 0x2: {
    if (!notification_sem) {
      notification_sem = obj_new(OBJ_SEMAPHORE);
      if (notification_sem) {
        notification_sem->u.sem.max = 64;
        notification_sem->refs = 1;
      }
    }
    u32 h = handle_new(notification_sem);
    cmd[0] = ipc_header(0x2, 1, 2);
    cmd[1] = h ? 0 : RES_OUT_OF_HANDLES;
    cmd[2] = 0;
    cmd[3] = h;
    return;
  }
  case 0x5: {
    char name[9] = {0};
    u32 len = cmd[3] > 8u ? 8u : cmd[3];
    memcpy(name, &cmd[1], len);
    const Service *s = service_find(name);
    if (!s) {
      log_printf("srv: no service \"%s\"\n", name);
      cmd[0] = ipc_header(0x5, 1, 0);
      cmd[1] = RES_NOT_FOUND;
      return;
    }
    u32 h = session_new(s, NULL);
    cmd[0] = ipc_header(0x5, 1, 2);
    cmd[1] = h ? 0 : RES_OUT_OF_HANDLES;
    cmd[2] = 0x10u;
    cmd[3] = h;
    return;
  }
  case 0x9:
  case 0xA:
  case 0xC:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  case 0xB:
    cmd[0] = ipc_header(0xB, 2, 0);
    cmd[1] = 0;
    cmd[2] = 0;
    return;
  case 0xE:
  case 0xF: {
    char name[9] = {0};
    memcpy(name, &cmd[1], cmd[3] > 8u ? 8u : cmd[3]);
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = id == 0xE && service_find(name) ? 1u : 0u;
    return;
  }
  default:
    ipc_unknown("srv:", cmd);
  }
}

const Service srv_service = {"srv:", srv_request, NULL};

void ipc_reset(void) {
  notification_sem = NULL;
  memset(unknown_logged, 0, sizeof unknown_logged);
}
