#include "ncch.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint64_t align64(uint64_t v) { return (v + 63u) & ~63ull; }

static bool read_at(FILE *f, uint64_t off, void *buf, size_t n) {
  return !fseek(f, (long)off, SEEK_SET) && fread(buf, 1, n, f) == n;
}

static bool note(char *why, size_t cap, const char *text) {
  if (why && cap) snprintf(why, cap, "%s", text);
  return false;
}

static bool parse_ncch(FILE *f, uint64_t at, NcchInfo *n, char *why, size_t cap) {
  uint8_t h[0x200];
  if (!read_at(f, at, h, sizeof h) || memcmp(h + 0x100, "NCCH", 4))
    return note(why, cap, "This doesn't look like a 3DS program (no NCCH inside).");
  n->ncch = at;
  n->unit = 0x200u << h[0x18E];
  n->title_id = rd32(h + 0x118) | (uint64_t)rd32(h + 0x11C) << 32;
  n->platform = h[0x18C];
  if (!(h[0x18F] & 4u))
    return note(why, cap, "This program is still encrypted, so it can't run here. Decrypted and unencrypted software can.");
  n->exefs = at + (uint64_t)rd32(h + 0x1A0) * n->unit;
  n->exefs_size = rd32(h + 0x1A4) * n->unit;
  n->romfs = rd32(h + 0x1B4) ? at + (uint64_t)rd32(h + 0x1B0) * n->unit : 0;
  if (rd32(h + 0x180) < 0x400 || !read_at(f, at + 0x200, n->exheader, sizeof n->exheader))
    return note(why, cap, "The program's ExHeader is missing.");
  n->ok = true;
  return true;
}

bool ncch_open(FILE *f, NcchInfo *n, char *why, size_t cap) {
  memset(n, 0, sizeof *n);
  uint8_t h[0x200];
  if (!read_at(f, 0, h, sizeof h))
    return note(why, cap, "The file is too short.");
  if (!memcmp(h + 0x100, "NCCH", 4))
    return parse_ncch(f, 0, n, why, cap);
  if (!memcmp(h + 0x100, "NCSD", 4))
    return parse_ncch(f, (uint64_t)rd32(h + 0x120) * 0x200u, n, why, cap);
  uint32_t hs = rd32(h);
  if (hs == 0x2020) {
    uint64_t cert = align64(hs), ticket = align64(cert + rd32(h + 0x08));
    uint64_t tmd = align64(ticket + rd32(h + 0x0C)), content = align64(tmd + rd32(h + 0x10));
    uint8_t probe[0x104];
    if (read_at(f, content, probe, sizeof probe) && memcmp(probe + 0x100, "NCCH", 4))
      return note(why, cap, "This CIA is still encrypted, so it can't run here. Decrypted and unencrypted software can.");
    return parse_ncch(f, content, n, why, cap);
  }
  return note(why, cap, "Not a .3dsx, .cia or .3ds program this runner understands.");
}

bool ncch_exefs_file(FILE *f, const NcchInfo *n, const char *name, uint8_t **out, uint32_t *size) {
  *out = NULL; *size = 0;
  uint8_t h[0x200];
  if (!n->ok || !n->exefs || !read_at(f, n->exefs, h, sizeof h)) return false;
  for (int i = 0; i < 10; i++) {
    const uint8_t *e = h + i * 16;
    if (strncmp((const char *)e, name, 8)) continue;
    uint32_t off = rd32(e + 8), len = rd32(e + 12);
    if (!len || len > n->exefs_size) return false;
    uint8_t *buf = malloc(len);
    if (!buf) return false;
    if (!read_at(f, n->exefs + 0x200 + off, buf, len)) { free(buf); return false; }
    *out = buf; *size = len;
    return true;
  }
  return false;
}

uint8_t *blz_decompress(const uint8_t *in, uint32_t in_size, uint32_t *out_size) {
  if (in_size < 8) return NULL;
  uint32_t top = rd32(in + in_size - 8), extra = rd32(in + in_size - 4);
  uint32_t out_len = in_size + extra;
  uint32_t index = in_size - (top >> 24), stop = in_size - (top & 0xFFFFFFu);
  if ((top >> 24) < 8 || (top & 0xFFFFFFu) > in_size || index > in_size) return NULL;
  uint8_t *out = calloc(1, out_len ? out_len : 1);
  if (!out) return NULL;
  memcpy(out, in, in_size);
  uint32_t o = out_len;
  while (index > stop) {
    uint8_t control = in[--index];
    for (int i = 0; i < 8 && index > stop; i++, control <<= 1) {
      if (control & 0x80) {
        if (index < 2 + stop) { free(out); return NULL; }
        index -= 2;
        uint32_t seg = in[index] | in[index + 1] << 8;
        uint32_t len = ((seg >> 12) & 15u) + 3u, disp = (seg & 0xFFFu) + 2u;
        if (o < len) { free(out); return NULL; }
        for (uint32_t j = 0; j < len; j++) {
          if (o + disp >= out_len) { free(out); return NULL; }
          uint8_t b = out[o + disp];
          out[--o] = b;
        }
      } else {
        if (!o) { free(out); return NULL; }
        out[--o] = in[--index];
      }
    }
  }
  *out_size = out_len;
  return out;
}

bool ncch_smdh(FILE *f, const NcchInfo *n, uint8_t smdh[0x36C0]) {
  uint8_t *icon; uint32_t size;
  if (!ncch_exefs_file(f, n, "icon", &icon, &size)) return false;
  bool ok = size >= 0x36C0 && !memcmp(icon, "SMDH", 4);
  if (ok) memcpy(smdh, icon, 0x36C0);
  free(icon);
  return ok;
}

uint32_t ncch_romfs_level3(FILE *f, const NcchInfo *n) {
  uint8_t h[0x60];
  if (!n->romfs || !read_at(f, n->romfs, h, sizeof h) || memcmp(h, "IVFC", 4)) return 0;
  uint32_t master = rd32(h + 0x08), log2 = rd32(h + 0x4C);
  uint64_t block = log2 >= 4 && log2 <= 20 ? 1ull << log2 : 0x1000u;
  uint64_t off = (0x60u + master + block - 1) & ~(block - 1);
  uint64_t abs = n->romfs + off;
  return abs < 0xFFFFFFFFull ? (uint32_t)abs : 0;
}

uint32_t ncch_code_size(const NcchInfo *n, bool *compressed) {
  const uint8_t *x = n->exheader;
  if (compressed) *compressed = (x[0x0D] & 1u) != 0;
  return (rd32(x + 0x14) + rd32(x + 0x24) + rd32(x + 0x34)) * 0x1000u;
}

uint8_t *ncch_build_packed(FILE *f, const NcchInfo *n, uint32_t *bytes, int *prm, char *why, size_t cap) {
  const uint8_t *x = n->exheader;
  uint32_t text_addr = rd32(x + 0x10), text_pages = rd32(x + 0x14);
  uint32_t ro_addr = rd32(x + 0x20), ro_pages = rd32(x + 0x24);
  uint32_t data_addr = rd32(x + 0x30), data_pages = rd32(x + 0x34), data_size = rd32(x + 0x38);
  uint32_t bss = rd32(x + 0x3C);
  if (text_addr != 0x00100000u || ro_addr != text_addr + text_pages * 0x1000u ||
      data_addr != ro_addr + ro_pages * 0x1000u) {
    note(why, cap, "The program's code layout isn't the usual one.");
    return NULL;
  }
  if (data_size > data_pages * 0x1000u) data_size = data_pages * 0x1000u;
  uint32_t header = 0x20 + 3 * 8, code_len = 0;
  uint8_t *code;
  if (!ncch_exefs_file(f, n, ".code", &code, &code_len) || code_len < 8) {
    note(why, cap, "The program has no code (.code is missing).");
    return NULL;
  }
  uint32_t top = rd32(code + code_len - 8), extra = rd32(code + code_len - 4);
  uint32_t out_len = code_len + extra, stop = code_len - (top & 0xFFFFFFu);
  uint32_t text = text_pages * 0x1000u, ro = ro_pages * 0x1000u;
  uint32_t image = text + ro + ((data_size + bss + 0xFFFu) & ~0xFFFu);
  if ((top >> 24) < 8 || (top & 0xFFFFFFu) > code_len || out_len < text + ro + data_size || out_len > image) {
    free(code);
    note(why, cap, "The program's code could not be decompressed.");
    return NULL;
  }
  *prm = stop >= 8 ? !memcmp(code + 4, "_prm", 4) : -1;
  uint8_t *out = realloc(code, code_len + header);
  if (!out) { free(code); note(why, cap, "Not enough memory to read it."); return NULL; }
  memmove(out + header, out, code_len);
  memset(out, 0, header);
  memcpy(out, "3DSX", 4);
  out[4] = 0x20; out[6] = 8;
  wr32(out + 0x0C, 0x80000000u);
  wr32(out + 0x10, text);
  wr32(out + 0x14, ro);
  wr32(out + 0x18, data_size + bss);
  wr32(out + 0x1C, bss);
  *bytes = code_len + header;
  return out;
}

uint8_t *ncch_build_3dsx(FILE *f, const NcchInfo *n, uint32_t *bytes, char *why, size_t cap) {
  const uint8_t *x = n->exheader;
  uint32_t text_addr = rd32(x + 0x10), text_pages = rd32(x + 0x14);
  uint32_t ro_addr = rd32(x + 0x20), ro_pages = rd32(x + 0x24);
  uint32_t data_addr = rd32(x + 0x30), data_pages = rd32(x + 0x34), data_size = rd32(x + 0x38);
  uint32_t bss = rd32(x + 0x3C);
  bool compressed = (x[0x0D] & 1u) != 0;
  if (text_addr != 0x00100000u || ro_addr != text_addr + text_pages * 0x1000u ||
      data_addr != ro_addr + ro_pages * 0x1000u) {
    note(why, cap, "The program's code layout isn't the usual one.");
    return NULL;
  }
  uint8_t *code; uint32_t code_len;
  if (!ncch_exefs_file(f, n, ".code", &code, &code_len)) {
    note(why, cap, "The program has no code (.code is missing).");
    return NULL;
  }
  if (compressed) {
    uint32_t raw_len;
    uint8_t *raw = blz_decompress(code, code_len, &raw_len);
    free(code);
    if (!raw) { note(why, cap, "The program's code could not be decompressed."); return NULL; }
    code = raw; code_len = raw_len;
  }
  uint32_t text = text_pages * 0x1000u, ro = ro_pages * 0x1000u;
  if (data_size > data_pages * 0x1000u) data_size = data_pages * 0x1000u;
  if ((uint64_t)text + ro + data_size > code_len) {
    free(code);
    note(why, cap, "The program's code is shorter than its ExHeader says.");
    return NULL;
  }
  uint32_t header = 0x20 + 3 * 8, total = header + text + ro + data_size;
  uint8_t *out = calloc(1, total);
  if (!out) { free(code); note(why, cap, "Not enough memory to read it."); return NULL; }
  memcpy(out, "3DSX", 4);
  out[4] = 0x20; out[6] = 8;
  wr32(out + 0x10, text);
  wr32(out + 0x14, ro);
  wr32(out + 0x18, data_size + bss);
  wr32(out + 0x1C, bss);
  memcpy(out + header, code, text + ro + data_size);
  free(code);
  *bytes = total;
  return out;
}

uint32_t ncch_stack_size(const NcchInfo *n) { return rd32(n->exheader + 0x1C); }

uint32_t ncch_app_memory(const NcchInfo *n, bool new3ds) {
  uint32_t o3ds = n->exheader[0x20E] >> 4, n3ds = n->exheader[0x20D] & 0xFu;
  if (new3ds && n3ds == 1) return 0x07C00000u | 6u;
  if (new3ds && n3ds == 2) return 0x0B200000u | 7u;
  static const uint32_t sizes[6] = {0x04000000u, 0, 0x06000000u, 0x05000000u, 0x04800000u, 0x02000000u};
  return o3ds < 6 && sizes[o3ds] ? sizes[o3ds] | o3ds : 0x04000000u;
}

int ncch_services(const NcchInfo *n, char names[34][9]) {
  int count = 0;
  for (int i = 0; i < 34; i++) {
    const uint8_t *p = n->exheader + (i < 32 ? 0x250 + i * 8 : 0x360 + (i - 32) * 8);
    if (!p[0]) continue;
    for (int k = 0; k < 8; k++) names[count][k] = (p[k] >= 0x20 && p[k] < 0x7F) ? (char)p[k] : 0;
    names[count][8] = 0;
    count++;
  }
  return count;
}

uint32_t ncch_kernel_version(const NcchInfo *n) {
  for (uint32_t off = 0x370; off < 0x3E0; off += 4) {
    uint32_t d = rd32(n->exheader + off);
    if ((d & 0xFE000000u) == 0xFC000000u) return d & 0xFFFFu;
  }
  return 0;
}
