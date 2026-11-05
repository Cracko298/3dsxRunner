#include "sup.h"

#define SLOT  0
#define FLAGS 1

static struct {
  u32 id;
  Object *session;
  u32 va, size, done;
  u64 offset;
  bool cached;
  bool bounce;
  u32 count;
} pend;

#define RA_SIZE 0x10000u
static u8 ra_buf[RA_SIZE] __attribute__((aligned(64)));
static struct {
  u32 slot;
  u64 offset;
  u32 bytes;
} ra;

#define BOUNCE_SIZE 0x10000u
static u8 bounce_buf[BOUNCE_SIZE] __attribute__((aligned(64)));

#define DIRENTS 16
static JobDirent dirents[DIRENTS] __attribute__((aligned(64)));

static u32 archives_open;

static void put_utf8(char *out, u32 *n, u32 cap, u32 c) {
  char b[4];
  int k;
  if (c < 0x80)
    b[0] = (char)c, k = 1;
  else if (c < 0x800)
    b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), k = 2;
  else if (c < 0x10000)
    b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 63)),
    b[2] = (char)(0x80 | (c & 63)), k = 3;
  else
    b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 63)),
    b[2] = (char)(0x80 | ((c >> 6) & 63)), b[3] = (char)(0x80 | (c & 63)), k = 4;
  if (*n + (u32)k < cap)
    for (int i = 0; i < k; i++)
      out[(*n)++] = b[i];
}

static bool get_path(u32 type, u32 size, u32 va, char *out, u32 cap) {
  u32 n = 0;
  out[0] = 0;
  if (type == 1) {
    out[0] = '/', out[1] = 0;
    return true;
  }
  if ((type != 3 && type != 4) || !mmu_user_ok(va, size))
    return false;
  out[n++] = '/';
  if (type == 3) {
    const char *s = (const char *)(uintptr_t)va;
    for (u32 i = 0; i < size && s[i]; i++)
      if (!(n == 1 && s[i] == '/') && n + 1 < cap)
        out[n++] = s[i];
  } else {
    const u16 *s = (const u16 *)(uintptr_t)va;
    for (u32 i = 0; i < size / 2 && s[i]; i++) {
      u32 c = s[i];
      if (c >= 0xD800 && c < 0xDC00 && i + 1 < size / 2 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
        c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
      if (n == 1 && c == '/')
        continue;
      put_utf8(out, &n, cap, c);
    }
  }
  while (n > 1 && out[n - 1] == '/')
    n--;
  out[n] = 0;
  return true;
}

static bool sd_archive(u32 lo, u32 hi) {
  (void)hi;
  return lo == 9u || lo == 0xAu;
}

static void issue(Thread *t, u32 op, void (*done)(Thread *t)) {
  P->fs.op = op;
  arm9_request(t, done);
}

static u32 *cmd_of(Thread *t) { return (u32 *)(uintptr_t)(t->ctx.tls + 0x80u); }

static void ra_drop(void) { ra.slot = 0; }

static u32 run_at(u32 va, u32 size, u32 *pa) {
  *pa = mmu_pa(va);
  if (!*pa || !mmu_user_ok(va, 1))
    return 0;
  u32 len = 0x1000u - (va & 0xFFFu);
  while (len < size && mmu_user_ok(va + len, 1) && mmu_pa(va + len) == *pa + len)
    len += 0x1000u;
  return len < size ? len : size;
}

static void done_simple(Thread *t) {
  u32 *cmd = cmd_of(t);
  cmd[0] = ipc_header(pend.id, 1, 0);
  cmd[1] = (u32)P->fs.result;
}

static void done_open(Thread *t) {
  u32 *cmd = cmd_of(t);
  u32 id = pend.id;
  if (P->fs.result) {
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = (u32)P->fs.result;
    return;
  }
  Object *se = NULL;
  u32 h = session_new(id == 0x80B ? &fs_dir_service : &fs_file_service, &se);
  if (!h) {
    if (id == 0x80B)
      P->close_dirs |= 1u << (P->fs.value & 31u);
    else
      P->close_files |= 1u << (P->fs.value & 31u);
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = RES_OUT_OF_HANDLES;
    return;
  }
  se->u.se.data[SLOT] = P->fs.value;
  se->u.se.data[FLAGS] = P->fs.flags;
  cmd[0] = ipc_header(id, 1, 2);
  cmd[1] = 0;
  cmd[2] = 0x10u;
  cmd[3] = h;
}

static void done_free(Thread *t) {
  u32 *cmd = cmd_of(t);
  u64 bytes = P->fs.result ? 0 : P->fs.value64;
  u32 cluster = P->fs.value ? P->fs.value : 0x8000u;
  if (pend.id == 0x812) {
    cmd[0] = ipc_header(0x812, 3, 0);
    cmd[1] = (u32)P->fs.result;
    cmd[2] = (u32)bytes;
    cmd[3] = (u32)(bytes >> 32);
    return;
  }
  cmd[0] = ipc_header(pend.id, 5, 0);
  cmd[1] = (u32)P->fs.result;
  cmd[2] = 512;
  cmd[3] = cluster;
  cmd[4] = (u32)(P->fs.offset / cluster);
  cmd[5] = (u32)(bytes / cluster);
}

static void fs_request(Thread *t, Object *session, u32 *cmd) {
  (void)session;
  u32 id = cmd[0] >> 16;
  JobFs *f = &P->fs;
  pend.id = id;
  pend.session = NULL;
  switch (id) {
  case 0x801:
  case 0x861:
  case 0x862:
  case 0x80D:
  case 0x401:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  case 0x863:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 0;
    return;
  case 0x80C:
    if (cmd[1] != 9u && cmd[1] != 0xAu) {
      log_printf("fs: archive 0x%X is not here\n", cmd[1]);
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    archives_open++;
    cmd[0] = ipc_header(id, 3, 0);
    cmd[2] = cmd[1];
    cmd[3] = archives_open;
    cmd[1] = 0;
    return;
  case 0x80E:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  case 0x817:
  case 0x818:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 1;
    return;
  case 0x868:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 1;
    return;
  case 0x802:
  case 0x803: {
    bool direct = id == 0x803;
    u32 arch = direct ? cmd[2] : cmd[2], arch_hi = direct ? 0 : cmd[3];
    u32 ptype = direct ? cmd[5] : cmd[4], psize = direct ? cmd[6] : cmd[5];
    u32 flags = direct ? cmd[7] : cmd[6];
    u32 pva = direct ? cmd[12] : cmd[9];
    if (!sd_archive(arch, arch_hi)) {
      if (direct)
        log_printf("fs: archive 0x%X is not here\n", arch);
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    if (!get_path(ptype, psize, pva, f->path, sizeof f->path)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = 0xE0E046BEu;
      return;
    }
    f->flags = flags;
    if (flags & 2u)
      ra_drop();
    issue(t, FSOP_OPEN, done_open);
    return;
  }
  case 0x804:
  case 0x806:
  case 0x807:
  case 0x809: {
    u32 pva = id == 0x809 ? cmd[8] : cmd[7];
    if (!sd_archive(cmd[2], cmd[3]) || !get_path(cmd[4], cmd[5], pva, f->path, sizeof f->path)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    ra_drop();
    f->flags = id == 0x807 ? 1u : 0u;
    issue(t, id == 0x804 ? FSOP_DELETE : id == 0x809 ? FSOP_MKDIR : FSOP_RMDIR, done_simple);
    return;
  }
  case 0x808:
    if (!sd_archive(cmd[2], cmd[3]) || !get_path(cmd[4], cmd[5], cmd[10], f->path, sizeof f->path)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    ra_drop();
    f->value64 = cmd[7] | (u64)cmd[8] << 32;
    issue(t, FSOP_CREATE, done_simple);
    return;
  case 0x805:
  case 0x80A: {
    if (!sd_archive(cmd[2], cmd[3]) || !sd_archive(cmd[6], cmd[7]) ||
        !get_path(cmd[4], cmd[5], cmd[11], f->path, sizeof f->path) ||
        !get_path(cmd[8], cmd[9], cmd[13], f->path2, sizeof f->path2)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    ra_drop();
    issue(t, FSOP_RENAME, done_simple);
    return;
  }
  case 0x80B:
    if (!sd_archive(cmd[1], cmd[2]) || !get_path(cmd[3], cmd[4], cmd[6], f->path, sizeof f->path)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    issue(t, FSOP_OPENDIR, done_open);
    return;
  case 0x812:
  case 0x814:
  case 0x849:
    issue(t, FSOP_FREE, done_free);
    return;
  default:
    ipc_unknown("fs:USER", cmd);
  }
}

static void reply_rw(Thread *t, s32 res) {
  u32 *cmd = cmd_of(t);
  bool write = pend.id == 0x803;
  cmd[0] = ipc_header(pend.id, 2, 2);
  cmd[1] = (u32)res;
  cmd[2] = pend.done;
  cmd[3] = (pend.size << 4) | (write ? 0xAu : 0xCu);
  cmd[4] = pend.va;
}

static void rw_next(Thread *t);

static void done_rw(Thread *t) {
  JobFs *f = &P->fs;
  if (f->result) {
    reply_rw(t, f->result);
    return;
  }
  if (pend.cached) {
    ra.slot = pend.session->u.se.data[SLOT];
    ra.offset = f->offset;
    ra.bytes = f->value;
    u64 rel = pend.offset - ra.offset;
    u32 n = rel < ra.bytes ? ra.bytes - (u32)rel : 0;
    if (n > pend.size)
      n = pend.size;
    memcpy((void *)(uintptr_t)pend.va, ra_buf + rel, n);
    pend.done = n;
    reply_rw(t, 0);
    return;
  }
  u32 asked = f->size;
  if (pend.bounce && pend.id == 0x802)
    memcpy((void *)(uintptr_t)(pend.va + pend.done), bounce_buf, f->value < asked ? f->value : asked);
  pend.done += f->value;
  if (f->value < asked || pend.done >= pend.size) {
    reply_rw(t, 0);
    return;
  }
  rw_next(t);
}

static void rw_next(Thread *t) {
  JobFs *f = &P->fs;
  u32 pa, len = run_at(pend.va + pend.done, pend.size - pend.done, &pa);
  if (!len) {
    reply_rw(t, (s32)RES_INVALID_ADDR);
    return;
  }
  pend.bounce = pa >= 0x28000000u;
  if (pend.bounce) {
    if (len > BOUNCE_SIZE)
      len = BOUNCE_SIZE;
    if (pend.id == 0x803)
      memcpy(bounce_buf, (const void *)(uintptr_t)(pend.va + pend.done), len);
    pa = sup_pa(bounce_buf);
  }
  f->file = pend.session->u.se.data[SLOT];
  f->offset = pend.offset + pend.done;
  f->size = len;
  f->buf = pa;
  pend.cached = false;
  issue(t, pend.id == 0x803 ? FSOP_WRITE : FSOP_READ, done_rw);
}

static void done_size(Thread *t) {
  u32 *cmd = cmd_of(t);
  cmd[0] = ipc_header(0x804, 3, 0);
  cmd[1] = (u32)P->fs.result;
  cmd[2] = (u32)P->fs.value64;
  cmd[3] = (u32)(P->fs.value64 >> 32);
}

static void file_request(Thread *t, Object *se, u32 *cmd) {
  u32 id = cmd[0] >> 16;
  JobFs *f = &P->fs;
  u32 slot = se->u.se.data[SLOT];
  pend.id = id;
  pend.session = se;
  switch (id) {
  case 0x802: {
    pend.offset = cmd[1] | (u64)cmd[2] << 32;
    pend.size = cmd[3];
    pend.va = cmd[5];
    pend.done = 0;
    if (!pend.size) {
      reply_rw(t, 0);
      return;
    }
    if (!mmu_user_ok(pend.va, pend.size)) {
      reply_rw(t, (s32)RES_INVALID_ADDR);
      return;
    }
    bool readonly = !(se->u.se.data[FLAGS] & 2u);
    if (readonly && ra.slot == slot && pend.offset >= ra.offset &&
        pend.offset + pend.size <= ra.offset + ra.bytes) {
      memcpy((void *)(uintptr_t)pend.va, ra_buf + (pend.offset - ra.offset), pend.size);
      pend.done = pend.size;
      reply_rw(t, 0);
      return;
    }
    if (readonly && pend.size <= RA_SIZE / 4) {
      f->file = slot;
      f->offset = pend.offset & ~0x1FFull;
      f->size = RA_SIZE;
      f->buf = sup_pa(ra_buf);
      pend.cached = true;
      ra_drop();
      issue(t, FSOP_READ, done_rw);
      return;
    }
    rw_next(t);
    return;
  }
  case 0x803:
    pend.offset = cmd[1] | (u64)cmd[2] << 32;
    pend.size = cmd[3];
    pend.va = cmd[6];
    pend.done = 0;
    ra_drop();
    if (!pend.size) {
      reply_rw(t, 0);
      return;
    }
    f->flags = cmd[4];
    rw_next(t);
    return;
  case 0x804:
    f->file = slot;
    issue(t, FSOP_SIZE, done_size);
    return;
  case 0x805:
    ra_drop();
    f->file = slot;
    f->value64 = cmd[1] | (u64)cmd[2] << 32;
    issue(t, FSOP_SETSIZE, done_simple);
    return;
  case 0x808:
    if (ra.slot == slot)
      ra_drop();
    f->file = slot;
    se->u.se.data[SLOT] = 0;
    if (!slot) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = 0;
      return;
    }
    issue(t, FSOP_CLOSE, done_simple);
    return;
  case 0x806:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 0;
    return;
  case 0x807:
  case 0x809:
  case 0x80A:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  case 0x80B:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 0;
    return;
  default:
    ipc_unknown("fs file", cmd);
  }
}

static void file_closed(Object *se) {
  u32 slot = se->u.se.data[SLOT];
  if (slot) {
    if (ra.slot == slot)
      ra_drop();
    P->close_files |= 1u << (slot & 31u);
  }
}

static void dir_next(Thread *t);

static void done_dir(Thread *t) {
  u32 *cmd = cmd_of(t);
  JobFs *f = &P->fs;
  u32 n = f->result ? 0 : f->value;
  if (n > DIRENTS)
    n = DIRENTS;
  u8 *out = (u8 *)(uintptr_t)(pend.va + pend.done * 0x228u);
  for (u32 i = 0; i < n; i++, out += 0x228) {
    memset(out, 0, 0x228);
    const u8 *s = (const u8 *)dirents[i].name;
    u16 *name = (u16 *)(void *)out;
    u32 k = 0;
    while (*s && k < 0x105) {
      u32 c = *s++;
      if (c >= 0xF0 && s[0] && s[1] && s[2])
        c = (c & 7u) << 18 | (s[0] & 63u) << 12 | (s[1] & 63u) << 6 | (s[2] & 63u), s += 3;
      else if (c >= 0xE0 && s[0] && s[1])
        c = (c & 15u) << 12 | (s[0] & 63u) << 6 | (s[1] & 63u), s += 2;
      else if (c >= 0xC0 && s[0])
        c = (c & 31u) << 6 | (s[0] & 63u), s += 1;
      if (c >= 0x10000) {
        if (k + 2 > 0x105)
          break;
        c -= 0x10000;
        name[k++] = (u16)(0xD800 + (c >> 10));
        name[k++] = (u16)(0xDC00 + (c & 0x3FF));
      } else {
        name[k++] = (u16)c;
      }
    }
    char *sn = (char *)out + 0x20C, *se = (char *)out + 0x216;
    const char *nm = dirents[i].name, *dot = NULL;
    for (const char *p = nm; *p; p++)
      if (*p == '.')
        dot = p;
    for (int j = 0, k2 = 0; nm[j] && (!dot || nm + j < dot) && k2 < 8; j++)
      sn[k2++] = (nm[j] >= 'a' && nm[j] <= 'z') ? (char)(nm[j] - 32) : nm[j];
    if (dot)
      for (int j = 1, k2 = 0; dot[j] && k2 < 3; j++)
        se[k2++] = (dot[j] >= 'a' && dot[j] <= 'z') ? (char)(dot[j] - 32) : dot[j];
    out[0x21A] = 1;
    *(u32 *)(void *)(out + 0x21C) = dirents[i].is_dir ? 1u : 0x10000u;
    *(u64 *)(void *)(out + 0x220) = dirents[i].size;
  }
  pend.done += n;
  if (!f->result && n == DIRENTS && pend.done < pend.count) {
    dir_next(t);
    return;
  }
  cmd[0] = ipc_header(0x801, 2, 2);
  cmd[1] = (u32)f->result;
  cmd[2] = pend.done;
  cmd[3] = (pend.count * 0x228u) << 4 | 0xCu;
  cmd[4] = pend.va;
}

static void dir_next(Thread *t) {
  JobFs *f = &P->fs;
  u32 want = pend.count - pend.done;
  f->file = pend.session->u.se.data[SLOT];
  f->size = want < DIRENTS ? want : DIRENTS;
  f->buf = sup_pa(dirents);
  issue(t, FSOP_READDIR, done_dir);
}

static void dir_request(Thread *t, Object *se, u32 *cmd) {
  u32 id = cmd[0] >> 16;
  pend.id = id;
  pend.session = se;
  switch (id) {
  case 0x801:
    pend.count = cmd[1];
    pend.va = cmd[3];
    pend.done = 0;
    if (!pend.count || !mmu_user_ok(pend.va, pend.count * 0x228u)) {
      cmd[0] = ipc_header(id, 2, 0);
      cmd[1] = pend.count ? RES_INVALID_ADDR : 0;
      cmd[2] = 0;
      return;
    }
    dir_next(t);
    return;
  case 0x802: {
    u32 slot = se->u.se.data[SLOT];
    se->u.se.data[SLOT] = 0;
    if (!slot) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = 0;
      return;
    }
    P->fs.file = slot;
    issue(t, FSOP_CLOSEDIR, done_simple);
    return;
  }
  case 0x803:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  default:
    ipc_unknown("fs dir", cmd);
  }
}

static void dir_closed(Object *se) {
  u32 slot = se->u.se.data[SLOT];
  if (slot)
    P->close_dirs |= 1u << (slot & 31u);
}

const Service fs_service = {"fs:USER", fs_request, NULL};
const Service fs_file_service = {"fs file", file_request, file_closed};
const Service fs_dir_service = {"fs dir", dir_request, dir_closed};

void fs_reset(void) {
  memset(&pend, 0, sizeof pend);
  ra_drop();
  archives_open = 0;
}
