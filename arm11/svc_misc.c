#include "sup.h"

static void ok(u32 *cmd, u32 id, u32 normal) {
  cmd[0] = ipc_header(id, normal, 0);
  cmd[1] = 0;
  for (u32 i = 2; i <= normal; i++)
    cmd[i] = 0;
}

static Object *apt_lock, *font_block;
static bool font_tried;

/* The system font is built from Aurora's font through temporary private
 * aliases, which get unmapped afterwards. r6 mapped whole megabytes at their
 * real addresses instead, and cleaning those up also unmapped part of the
 * display pool. YAMKC then crashed on the very next frame copy. I still
 * think about that one at night. */
static Object *shared_font(void) {
  if (font_block || font_tried)
    return font_block;
  font_tried = true;
  if (!font_pa) {
    log_printf("APT: no font (the ARM9 gave none)\n");
    return NULL;
  }

  u32 source[3];
  const u32 pas[3] = {P->font_head_pa, P->font_glyphs_pa, P->font_atlas_pa};
  for (u32 i=0;i<3;i++) {
    u32 base=pas[i]&~0xFFFFFu, alias=VA_FONT_SOURCE+i*0x200000u;
    if (base<0x20000000u || base>0x27E00000u ||
        !mmu_map(alias,base,0x200000u,MAP_PRIV|MAP_RO)) {
      mmu_unmap(VA_FONT_SOURCE,0x600000u);
      log_printf("APT: source font could not be mapped\n");
      return NULL;
    }
    source[i]=alias+(pas[i]&0xFFFFFu);
  }
  if (!mmu_map(font_va, font_pa, FONT_RESERVE, MAP_USER | MAP_RO)) {
    mmu_unmap(VA_FONT_SOURCE,0x600000u);
    log_printf("APT: the font could not be mapped\n");
    return NULL;
  }
  u32 size = font_build((u8 *)(uintptr_t)font_va, font_va, FONT_RESERVE,source);
  mmu_unmap(VA_FONT_SOURCE,0x600000u);
  cache_clean_all();
  if (!size) {
    log_printf("APT: Aurora's font could not be translated\n");
    return NULL;
  }
  font_block = obj_new(OBJ_MEMBLOCK);
  if (font_block) {
    font_block->u.mb.pa = font_pa;
    font_block->u.mb.size = (size + 0xFFFu) & ~0xFFFu;
    font_block->u.mb.va = font_va;
    font_block->refs = 1;
  }
  return font_block;
}

static void apt_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x1: {
    if (!apt_lock && (apt_lock = obj_new(OBJ_MUTEX)))
      apt_lock->refs = 1;
    ok(cmd, id, 3);
    cmd[0] = ipc_header(id, 3, 2);
    cmd[4] = 0;
    cmd[5] = handle_new(apt_lock);
    return;
  }
  case 0x2: {
    Object *a = obj_new(OBJ_EVENT), *b = obj_new(OBJ_EVENT);
    ok(cmd, id, 1);
    cmd[0] = ipc_header(id, 1, 3);
    cmd[2] = 1u << 26;
    cmd[3] = handle_new(a);
    cmd[4] = handle_new(b);
    return;
  }
  case 0x58:
    ok(cmd, id, 3);
    cmd[2] = 0x0FF3FF00u;
    cmd[3] = 0x00040000u;
    return;
  case 0x102:
    ok(cmd, id, 2);
    cmd[2] = P->new3ds ? 1u : 0u;
    return;
  case 0x50:
    ok(cmd, id, 2);
    cmd[2] = 80;
    return;
  case 0x44: {
    Object *f = shared_font();
    u32 h = f ? handle_new(f) : 0;
    if (!h) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = 0xC8A0CFF0u;
      return;
    }
    cmd[0] = ipc_header(id, 2, 2);
    cmd[1] = 0;
    cmd[2] = font_va;
    cmd[3] = 0;
    cmd[4] = h;
    return;
  }
  case 0x0D:
  case 0x0E:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0xC8A0CFF0u;
    return;
  case 0x2C:
  case 0x4E:
  case 0x32:
  case 0x24:
    log_printf("APT 0x%X: leaving\n", id);
    guest_exit(0);
  default:
    ok(cmd, id, 8);
    {
      static u32 seen[16];
      for (int i = 0; i < 16; i++) {
        if (seen[i] == id + 1u)
          break;
        if (!seen[i]) {
          seen[i] = id + 1u;
          log_printf("APT 0x%X: ok\n", id);
          break;
        }
      }
    }
  }
}

const Service apt_service = {"APT:U", apt_request, NULL};

static void cfg_block(u32 blk, u8 *out, u32 size) {
  if (!size) return;
  memset(out, 0, size);
  switch (blk) {
  case 0x000A0002:
    out[0] = 1;
    break;
  case 0x000B0000:
    if (size >= 4)
      out[2] = 2, out[3] = 49;
    break;
  case 0x000A0000: {
    const char *name = "Aurora";
    for (u32 i = 0; name[i] && i * 2 + 1 < size && i < 10; i++)
      out[i * 2] = (u8)name[i];
    break;
  }
  case 0x000A0001:
    if (size >= 2)
      out[0] = 1, out[1] = 1;
    break;
  case 0x00070001:
    out[0] = 1;
    break;
  case 0x000F0004:
    out[0] = P->new3ds ? 2 : 0;
    break;
  default: {
    static u32 seen[16];
    for (int i = 0; i < 16; i++) {
      if (seen[i] == blk)
        break;
      if (!seen[i]) {
        seen[i] = blk;
        log_printf("cfg block 0x%08X: zeros\n", blk);
        break;
      }
    }
  }
  }
}

static void cfg_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x001:
  case 0x401:
  case 0x801: {
    u32 size = cmd[1], blk = cmd[2], va = cmd[4];
    bool valid = size && size <= 0x10000 && mmu_user_ok(va, size);
    if (valid) cfg_block(blk, (u8 *)(uintptr_t)va, size);
    cmd[0] = ipc_header(id, 1, 2);
    cmd[1] = valid ? 0 : RES_INVALID_ADDR;
    cmd[2] = (size << 4) | 0xCu;
    cmd[3] = va;
    return;
  }
  case 0x002:
    ok(cmd, id, 2);
    cmd[2] = 1;
    return;
  case 0x003: {
    u32 salt = cmd[1];
    ok(cmd, id, 3);
    cmd[2] = 0x1234ABCDu ^ salt;
    cmd[3] = 0x55AA0F0Fu;
    return;
  }
  case 0x004:
    ok(cmd, id, 2);
    cmd[2] = 1;
    return;
  case 0x005:
    ok(cmd, id, 2);
    cmd[2] = P->new3ds ? 2u : 0u;
    return;
  case 0x006:
    ok(cmd, id, 2);
    cmd[2] = 1;
    return;
  case 0x009:
    ok(cmd, id, 2);
    cmd[2] = 'U' | 'S' << 8;
    return;
  case 0x00A:
    ok(cmd, id, 2);
    cmd[2] = 49;
    return;
  case 0x00B:
    ok(cmd, id, 2);
    cmd[2] = 0;
    return;
  default:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = RES_UNIMPL;
  }
}

const Service cfg_service = {"cfg:u", cfg_request, NULL};

static void ptm_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  (void)session;
  u32 id = cmd[0] >> 16;
  switch (id) {
  case 0x5:
  case 0x8:
  case 0x9:
    ok(cmd, id, 2);
    return;
  case 0x6:
    ok(cmd, id, 2);
    cmd[2] = 1;
    return;
  case 0x7:
    ok(cmd, id, 2);
    cmd[2] = 5;
    return;
  case 0x040A:
    ok(cmd, id, 2);
    cmd[2] = P->new3ds ? 1u : 0u;
    return;
  case 0x0407:
  case 0x0409:
    log_printf("ptm 0x%X: leaving\n", id);
    guest_exit(0);
  default:
    ok(cmd, id, 8);
  }
}

const Service ptm_service = {"ptm:u", ptm_request, NULL};

static u32 ac_pending_id, ac_last_error;
static Object *ac_event;
static void ac_done(Thread *t) {
  u32 *cmd=(u32 *)(uintptr_t)(t->ctx.tls+0x80u);
  JobHttp *h=http_network_result();
  if (ac_pending_id==4) {
    ac_last_error=(u32)h->result;
    event_signal(ac_event); obj_unref(ac_event); ac_event=NULL;
    ok(cmd,4,1);
  } else {
    ok(cmd,ac_pending_id,2);
    cmd[2]=ac_pending_id==0xC ? (h->status?3u:1u) : (h->status?2u:0u);
  }
}
static void ac_request(Thread *t, Object *session, u32 *cmd) {
  (void)session;
  u32 id=cmd[0]>>16;
  switch (id) {
  case 1: {
    u32 size; u32 *buf=static_buffer(t,0,&size);
    if (!buf || size<0x200 || !mmu_user_write_ok((u32)(uintptr_t)buf,0x200)) {
      cmd[0]=ipc_header(id,1,0); cmd[1]=RES_INVALID_ADDR; return;
    }
    memset(buf,0,0x200); ok(cmd,id,1); return;
  }
  case 4:
    ac_event=handle_obj(cmd[4]);
    if (!ac_event || ac_event->type!=OBJ_EVENT) {
      ac_event=NULL; cmd[0]=ipc_header(id,1,0); cmd[1]=RES_INVALID_HANDLE; return;
    }
    obj_ref(ac_event); ac_pending_id=id;
    http_network_request(t,HTTPOP_NET_CONNECT,ac_done); return;
  case 0xA: case 0xB:
    ok(cmd,id,2); cmd[2]=ac_last_error; return;
  case 0xC: case 0xD:
    ac_pending_id=id; http_network_request(t,HTTPOP_NET_STATUS,ac_done); return;
  case 0x22: case 0x25: case 0x2D:
    ok(cmd,id,1); return;
  case 0x36:
    ok(cmd,id,2); return;
  default:
    cmd[0]=ipc_header(id,1,0); cmd[1]=RES_UNIMPL; return;
  }
}

const Service ac_service = {"ac:u", ac_request, NULL};

static void stub_request(Thread *t, Object *session, u32 *cmd) {
  (void)t;
  ipc_unknown(session->u.se.svc->name, cmd);
}

const Service stub_services[] = {
    {"ndm:u", stub_request, NULL},    {"gsp::Lcd", stub_request, NULL},
    {"am:u", stub_request, NULL},     {"am:net", stub_request, NULL},
    {"am:sys", stub_request, NULL},   {"am:app", stub_request, NULL},
    {"act:u", stub_request, NULL},    {"frd:u", stub_request, NULL},
    {"news:u", stub_request, NULL},   {"boss:U", stub_request, NULL},
    {"mcu::HWC", stub_request, NULL}, {"ps:ps", stub_request, NULL},
    {"nim:aoc", stub_request, NULL},  {"cecd:u", stub_request, NULL},
    {NULL, NULL, NULL},
};

void misc_reset(void) {
  apt_lock = font_block = NULL;
  font_tried = false;
}
