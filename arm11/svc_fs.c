#include "sup.h"

#define SLOT  0
#define FLAGS 1
#define SELF  2
#define BASE  3
static bool self_path(const char *path);

static struct {
  u32 id;
  Object *session;
  u32 va, size, done;
  u64 offset;
  bool cached;
  bool bounce;
  u32 count;
  u32 base;
  u8 sysdata;
  u32 slot;
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

#define ARCH_SELFNCCH 3u
#define ARCH_SAVEDATA 4u
#define ARCH_EXTDATA  6u
#define ARCH_SHAREDEXT 7u
#define ARCH_NCCH 0x2345678Au

static struct { u32 lo, hi; } ncch_titles[8];
static u32 binary_word(u32 type, u32 size, u32 va, u32 i) {
  if (type != 2 || size < (i + 1) * 4 || !mmu_user_ok(va + i * 4, 4)) return 0;
  return *(const u32 *)(uintptr_t)(va + i * 4);
}

static bool retail_title(void) { return (P->launch_flags & 4u) != 0; }

static bool sd_archive(u32 lo, u32 hi) {
  (void)hi;
  if (lo == 9u || lo == 0xAu) return true;
  return retail_title() && (lo == ARCH_SAVEDATA || lo == ARCH_EXTDATA || lo == ARCH_SHAREDEXT);
}

static bool apath(u32 lo, u32 hi, u32 type, u32 size, u32 va, char *out, u32 cap) {
  if (lo == 9u || lo == 0xAu) return get_path(type, size, va, out, cap);
  if (!sd_archive(lo, hi)) return false;
  const char *leaf = lo == ARCH_SAVEDATA ? "/SaveData" : "/ExtData";
  char sub[256];
  if (!get_path(type, size, va, sub, sizeof sub)) return false;
  snprintf(out, cap, "%s%s%s", P->save_dir, leaf, (sub[0] == '/' && !sub[1]) ? "" : sub);
  return true;
}

static struct { char path[72]; u32 reads, writes; u64 rbytes, wbytes; } fstat[32];

static void trace_close(u32 slot) {
  slot &= 31u;
  if (!fstat[slot].path[0]) return;
  trace_printf("fs: closed %s: %u reads (%llu bytes), %u writes (%llu bytes)\n", fstat[slot].path,
               fstat[slot].reads, fstat[slot].rbytes, fstat[slot].writes, fstat[slot].wbytes);
  fstat[slot].path[0] = 0;
}

static void issue(Thread *t, u32 op, void (*done)(Thread *t)) {
  static const char *const names[] = {"", "open", "read", "write", "size", "set size", "close", "delete",
                                      "rename", "make folder", "remove folder", "create", "open folder",
                                      "read folder", "close folder", "check", "free space"};
  if (tracing() && (op == FSOP_DELETE || op == FSOP_RENAME || op == FSOP_MKDIR || op == FSOP_RMDIR ||
                    op == FSOP_CREATE || op == FSOP_OPENDIR)) {
    if (op == FSOP_RENAME) trace_printf("fs: rename %s -> %s\n", P->fs.path, P->fs.path2);
    else if (op == FSOP_CREATE) trace_printf("fs: create %s (%llu bytes)\n", P->fs.path, P->fs.value64);
    else trace_printf("fs: %s %s\n", names[op], P->fs.path);
  }
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

static void finish_open(Thread *t);

#define RES_FS_NOT_FORMATTED 0xC8A04554u
static void save_marker(char *out, u32 cap) { snprintf(out, cap, "%s/.formatted", P->save_dir); }

static void done_save_mount(Thread *t) {
  u32 *cmd = cmd_of(t);
  if (P->fs.result) {
    log_printf("fs: save data not formatted yet (first boot)\n");
    cmd[0] = ipc_header(0x80C, 1, 0);
    cmd[1] = RES_FS_NOT_FORMATTED;
    return;
  }
  archives_open++;
  cmd[0] = ipc_header(0x80C, 3, 0);
  cmd[2] = ARCH_SAVEDATA;
  cmd[3] = archives_open;
  cmd[1] = 0;
}

static void done_format(Thread *t) {
  u32 *cmd = cmd_of(t);
  log_printf("fs: save data formatted\n");
  cmd[0] = ipc_header(pend.id, 1, 0);
  cmd[1] = 0;
}

static void sysdata_peek(Thread *t, u64 offset, void (*then)(Thread *t)) {
  JobFs *f = &P->fs;
  f->file = pend.slot;
  f->offset = offset;
  f->size = 0x200;
  f->buf = sup_pa(bounce_buf);
  issue(t, FSOP_READ, then);
}

static u32 ivfc_level3(void) {
  if (P->fs.result || P->fs.value < 0x60 || memcmp(bounce_buf, "IVFC", 4)) return 0;
  u32 master, log2;
  memcpy(&master, bounce_buf + 0x08, 4);
  memcpy(&log2, bounce_buf + 0x4C, 4);
  u32 block = log2 >= 4 && log2 <= 20 ? 1u << log2 : 0x1000u;
  return (0x60u + master + block - 1) & ~(block - 1);
}

static void sysdata_done(Thread *t) {
  P->fs.result = 0;
  P->fs.value = pend.slot;
  finish_open(t);
}

static void done_sysdata_romfs(Thread *t) {
  u32 l3 = ivfc_level3();
  if (!l3) log_printf("fs: %s has no readable RomFS\n", P->fs.path);
  pend.base += l3;
  sysdata_done(t);
}

static void done_sysdata_peek(Thread *t) {
  u32 l3 = ivfc_level3();
  pend.base = l3;
  if (!l3 && !P->fs.result && P->fs.value >= 0x200 && !memcmp(bounce_buf + 0x100, "NCCH", 4)) {
    u32 off;
    memcpy(&off, bounce_buf + 0x1B0, 4);
    u32 units = 0x200u << bounce_buf[0x18E];
    if (!(bounce_buf[0x18F] & 4u)) {
      log_printf("fs: %s is still encrypted (decrypt it with GodMode9 first)\n", P->fs.path);
    } else if (off) {
      pend.base = off * units;
      sysdata_peek(t, pend.base, done_sysdata_romfs);
      return;
    }
  }
  sysdata_done(t);
}

static void done_open(Thread *t) {
  u32 *cmd = cmd_of(t);
  u32 id = pend.id;
  if (pend.sysdata == 1 && P->fs.path2[0]) {
    P->fs.path2[sizeof P->fs.path2 - 1] = 0;
    log_printf("fs: NAND copy of %s: %s\n", P->fs.path + 16, P->fs.path2);
  }
  if (P->fs.result && pend.sysdata == 1) {
    size_t n = strlen(P->fs.path);
    memcpy(P->fs.path + n - 6, ".app", 5);
    pend.sysdata = 2;
    P->fs.flags = 1u;
    issue(t, FSOP_OPEN, done_open);
    return;
  }
  if (P->fs.result && !pend.sysdata && retail_title()) {
    static u32 logged;
    if (logged < 8) {
      logged++;
      log_printf("fs: can't open %s (%08X)\n", P->fs.path, (u32)P->fs.result);
    }
  }
  if (P->fs.result) {
    if (pend.sysdata) {
      size_t n = strlen(P->fs.path);
      P->fs.path[n - 4] = 0;
      log_printf("fs: no %s.romfs or .app (dump that title there)\n", P->fs.path);
    }
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = (u32)P->fs.result;
    return;
  }
  if (pend.sysdata) {
    pend.sysdata = 0;
    pend.slot = P->fs.value;
    sysdata_peek(t, 0, done_sysdata_peek);
    return;
  }
  finish_open(t);
}

static void finish_open(Thread *t) {
  u32 *cmd = cmd_of(t);
  u32 id = pend.id;
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
  se->u.se.data[SELF] = !pend.base && self_path(P->fs.path);
  se->u.se.data[BASE] = pend.base;
  if (tracing()) {
    u32 s = P->fs.value & 31u;
    trace_close(s);
    snprintf(fstat[s].path, sizeof fstat[s].path, "%s", P->fs.path);
    fstat[s].reads = fstat[s].writes = 0;
    fstat[s].rbytes = fstat[s].wbytes = 0;
    trace_printf("fs: opened %s (%s)\n", P->fs.path, (P->fs.flags & 2u) ? "read/write" : "read");
  }
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
  pend.base = 0;
  pend.sysdata = 0;
  switch (id) {
  case 0x801:
  case 0x861:
  case 0x862:
  case 0x80D:
  case 0x401:
    cmd[0] = ipc_header(id, 1, 0);
    cmd[1] = 0;
    return;
  case 0x84C:
    save_marker(f->path, sizeof f->path);
    f->value64 = 0;
    issue(t, FSOP_CREATE, done_format);
    return;
  case 0x863:
    cmd[0] = ipc_header(id, 2, 0);
    cmd[1] = 0;
    cmd[2] = 0;
    return;
  case 0x80C:
    trace_printf("fs: mount archive 0x%X\n", cmd[1]);
    if (cmd[1] == ARCH_NCCH) {
      u32 lo = binary_word(cmd[2], cmd[3], cmd[5], 0), hi = binary_word(cmd[2], cmd[3], cmd[5], 1);
      log_printf("fs: title %08X%08X's content (archive 0x2345678A, media %u)\n", hi, lo,
                 binary_word(cmd[2], cmd[3], cmd[5], 2));
      archives_open++;
      ncch_titles[archives_open & 7u].lo = lo;
      ncch_titles[archives_open & 7u].hi = hi;
      cmd[0] = ipc_header(id, 3, 0);
      cmd[2] = ARCH_NCCH;
      cmd[3] = archives_open;
      cmd[1] = 0;
      return;
    }
    if (cmd[1] == ARCH_SAVEDATA && retail_title()) {
      save_marker(f->path, sizeof f->path);
      issue(t, FSOP_STAT, done_save_mount);
      return;
    }
    if (!sd_archive(cmd[1], 0) && !(cmd[1] == ARCH_SELFNCCH && P->self_romfs)) {
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
    if (arch == ARCH_NCCH) {
      u32 lo, hi;
      if (direct) {
        lo = binary_word(cmd[3], cmd[4], cmd[10], 0), hi = binary_word(cmd[3], cmd[4], cmd[10], 1);
        log_printf("fs: title %08X%08X's content (archive 0x2345678A)\n", hi, lo);
      } else {
        lo = ncch_titles[cmd[3] & 7u].lo, hi = ncch_titles[cmd[3] & 7u].hi;
      }
      snprintf(f->path, sizeof f->path, "/Aurora/sysdata/%08X%08X.romfs", hi, lo);
      f->flags = 1u;
      f->path2[0] = 0;
      pend.sysdata = 1;
      issue(t, FSOP_OPEN, done_open);
      return;
    }
    if (arch == ARCH_SELFNCCH && P->self_romfs) {
      snprintf(f->path, sizeof f->path, "%s", P->path + 5);
      f->flags = 1u;
      pend.base = P->self_romfs;
      issue(t, FSOP_OPEN, done_open);
      return;
    }
    if (!sd_archive(arch, arch_hi)) {
      if (direct)
        log_printf("fs: archive 0x%X is not here\n", arch);
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    if (!apath(arch, arch_hi, ptype, psize, pva, f->path, sizeof f->path)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = 0xE0E046BEu;
      return;
    }
    f->flags = flags;
    if (flags & 2u)
      ra_drop();
    if (retail_title()) {
      static u32 shown;
      if (shown++ < 12) log_printf("fs: open %s%s\n", f->path, (flags & 2u) ? " (write)" : "");
    }
    issue(t, FSOP_OPEN, done_open);
    return;
  }
  case 0x804:
  case 0x806:
  case 0x807:
  case 0x809: {
    u32 pva = id == 0x809 ? cmd[8] : cmd[7];
    if (!apath(cmd[2], cmd[3], cmd[4], cmd[5], pva, f->path, sizeof f->path)) {
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
    if (!apath(cmd[2], cmd[3], cmd[4], cmd[5], cmd[10], f->path, sizeof f->path)) {
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
    if (!apath(cmd[2], cmd[3], cmd[4], cmd[5], cmd[11], f->path, sizeof f->path) ||
        !apath(cmd[6], cmd[7], cmd[8], cmd[9], cmd[13], f->path2, sizeof f->path2)) {
      cmd[0] = ipc_header(id, 1, 0);
      cmd[1] = RES_FS_NOT_FOUND;
      return;
    }
    ra_drop();
    issue(t, FSOP_RENAME, done_simple);
    return;
  }
  case 0x80B:
    if (!apath(cmd[1], cmd[2], cmd[3], cmd[4], cmd[6], f->path, sizeof f->path)) {
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

static bool self_path(const char *path) {
  const char *own = P->path;
  if (!P->self_romfs || memcmp(own, "sdmc:", 5)) return false;
  own += 5;
  for (; *own && *path; own++, path++) {
    char a = *own, b = *path;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return !*own && !*path;
}

static void self_header(u64 offset, u8 *buf, u32 n) {
  u8 h[0x2C];
  memset(h, 0, sizeof h);
  memcpy(h, "3DSX", 4);
  h[4] = 0x2C;
  h[6] = 8;
  u32 fs = P->self_romfs;
  h[0x28] = (u8)fs; h[0x29] = (u8)(fs >> 8); h[0x2A] = (u8)(fs >> 16); h[0x2B] = (u8)(fs >> 24);
  for (u32 i = 0; i < n && offset + i < sizeof h; i++) buf[i] = h[offset + i];
}

static void reply_rw(Thread *t, s32 res) {
  u32 *cmd = cmd_of(t);
  bool write = pend.id == 0x803;
  if (!write && !res && pend.done && pend.offset < 0x2C && pend.session && pend.session->u.se.data[SELF])
    self_header(pend.offset, (u8 *)(uintptr_t)pend.va, pend.done);
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
  u64 size = P->fs.value64;
  u32 base = pend.session ? pend.session->u.se.data[BASE] : 0;
  size = size > base ? size - base : 0;
  cmd[0] = ipc_header(0x804, 3, 0);
  cmd[1] = (u32)P->fs.result;
  cmd[2] = (u32)size;
  cmd[3] = (u32)(size >> 32);
}

static u8 links[32];

static void file_request(Thread *t, Object *se, u32 *cmd) {
  u32 id = cmd[0] >> 16;
  JobFs *f = &P->fs;
  u32 slot = se->u.se.data[SLOT];
  pend.id = id;
  pend.session = se;
  switch (id) {
  case 0x802: {
    pend.offset = (cmd[1] | (u64)cmd[2] << 32) + se->u.se.data[BASE];
    pend.size = cmd[3];
    if (tracing() && slot) {
      u32 s = slot & 31u;
      if (fstat[s].reads < 4)
        trace_printf("fs: read %s @%llX +%X\n", fstat[s].path, cmd[1] | (u64)cmd[2] << 32, cmd[3]);
      fstat[s].reads++;
      fstat[s].rbytes += cmd[3];
    }
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
    if (tracing() && slot) {
      u32 s = slot & 31u;
      if (fstat[s].writes < 16)
        trace_printf("fs: write %s @%llX +%X\n", fstat[s].path, pend.offset, cmd[3]);
      fstat[s].writes++;
      fstat[s].wbytes += cmd[3];
    }
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
  case 0x80C: {
    Object *copy = NULL;
    u32 h = slot ? session_new(&fs_file_service, &copy) : 0;
    if (h) {
      for (int i = 0; i < 6; i++) copy->u.se.data[i] = se->u.se.data[i];
      links[slot & 31u]++;
    }
    cmd[0] = ipc_header(id, 1, 2);
    cmd[1] = h ? 0 : (slot ? RES_OUT_OF_HANDLES : RES_INVALID_HANDLE);
    cmd[2] = 0x10u;
    cmd[3] = h;
    return;
  }
  case 0x808:
    if (ra.slot == slot)
      ra_drop();
    f->file = slot;
    se->u.se.data[SLOT] = 0;
    if (slot && links[slot & 31u]) {
      links[slot & 31u]--;
      slot = 0;
    }
    if (slot) trace_close(slot);
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
  if (slot && links[slot & 31u]) {
    links[slot & 31u]--;
    return;
  }
  if (slot) {
    trace_close(slot);
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
  memset(links, 0, sizeof links);
  memset(fstat, 0, sizeof fstat);
}
