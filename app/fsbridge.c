#include <aurora_app.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fsbridge.h"
#include "sysnand.h"
#include <stdlib.h>

#ifndef BUF_PTR
#define BUF_PTR(pa, size) ((void *)(pa))
#endif

#define SLOTS 32
static int files[SLOTS];
static FsDir *dirs[SLOTS];

#define R_NOT_FOUND 0xC8804478u
#define R_EXISTS    0xC82044BEu
#define R_FULL      0xC86044D2u
#define R_FAIL      0xC8A04554u

static int32_t result_of(int e) {
  switch (e) {
  case ENOENT:
  case ENOTDIR:
    return (int32_t)R_NOT_FOUND;
  case EEXIST:
    return (int32_t)R_EXISTS;
  case ENOSPC:
    return (int32_t)R_FULL;
  default:
    return (int32_t)R_FAIL;
  }
}

static int fd_of(uint32_t slot) { return slot && slot < SLOTS ? files[slot] - 1 : -1; }

static bool remove_tree(const char *path) {
  FsDir *d = fs_dir_open(path);
  if (!d)
    return false;
  FsEntry e;
  static char child[600];
  bool ok = true;
  while (ok && fs_dir_read(d, &e)) {
    char *sub = child;
    snprintf(sub, sizeof child, "%s/%s", path, e.name);
    if (e.is_dir) {
      char copy[600];
      snprintf(copy, sizeof copy, "%s", sub);
      fs_dir_close(d);
      ok = remove_tree(copy);
      d = ok ? fs_dir_open(path) : NULL;
      if (!d)
        return false;
    } else {
      ok = unlink(sub) == 0;
    }
  }
  fs_dir_close(d);
  return ok && rmdir(path) == 0;
}

void fsbridge_do(JobFs *f) {
  f->result = 0;
  switch (f->op) {
  case FSOP_OPEN: {
    uint32_t slot = 1;
    while (slot < SLOTS && files[slot])
      slot++;
    if (slot == SLOTS) {
      f->result = (int32_t)R_FAIL;
      break;
    }
    struct stat st;
    if (!stat(f->path, &st) && S_ISDIR(st.st_mode)) {
      f->result = (int32_t)R_NOT_FOUND;
      break;
    }
    int fl = (f->flags & 2u) ? O_RDWR : O_RDONLY;
    if (f->flags & 4u)
      fl |= O_CREAT;
    int fd = open(f->path, fl, 0666);
    if (fd < 0 && errno == ENOENT && !(f->flags & 2u) && !strncmp(f->path, "/Aurora/sysdata/", 16) &&
        strlen(f->path) == 16 + 16 + 6 && !strcmp(f->path + 32, ".romfs")) {
      char hex[17];
      memcpy(hex, f->path + 16, 16);
      hex[16] = 0;
      const char *why = sysnand_dump(strtoull(hex, NULL, 16), f->path);
      snprintf(f->path2, sizeof f->path2, "%s", why ? why : "copied from the system NAND");
      if (!why)
        fd = open(f->path, fl, 0666);
    }
    if (fd < 0) {
      f->result = result_of(errno);
      break;
    }
    files[slot] = fd + 1;
    f->value = slot;
    break;
  }
  case FSOP_READ:
  case FSOP_WRITE: {
    int fd = fd_of(f->file);
    if (fd < 0 || f->offset > 0x7FFFFFFFull) {
      f->result = (int32_t)R_FAIL;
      break;
    }
    if (lseek(fd, (off_t)f->offset, SEEK_SET) < 0) {
      f->result = result_of(errno);
      break;
    }
    void *buf = BUF_PTR(f->buf, f->size);
    int n = f->op == FSOP_READ ? (int)read(fd, buf, f->size) : (int)write(fd, buf, f->size);
    if (n < 0) {
      f->result = result_of(errno);
      break;
    }
    if (f->op == FSOP_WRITE && (f->flags & 1u))
      fsync(fd);
    f->value = (uint32_t)n;
    break;
  }
  case FSOP_SIZE: {
    struct stat st;
    int fd = fd_of(f->file);
    if (fd < 0 || fstat(fd, &st)) {
      f->result = (int32_t)R_FAIL;
      break;
    }
    f->value64 = (uint64_t)st.st_size;
    break;
  }
  case FSOP_SETSIZE: {
    int fd = fd_of(f->file);
    if (fd < 0 || f->value64 > 0x7FFFFFFFull || ftruncate(fd, (off_t)f->value64))
      f->result = fd < 0 ? (int32_t)R_FAIL : result_of(errno);
    break;
  }
  case FSOP_CLOSE: {
    int fd = fd_of(f->file);
    if (fd >= 0) {
      close(fd);
      files[f->file] = 0;
    }
    break;
  }
  case FSOP_DELETE:
    if (unlink(f->path))
      f->result = result_of(errno);
    break;
  case FSOP_RENAME:
    if (rename(f->path, f->path2))
      f->result = result_of(errno);
    break;
  case FSOP_MKDIR:
    if (mkdir(f->path, 0777))
      f->result = result_of(errno);
    break;
  case FSOP_RMDIR:
    if (f->flags & 1u) {
      if (!fs_is_dir(f->path))
        f->result = (int32_t)R_NOT_FOUND;
      else if (!remove_tree(f->path))
        f->result = (int32_t)R_FAIL;
    } else if (rmdir(f->path)) {
      f->result = result_of(errno);
    }
    break;
  case FSOP_CREATE: {
    if (fs_exists(f->path)) {
      f->result = (int32_t)R_EXISTS;
      break;
    }
    int fd = open(f->path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      f->result = result_of(errno);
      break;
    }
    if (f->value64 && (f->value64 > 0x7FFFFFFFull || ftruncate(fd, (off_t)f->value64)))
      f->result = (int32_t)R_FULL;
    close(fd);
    break;
  }
  case FSOP_OPENDIR: {
    uint32_t slot = 1;
    while (slot < SLOTS && dirs[slot])
      slot++;
    FsDir *d = slot < SLOTS ? fs_dir_open(f->path) : NULL;
    if (!d) {
      f->result = slot < SLOTS ? (int32_t)R_NOT_FOUND : (int32_t)R_FAIL;
      break;
    }
    dirs[slot] = d;
    f->value = slot;
    break;
  }
  case FSOP_READDIR: {
    FsDir *d = f->file && f->file < SLOTS ? dirs[f->file] : NULL;
    JobDirent *out = (JobDirent *)BUF_PTR(f->buf, f->size * (uint32_t)sizeof(JobDirent));
    if (!d || !out) {
      f->result = (int32_t)R_FAIL;
      break;
    }
    uint32_t n = 0;
    FsEntry e;
    while (n < f->size && fs_dir_read(d, &e)) {
      memset(&out[n], 0, sizeof out[n]);
      snprintf(out[n].name, sizeof out[n].name, "%s", e.name);
      out[n].is_dir = e.is_dir;
      out[n].size = e.size;
      n++;
    }
    f->value = n;
    break;
  }
  case FSOP_CLOSEDIR:
    if (f->file && f->file < SLOTS && dirs[f->file]) {
      fs_dir_close(dirs[f->file]);
      dirs[f->file] = NULL;
    }
    break;
  case FSOP_STAT: {
    struct stat st;
    if (stat(f->path, &st))
      f->result = result_of(errno);
    else
      f->value = S_ISDIR(st.st_mode) ? 2u : 1u;
    break;
  }
  case FSOP_FREE:

    f->value64 = 2ull << 30;
    f->offset = 32ull << 30;
    f->value = 0x8000;
    break;
  default:
    f->result = (int32_t)R_FAIL;
  }
}

void fsbridge_drop(uint32_t files_mask, uint32_t dirs_mask) {
  for (uint32_t i = 1; i < SLOTS; i++) {
    if ((files_mask >> i) & 1u && files[i]) {
      close(files[i] - 1);
      files[i] = 0;
    }
    if ((dirs_mask >> i) & 1u && dirs[i]) {
      fs_dir_close(dirs[i]);
      dirs[i] = NULL;
    }
  }
}

void fsbridge_close_all(void) { fsbridge_drop(0xFFFFFFFFu, 0xFFFFFFFFu); }
