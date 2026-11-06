#include "aurora.h"
#include "aes.h"
#include "ff.h"
#include "sdmmc.h"
#include "sysnand.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const u8 n3ds_ctrnand_keyy[16] = {0x4D, 0x80, 0x4F, 0x4E, 0x99, 0x90, 0x19, 0x46,
                                         0x13, 0xA2, 0x04, 0xAC, 0x58, 0x44, 0x60, 0xBE};
static u8 ctr_base[16];
static u32 part_start;
static int part_slot;
static u32 sector[128];
static u32 chunk[0x4000];

static u32 ror(u32 x, int n) { return x >> n | x << (32 - n); }
static void sha256(const void *data, u32 len, u8 out[32]) {
  static const u32 k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  u8 block[128] = {0};
  u32 blocks = len + 9 <= 64 ? 1 : 2;
  memcpy(block, data, len);
  block[len] = 0x80;
  u64 bits = (u64)len * 8;
  for (int i = 0; i < 8; i++) block[blocks * 64 - 1 - i] = (u8)(bits >> (8 * i));
  for (u32 b = 0; b < blocks; b++) {
    u32 w[64], a[8];
    for (int i = 0; i < 16; i++) {
      const u8 *p = block + b * 64 + i * 4;
      w[i] = (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    }
    for (int i = 16; i < 64; i++) {
      u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, h, sizeof a);
    for (int i = 0; i < 64; i++) {
      u32 t1 = a[7] + (ror(a[4], 6) ^ ror(a[4], 11) ^ ror(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k[i] + w[i];
      u32 t2 = (ror(a[0], 2) ^ ror(a[0], 13) ^ ror(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
      memmove(a + 1, a, 7 * sizeof(u32));
      a[4] += t1;
      a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += a[i];
  }
  for (int i = 0; i < 32; i++) out[i] = (u8)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static int ctrnand_read(u32 at, u32 count, u8 *out) {
  u8 ctr[16];
  if ((u32)out & 3u) {
    for (u32 i = 0; i < count; i++) {
      if (ctrnand_read(at + i, 1, (u8 *)sector)) return -1;
      memcpy(out + i * 512u, sector, 512);
    }
    return 0;
  }
  while (count) {
    u32 n = count > 128u ? 128u : count;
    if (sdmmc_nand_readsectors(part_start + at, n, out)) return -1;
    memcpy(ctr, ctr_base, 16);
    aes_ctr_add(ctr, (part_start + at) * 32u);
    aes_ctr(part_slot, ctr, out, n * 512u);
    at += n, out += n * 512u, count -= n;
  }
  return 0;
}

static const char *ctrnand_open(void) {
  const u8 *h = (const u8 *)sector;
  u8 cid[16], hash[32];
  u32 start = 0, size = 0;
  int crypt = 0;
  if (sdmmc_nand_init() || sdmmc_nand_readsectors(0, 1, (u8 *)sector)) return "the NAND didn't answer";
  if (memcmp(h + 0x100, "NCSD", 4)) return "no NAND partition table";
  for (int i = 0; i < 8 && !size; i++) {
    u8 fs = h[0x110 + i], ct = h[0x118 + i];
    if (fs == 1 && (ct == 2 || ct == 3) && rd32(h + 0x124 + i * 8)) {
      start = rd32(h + 0x120 + i * 8);
      size = rd32(h + 0x124 + i * 8);
      crypt = ct;
    }
  }
  if (!size || !sdmmc_nand_cid(cid)) return "no CTRNAND partition";
  part_start = start;
  part_slot = crypt == 3 ? 0x05 : 0x04;
  if (part_slot == 0x05) aes_keyy(0x05, n3ds_ctrnand_keyy);
  sha256(cid, 16, hash);
  memcpy(ctr_base, hash, 16);
  if (ctrnand_read(0, 1, (u8 *)sector) || h[510] != 0x55 || h[511] != 0xAA)
    return "CTRNAND didn't decrypt (keys not set?)";
  g_nand_sectors = size;
  g_nand_read = ctrnand_read;
  return NULL;
}

static void ctrnand_close(void) {
  g_nand_read = 0;
  g_nand_sectors = 0;
}

static bool read_at(FIL *f, u32 at, void *buf, u32 n) {
  UINT got = 0;
  return f_lseek(f, at) == FR_OK && f_read(f, buf, n, &got) == FR_OK && got == n;
}

static void romfs_ctr(const u8 *h, u8 *ctr) {
  memset(ctr, 0, 16);
  if ((h[0x112] | h[0x113] << 8) == 1) {
    u32 x = rd32(h + 0x1B0) * (0x200u << h[0x18E]);
    for (int i = 0; i < 8; i++) ctr[i] = h[0x108 + i];
    for (int i = 0; i < 4; i++) ctr[12 + i] = (u8)(x >> (24 - 8 * i));
  } else {
    for (int i = 0; i < 8; i++) ctr[i] = h[0x10F - i];
    ctr[8] = 3;
  }
}

static const char *copy_romfs(FIL *in, const char *out_path) {
  static u8 h[0x200] __attribute__((aligned(4)));
  u8 ctr[16];
  if (!read_at(in, 0, h, sizeof h) || memcmp(h + 0x100, "NCCH", 4)) return "the .app isn't an NCCH";
  const u8 *flags = h + 0x188;
  u32 unit = 0x200u << flags[6];
  u32 off = rd32(h + 0x1B0) * unit, size = rd32(h + 0x1B4) * unit;
  bool crypt = !(flags[7] & 0x04);
  if (!off || !size) return "the title has no RomFS";
  if (crypt && ((flags[7] & 0x21) || flags[3])) return "it uses a key the ARM9 doesn't have";
  if (crypt) {
    aes_keyy(0x2C, h);
    romfs_ctr(h, ctr);
  }
  char tmp[160];
  snprintf(tmp, sizeof tmp, "%s.part", out_path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return "couldn't write to the SD card";
  const char *why = NULL;
  for (u32 done = 0; done < size && !why;) {
    u32 n = size - done > sizeof chunk ? sizeof chunk : size - done;
    if (!read_at(in, off + done, chunk, n)) why = "reading the NAND failed";
    else {
      if (crypt) aes_ctr(0x2C, ctr, chunk, (n + 15u) & ~15u);
      if (write(fd, chunk, n) != (int)n) why = "the SD card is full";
    }
    done += n;
  }
  close(fd);
  if (why || rename(tmp, out_path)) {
    unlink(tmp);
    return why ? why : "couldn't write to the SD card";
  }
  return NULL;
}

const char *sysnand_dump(uint64_t title, const char *out_path) {
  static FATFS nandfs;
  static FIL f;
  static DIR d;
  static FILINFO fi;
  char dir[64], app[128];
  const char *why = ctrnand_open();
  if (why) {
    ctrnand_close();
    return why;
  }
  if (f_mount(&nandfs, "1:", 1) != FR_OK) {
    ctrnand_close();
    return "CTRNAND wouldn't mount";
  }
  snprintf(dir, sizeof dir, "1:/title/%08lx/%08lx/content", (unsigned long)(title >> 32),
           (unsigned long)(u32)title);
  app[0] = 0;
  if (f_opendir(&d, dir) == FR_OK) {
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
      size_t n = strlen(fi.fname);
      if (!(fi.fattrib & AM_DIR) && n > 4 && !strcasecmp(fi.fname + n - 4, ".app")) {
        snprintf(app, sizeof app, "%s/%s", dir, fi.fname);
        break;
      }
    }
    f_closedir(&d);
  }
  if (!app[0]) why = "the console doesn't have that title";
  else if (f_open(&f, app, FA_READ) != FR_OK) why = "couldn't open its .app";
  else {
    mkdir("/Aurora/sysdata", 0777);
    why = copy_romfs(&f, out_path);
    f_close(&f);
  }
  f_mount(NULL, "1:", 0);
  ctrnand_close();
  return why;
}
