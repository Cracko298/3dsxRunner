/* The cheat menu. The game is paused while it's open (clock and audio too),
 * and it draws into private RAM buffers, never into the game's VRAM. */

#include <aurora_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheatfile.h"
#include "job.h"
#include "overlay.h"
#include "titlecfg.h"

extern void os_cache_sync(void);

#ifndef OVERLAY_JOB
#define OVERLAY_JOB ((JobParams *)JOB_PARAMS)
#define ARM9_PA(p) ((uint32_t)(uintptr_t)(p))
#define ARM9_MALLOC(n) malloc(n)
#endif
#define job OVERLAY_JOB

#define BG     RGB(16, 20, 30)
#define PANEL  RGB(30, 36, 52)
#define DIM    RGB(150, 160, 180)
#define GOOD   RGB(110, 220, 140)
#define BAD    RGB(255, 120, 110)

typedef struct {
  uint32_t va, type, value;
} Freeze;

#define MAX_FREEZES 32
#define HIT_CAP     (256u * 1024u)
#define MAX_PROG    32

static struct {
  char title[64], name[200], settings_path[256];
  char cheat_path[256], state_path[256];
  CheatList cheats;
  int prog_index[MAX_PROG];
  Freeze freezes[MAX_FREEZES];
  int n_freezes;
  uint32_t s_type, s_cmp, s_value, s_flags;
  JobHit *hits;
  uint32_t count;
  bool overflow, searched;
  uint32_t hex_va;
  bool active;
} ov;

static uint32_t hotkey = KEY_SELECT;
static uint32_t masked;
static char toast[120];

static const struct {
  const char *name;
  uint32_t keys;
} hotkeys[] = {
    {"SELECT", KEY_SELECT},
    {"L + SELECT", KEY_L | KEY_SELECT},
    {"R + SELECT", KEY_R | KEY_SELECT},
    {"L + R + SELECT", KEY_L | KEY_R | KEY_SELECT},
    {"L + DOWN", KEY_L | KEY_DOWN},
};
#define N_HOTKEYS (int)(sizeof hotkeys / sizeof hotkeys[0])

static const char *const type_names[] = {"u8", "u16", "u32", "float"};
static const char *const cmp_names[] = {"equal to",   "not equal to", "bigger than", "smaller than",
                                        "changed",    "unchanged",    "bigger",      "smaller",
                                        "bigger by",  "smaller by"};

static uint32_t accent(void) {
  uint32_t a = sys_accent();
  return a ? a : COLOR_AURORA;
}

static char *read_text(const char *path,size_t *size) {
  *size=0;
  FILE *f=fopen(path,"rb"); if(!f) return NULL;
  if(fseek(f,0,SEEK_END)) { fclose(f); return NULL; }
  long n=ftell(f);
  if(n<0 || n>256*1024 || fseek(f,0,SEEK_SET)) { fclose(f); return NULL; }
  char *text=malloc((size_t)n+1);
  if(!text) { fclose(f); return NULL; }
  bool ok=fread(text,1,(size_t)n,f)==(size_t)n;
  fclose(f);
  if(!ok) { free(text); return NULL; }
  text[n]=0; *size=(size_t)n; return text;
}

static void settings_path(char *out, size_t cap) { snprintf(out, cap, "%s", ov.settings_path); }

void overlay_settings_load(void) {
  char path[256];
  settings_path(path, sizeof path);
  size_t n;
  hotkey=KEY_SELECT;
  char *text = read_text(path, &n);
  if (!text)
    return;
  const char *k = strstr(text, "hotkey=");
  if (k)
    for (int i = 0; i < N_HOTKEYS; i++)
      if (!strncmp(k + 7, hotkeys[i].name, strlen(hotkeys[i].name)) &&
          (k[7 + strlen(hotkeys[i].name)] == '\n' || k[7 + strlen(hotkeys[i].name)] == '\r' ||
           !k[7 + strlen(hotkeys[i].name)]))
        hotkey = hotkeys[i].keys;
  free(text);
}

static void settings_save(void) {
  char path[256], text[64];
  settings_path(path, sizeof path);
  const char *name = "SELECT";
  for (int i = 0; i < N_HOTKEYS; i++)
    if (hotkeys[i].keys == hotkey)
      name = hotkeys[i].name;
  int n = snprintf(text, sizeof text, "hotkey=%s\n", name);
  if(!title_prepare(ov.name) || !fs_write_file(path,text,(size_t)n))
    snprintf(toast,sizeof toast,"Could not save the hotkey.");
}

static bool mem_op(void) {
  os_cache_sync();
  if (!runner_job(JOB_MEM))
    return false;
  os_cache_sync();
  return job->mem.result == 0 && job->yield!=YIELD_ERROR && job->yield!=YIELD_CRASH;
}

static uint8_t *xfer;

static bool lend_page(void) {
  if (!xfer)
    xfer = ARM9_MALLOC(4096);
  return xfer != NULL;
}

static bool mem_read(uint32_t va, void *buf, uint32_t size) {
  if (!lend_page() || size > 4096)
    return false;
  memset(&job->mem, 0, sizeof job->mem);
  job->mem.op = MEM_READ;
  job->mem.va = va;
  job->mem.size = size;
  job->mem.buf = ARM9_PA(xfer);
  if (!mem_op())
    return false;
  memcpy(buf, xfer, size);
  return true;
}

static bool mem_write(uint32_t va, const void *buf, uint32_t size) {
  if (!lend_page() || size > 4096)
    return false;
  memcpy(xfer, buf, size);
  memset(&job->mem, 0, sizeof job->mem);
  job->mem.op = MEM_WRITE;
  job->mem.va = va;
  job->mem.size = size;
  job->mem.buf = ARM9_PA(xfer);
  return mem_op();
}

static uint32_t width_of(uint32_t type) { return type == MT_U8 ? 1u : type == MT_U16 ? 2u : 4u; }

static uint32_t *freeze_lines(uint32_t *lines) {
  static uint32_t c[MAX_FREEZES * 6];
  uint32_t n = 0;
  for (int i = 0; i < ov.n_freezes; i++) {
    const Freeze *f = &ov.freezes[i];
    uint32_t t = f->type == MT_U8 ? 0x20000000u : f->type == MT_U16 ? 0x10000000u : 0u;
    c[n * 2] = 0xD3000000u, c[n * 2 + 1] = f->va, n++;
    c[n * 2] = t, c[n * 2 + 1] = f->value, n++;
    c[n * 2] = 0xD2000000u, c[n * 2 + 1] = 0, n++;
  }
  *lines = n;
  return c;
}

static void push_cheats(void) {
  uint32_t extra_lines, bytes;
  uint32_t *extra = freeze_lines(&extra_lines);
  uint32_t *prog = cheatfile_program(&ov.cheats, extra, extra_lines, &bytes, ov.prog_index, MAX_PROG);
  static uint8_t *lent;
  static uint32_t lent_cap;
  if(bytes==UINT32_MAX) {
    snprintf(toast,sizeof toast,"Too many enabled codes (32 cheats / 32 KiB max).");
    return;
  }
  if (bytes > lent_cap) {
    uint32_t new_cap=(bytes+4095u)&~4095u;
    uint8_t *grown=ARM9_MALLOC(new_cap);
    if(grown) { free(lent); lent=grown; lent_cap=new_cap; }
    else { snprintf(toast,sizeof toast,"Not enough memory for cheats."); free(prog); return; }
  }
  if (bytes && !lent) {
    snprintf(toast, sizeof toast, "Not enough memory for the cheats.");
    free(prog);
    return;
  }
  if (bytes)
    memcpy(lent, prog, bytes);
  job->cheat_pa = bytes ? ARM9_PA(lent) : 0;
  job->cheat_bytes = bytes;
  os_cache_sync();
  if (!runner_job(JOB_CHEATS) || job->mem.result)
    snprintf(toast, sizeof toast, "The cheats could not be given to the program.");
  free(prog);
}

static void save_state(void) {
  char *text = malloc(16384);
  if (!text)
    return;
  size_t n = 0;
  for (int i = 0; i < ov.cheats.n && n < 16000; i++)
    if (ov.cheats.c[i].enabled)
      n += (size_t)snprintf(text + n, 16384 - n, "%s\n", ov.cheats.c[i].name);
  if(!title_prepare(ov.name) || !fs_write_file(ov.state_path,text,n))
    snprintf(toast,sizeof toast,"Could not save enabled cheats.");
  free(text);
}

static void load_cheats(void) {
  cheatfile_free(&ov.cheats);
  size_t n;
  char *text = read_text(ov.cheat_path, &n);
  if (!text)
    return;
  cheatfile_parse(text, &ov.cheats);
  free(text);
  char *on = read_text(ov.state_path, &n);
  if (!on)
    return;
  for (char *line = on; *line;) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = 0;
    for (int i = 0; i < ov.cheats.n; i++)
      if (!strcmp(ov.cheats.c[i].name, line))
        ov.cheats.c[i].enabled = true;
    if (!nl)
      break;
    line = nl + 1;
  }
  free(on);
}

void overlay_game_start(const char *title, const char *path) {
  JobHit *hits = ov.hits;
  memset(&ov, 0, sizeof ov);
  ov.hits = hits;
  snprintf(ov.title, sizeof ov.title, "%s", title);
  snprintf(ov.name,sizeof ov.name,"%s",path);
  if(!title_path(path,"cheats.txt",ov.cheat_path) ||
     !title_path(path,"enabled.txt",ov.state_path) ||
     !title_path(path,"overlay.cfg",ov.settings_path)) return;
  overlay_settings_load();
  ov.s_type = MT_U32;
  ov.active = true;
  load_cheats();
  job->cheats_on = 1;
  for (int i = 0; i < ov.cheats.n; i++)
    if (ov.cheats.c[i].enabled) {
      push_cheats();
      break;
    }
  masked = 0;
}

void overlay_game_end(void) {
  cheatfile_free(&ov.cheats);
  free(ov.hits); ov.hits=NULL;
  ov.active = false;
}

bool overlay_hotkey(uint32_t down, uint32_t held) {
  return ov.active && !(masked & hotkey) && (down & hotkey) && (held & hotkey) == hotkey;
}

uint32_t overlay_mask(uint32_t held) {
  masked &= held;
  return masked;
}

static uint32_t pad(void) {
  static uint32_t since, last_rep;
  uint32_t down = hid_keys_down(), held = hid_keys_held(), now = sys_millis();
  const uint32_t rep = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_CPAD_UP | KEY_CPAD_DOWN;
  if (down & rep)
    since = now, last_rep = now;
  else if ((held & rep) && now - since > 350 && now - last_rep > 70) {
    last_rep = now;
    down |= held & rep;
  }
  if (down & KEY_CPAD_UP) down |= KEY_UP;
  if (down & KEY_CPAD_DOWN) down |= KEY_DOWN;
  return down;
}

static bool frame(void) {
  if (!runner_overlay_frame())
    exit(0);
  return true;
}

static bool hotkey_again(uint32_t d) { return (d & hotkey) && (hid_keys_held() & hotkey) == hotkey; }

static void header(const char *title, const char *hint) {
  gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 240, BG);
  gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 24, PANEL);
  gfx_print(SCREEN_BOTTOM, 8, 4, FONT_BOLD, accent(), "%s", title);
  if (hint)
    gfx_print(SCREEN_BOTTOM, 8, 226, FONT_SMALL, DIM, "%s", hint);
  if (toast[0]) {
    gfx_rect(SCREEN_BOTTOM, 0, 209, 320, 15, PANEL);
    gfx_print(SCREEN_BOTTOM, 8, 210, FONT_SMALL, COLOR_YELLOW, "%.56s", toast);
  }
}

static void top_banner(void) {
  gfx_rect_blend(SCREEN_TOP, 0, 0, 400, 240, RGB(0, 0, 0), 120);
  gfx_rect(SCREEN_TOP, 0, 0, 400, 30, PANEL);
  gfx_print(SCREEN_TOP, 10, 6, FONT_BOLD, COLOR_WHITE, "Paused: %.40s", ov.title);
  const char *name = "the hotkey";
  for (int i = 0; i < N_HOTKEYS; i++)
    if (hotkeys[i].keys == hotkey) name = hotkeys[i].name;
  gfx_print(SCREEN_TOP, 10, 214, FONT_SMALL, COLOR_WHITE, "B: back    %s or START: back to the program", name);
}

typedef void (*LabelFn)(int i, char *buf, size_t cap, uint32_t *color);

#define LIST_ROWS 9
#define LIST_Y    28
#define LIST_H    20

static int list(const char *title, const char *hint, int n, int *sel, LabelFn label, uint32_t *key) {
  int scroll = 0;
  for (;;) {
    if (*sel >= n) *sel = n ? n - 1 : 0;
    if (*sel < scroll) scroll = *sel;
    if (*sel >= scroll + LIST_ROWS) scroll = *sel - LIST_ROWS + 1;
    header(title, hint);
    for (int r = 0; r < LIST_ROWS && scroll + r < n; r++) {
      int i = scroll + r, y = LIST_Y + r * LIST_H;
      char buf[96];
      uint32_t color = COLOR_WHITE;
      label(i, buf, sizeof buf, &color);
      if (i == *sel) {
        gfx_round_rect(SCREEN_BOTTOM, 4, y, 312, LIST_H - 2, 5, accent());
        color = COLOR_BLACK;
      }
      gfx_print(SCREEN_BOTTOM, 10, y + 2, FONT_REGULAR, color, "%s", buf);
    }
    if (!n)
      gfx_print(SCREEN_BOTTOM, 10, LIST_Y + 4, FONT_REGULAR, DIM, "(nothing here)");
    if (n > LIST_ROWS)
      gfx_print(SCREEN_BOTTOM, 270, 6, FONT_SMALL, DIM, "%d/%d", *sel + 1, n);
    frame();
    uint32_t d = pad();
    if (d & (KEY_A | KEY_X | KEY_Y))
      toast[0] = 0;
    if (d & KEY_DOWN) *sel = n ? (*sel + 1) % n : 0;
    if (d & KEY_UP) *sel = n ? (*sel + n - 1) % n : 0;
    if (d & (KEY_RIGHT | KEY_R)) *sel = *sel + LIST_ROWS < n ? *sel + LIST_ROWS : (n ? n - 1 : 0);
    if (d & (KEY_LEFT | KEY_L)) *sel = *sel >= LIST_ROWS ? *sel - LIST_ROWS : 0;
    if (hid_touch_down()) {
      int r = (hid_touch_y() - LIST_Y) / LIST_H;
      if (hid_touch_y() >= LIST_Y && r < LIST_ROWS && scroll + r < n) {
        if (scroll + r == *sel) d |= KEY_A;
        *sel = scroll + r;
      }
    }
    if (hotkey_again(d) || (d & KEY_START)) return -2;
    if (d & KEY_B) return -1;
    if (n && (d & (KEY_A | KEY_X | KEY_Y))) {
      *key = d & KEY_A ? KEY_A : d & KEY_X ? KEY_X : KEY_Y;
      return *sel;
    }
  }
}

static void message(const char *title, const char *text) {
  for (;;) {
    header(title, "A or B: back");
    int y = 34;
    const char *p = text;
    char line[64];
    while (*p && y < 200) {
      int take = (int)strlen(p) > 46 ? 46 : (int)strlen(p);
      if ((int)strlen(p) > 46)
        for (int k = take; k > 20; k--)
          if (p[k] == ' ') {
            take = k;
            break;
          }
      memcpy(line, p, (size_t)take);
      line[take] = 0;
      gfx_print(SCREEN_BOTTOM, 10, y, FONT_REGULAR, COLOR_WHITE, "%s", line);
      p += take;
      while (*p == ' ') p++;
      y += 18;
    }
    frame();
    if (hid_keys_down() & (KEY_A | KEY_B)) return;
  }
}

enum { ED_HEX, ED_DEC, ED_FLOAT };

static bool edit_number(const char *title, uint32_t *v, uint32_t type, bool address) {
  int mode = address ? ED_HEX : type == MT_FLOAT ? ED_FLOAT : ED_DEC;
  uint32_t w = address ? 4u : width_of(type);
  uint64_t max = w == 4 ? 0xFFFFFFFFull : w == 2 ? 0xFFFFull : 0xFFull;
  int64_t x;
  if (mode == ED_FLOAT) {
    union { uint32_t u; float f; } c = {*v};
    float f = c.f;
    if (f != f || f > 9999999.0f || f < -9999999.0f) f = 0;
    x = (int64_t)(f * 1000.0f + (f < 0 ? -0.5f : 0.5f));
  } else {
    x = (int64_t)(*v & max);
  }
  int cur = 0;
  for (;;) {
    int digits = mode == ED_HEX ? (int)w * 2 : mode == ED_DEC ? (w == 4 ? 10 : w == 2 ? 5 : 3) : 10;
    if (cur >= digits + (mode == ED_FLOAT)) cur = 0;
    header(title, mode == ED_FLOAT ? "Up/Down: change  Left/Right: move  A: done  B: cancel"
                                   : "Up/Down: change  Left/Right: move  Y: hex/dec  A: done");
    char text[24];
    int n = 0;
    uint64_t mag = (uint64_t)(x < 0 ? -x : x);
    if (mode == ED_FLOAT)
      text[n++] = x < 0 ? '-' : '+';
    for (int k = digits - 1; k >= 0; k--) {
      uint64_t base = mode == ED_HEX ? 16u : 10u, p = 1;
      for (int j = 0; j < k; j++) p *= base;
      int d = (int)((mag / p) % base);
      text[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
      if (mode == ED_FLOAT && k == 3) text[n++] = '.';
    }
    text[n] = 0;
    int cw = 20, x0 = (320 - n * cw) / 2;
    gfx_round_rect(SCREEN_BOTTOM, x0 - 8, 70, n * cw + 16, 44, 8, PANEL);
    int at = 0, pos = 0;
    for (int i = 0; i < n; i++) {
      if (text[i] == '.') continue;
      if (at == cur) pos = i;
      at++;
    }
    for (int i = 0; i < n; i++)
      gfx_print(SCREEN_BOTTOM, x0 + i * cw + 3, 80, FONT_TITLE, i == pos ? accent() : COLOR_WHITE, "%c", text[i]);
    gfx_rect(SCREEN_BOTTOM, x0 + pos * cw, 110, cw - 2, 3, accent());
    if (mode == ED_HEX && !address)
      gfx_print(SCREEN_BOTTOM, 10, 130, FONT_SMALL, DIM, "= %llu", (unsigned long long)mag);
    else if (mode == ED_DEC)
      gfx_print(SCREEN_BOTTOM, 10, 130, FONT_SMALL, DIM, "= 0x%llX", (unsigned long long)mag);
    frame();
    uint32_t d = pad();
    int total = digits + (mode == ED_FLOAT);
    if (d & KEY_LEFT) cur = (cur + total - 1) % total;
    if (d & KEY_RIGHT) cur = (cur + 1) % total;
    if (d & (KEY_UP | KEY_DOWN)) {
      int dir = d & KEY_UP ? 1 : -1;
      if (mode == ED_FLOAT && cur == 0) {
        x = -x;
      } else {
        int k = digits - 1 - (cur - (mode == ED_FLOAT));
        uint64_t base = mode == ED_HEX ? 16u : 10u, p = 1;
        for (int j = 0; j < k; j++) p *= base;
        int dg = (int)((mag / p) % base), nd = (dg + dir + (int)base) % (int)base;
        mag = mag - (uint64_t)dg * p + (uint64_t)nd * p;
        x = x < 0 ? -(int64_t)mag : (int64_t)mag;
      }
    }
    if ((d & KEY_Y) && !address && mode != ED_FLOAT) {
      mode = mode == ED_HEX ? ED_DEC : ED_HEX;
      cur = 0;
    }
    if (d & KEY_B) return false;
    if (d & KEY_A) {
      if (mode == ED_FLOAT) {
        union { float f; uint32_t u; } c = {(float)x / 1000.0f};
        *v = c.u;
      } else {
        *v = (uint32_t)((uint64_t)(x < 0 ? -x : x) > max ? max : (uint64_t)x);
      }
      return true;
    }
  }
}

static void format_value(char *out, size_t cap, uint32_t type, uint32_t v) {
  if (type == MT_FLOAT) {
    union { uint32_t u; float f; } c = {v};
    float f = c.f;
    if (f != f)
      snprintf(out, cap, "NaN");
    else if (f > 2e9f || f < -2e9f)
      snprintf(out, cap, "%s(huge)", f < 0 ? "-" : "");
    else {
      long whole = (long)f;
      int frac = (int)((f - (float)whole) * 1000.0f);
      if (frac < 0) frac = -frac;
      snprintf(out, cap, "%s%ld.%03d", f < 0 && whole == 0 ? "-" : "", whole, frac);
    }
  } else {
    snprintf(out, cap, "%lu (0x%lX)", (unsigned long)v, (unsigned long)v);
  }
}

static bool freeze(uint32_t va, uint32_t type, uint32_t value) {
  for (int i = 0; i < ov.n_freezes; i++)
    if (ov.freezes[i].va == va) {
      ov.freezes[i].type = type, ov.freezes[i].value = value;
      push_cheats();
      return true;
    }
  if (ov.n_freezes == MAX_FREEZES) {
    snprintf(toast, sizeof toast, "%d values are frozen already.", MAX_FREEZES);
    return false;
  }
  ov.freezes[ov.n_freezes++] = (Freeze){va, type, value};
  push_cheats();
  return true;
}

static void save_cheat(uint32_t va, uint32_t type, uint32_t value) {
  char name[64];
  snprintf(name, sizeof name, "Value at %08lX", (unsigned long)va);
  uint32_t t = type == MT_U8 ? 0x20000000u : type == MT_U16 ? 0x10000000u : 0u;
  char text[256];
  int n;
  if (va < 0x10000000u)
    n = snprintf(text, sizeof text, "\n[%s]\n%08lX %08lX\n", name, (unsigned long)(t | va), (unsigned long)value);
  else
    n = snprintf(text, sizeof text, "\n[%s]\nD3000000 %08lX\n%08lX %08lX\nD2000000 00000000\n", name,
                 (unsigned long)va, (unsigned long)t, (unsigned long)value);
  size_t old = 0;
  char *had = read_text(ov.cheat_path, &old);
  if(!had && fs_exists(ov.cheat_path)) {
    snprintf(toast,sizeof toast,"Existing cheat file is unreadable or too large; kept unchanged."); return;
  }
  if(old+(size_t)n>256u*1024u || !title_prepare(ov.name)) {
    free(had); snprintf(toast,sizeof toast,"Cannot extend this cheat file."); return;
  }
  char *all = malloc(old + (size_t)n + 1);
  if (!all) {
    free(had);
    return;
  }
  if (had)
    memcpy(all, had, old);
  memcpy(all + old, text, (size_t)n);
  bool ok = fs_write_file(ov.cheat_path, all, old + (size_t)n);
  free(all);
  free(had);
  if (!ok) {
    snprintf(toast, sizeof toast, "SD:%.80s could not be written.", ov.cheat_path);
    return;
  }
  bool on[256] = {false};
  for (int i = 0; i < ov.cheats.n && i < 256; i++) on[i] = ov.cheats.c[i].enabled;
  load_cheats();
  for (int i = 0; i < ov.cheats.n && i < 256; i++) ov.cheats.c[i].enabled |= on[i];
  if (ov.cheats.n) ov.cheats.c[ov.cheats.n - 1].enabled = true;
  save_state();
  push_cheats();
  snprintf(toast, sizeof toast, "Saved as \"%s\" and turned on.", name);
}

static void hex_editor(uint32_t va) {
  ov.hex_va = va & ~0xFu;
  static uint8_t buf[128];
  int cur = 0;
  for (;;) {
    uint32_t base = ov.hex_va & ~0x7Fu;
    bool there = mem_read(base, buf, 128);
    header("Hex editor", "A: edit  Y: go to  X: freeze u32  L/R: page");
    for (int r = 0; r < 16; r++) {
      int y = 30 + r * 11;
      uint32_t line = base + (uint32_t)r * 8u;
      gfx_text(SCREEN_BOTTOM, 4, y, DIM, "%08lX", (unsigned long)line);
      for (int k = 0; k < 8; k++) {
        int i = r * 8 + k, x = 76 + k * 20;
        bool on = i == cur;
        if (on) gfx_rect(SCREEN_BOTTOM, x - 2, y - 2, 20, 11, accent());
        if (there)
          gfx_text(SCREEN_BOTTOM, x, y, on ? COLOR_BLACK : COLOR_WHITE, "%02X", buf[i]);
        else
          gfx_text(SCREEN_BOTTOM, x, y, BAD, "--");
        char ch = there && buf[i] >= 0x20 && buf[i] < 0x7F ? (char)buf[i] : '.';
        gfx_text(SCREEN_BOTTOM, 244 + k * 9, y, on ? accent() : DIM, "%c", ch);
      }
    }
    if (!there)
      gfx_print(SCREEN_BOTTOM, 76, 100, FONT_BOLD, BAD, "Not mapped");
    frame();
    uint32_t d = pad();
    if (d & KEY_B) return;
    if ((d & KEY_START) || hotkey_again(d)) {
      ov.hex_va = 0xFFFFFFFFu;
      return;
    }
    if (d & KEY_RIGHT) cur++;
    if (d & KEY_LEFT) cur--;
    if (d & KEY_DOWN) cur += 8;
    if (d & KEY_UP) cur -= 8;
    if (d & KEY_R) ov.hex_va = base + 128u;
    if (d & KEY_L) ov.hex_va = base - 128u;
    if (cur >= 128) ov.hex_va = base + 128u, cur -= 128;
    if (cur < 0) ov.hex_va = base - 128u, cur += 128;
    if (d & KEY_Y) {
      uint32_t to = base + (uint32_t)cur;
      if (edit_number("Go to address", &to, MT_U32, true)) {
        ov.hex_va = to;
        cur = (int)(to & 0x7Fu);
      }
    }
    if ((d & KEY_A) && there) {
      uint32_t v = buf[cur];
      char t[48];
      snprintf(t, sizeof t, "Byte at %08lX", (unsigned long)(base + (uint32_t)cur));
      if (edit_number(t, &v, MT_U8, false)) {
        uint8_t b = (uint8_t)v;
        if (!mem_write(base + (uint32_t)cur, &b, 1))
          snprintf(toast, sizeof toast, "That byte could not be written.");
      }
    }
    if ((d & KEY_X) && there) {
      uint32_t at = (base + (uint32_t)cur) & ~3u, v;
      memcpy(&v, &buf[at - base], 4);
      if (freeze(at, MT_U32, v))
        snprintf(toast, sizeof toast, "%08lX frozen at 0x%08lX.", (unsigned long)at, (unsigned long)v);
    }
  }
}

static void cheat_label(int i, char *buf, size_t cap, uint32_t *color) {
  const Cheat *c = &ov.cheats.c[i];
  bool failed = false;
  if (c->enabled)
    for (int k = 0; k < MAX_PROG && k < 32; k++)
      if (ov.prog_index[k] == i && (job->cheat_failed >> k & 1u))
        failed = true;
  snprintf(buf, cap, "%s %.40s%s", c->enabled ? "[ON]" : "[  ]", c->name, failed ? "  !" : "");
  *color = failed ? BAD : c->enabled ? GOOD : COLOR_WHITE;
}

static int cheats_screen(void) {
  static int sel;
  if (!ov.cheats.n) {
    char text[400];
    snprintf(text, sizeof text,
             "No cheats for this title. File: SD:%s (text format: a "
             "[name] line, then code lines), or make some with Search memory.",
             ov.cheat_path);
    message("Cheats", text);
    return 0;
  }
  for (;;) {
    uint32_t key;
    int i = list("Cheats", "A: on/off   X: note   B: back", ov.cheats.n, &sel, cheat_label, &key);
    if (i < 0) return i;
    if (key == KEY_A) {
      ov.cheats.c[i].enabled = !ov.cheats.c[i].enabled;
      push_cheats();
      save_state();
    } else if (key == KEY_X) {
      message(ov.cheats.c[i].name, ov.cheats.c[i].note[0] ? ov.cheats.c[i].note : "(this cheat has no note)");
    }
  }
}

static uint32_t *values;
static int values_first = -1;

static void result_label(int i, char *buf, size_t cap, uint32_t *color) {
  int first = i - i % LIST_ROWS;
  if (!values && !(values = ARM9_MALLOC(LIST_ROWS * 8u))) {
    snprintf(buf, cap, "(out of memory)");
    return;
  }
  if (values_first != first) {
    int n = (int)ov.count - first < LIST_ROWS ? (int)ov.count - first : LIST_ROWS;
    memset(&job->mem, 0, sizeof job->mem);
    job->mem.op = MEM_VALUES;
    job->mem.type = ov.s_type;
    job->mem.hits = ARM9_PA(ov.hits);
    job->mem.cap = HIT_CAP;
    job->mem.first = (uint32_t)first;
    job->mem.n = (uint32_t)n;
    job->mem.buf = ARM9_PA(values);
    if (!mem_op()) memset(values, 0, LIST_ROWS * 8u);
    values_first = first;
  }
  uint32_t v = values[(i - first) * 2], ok = values[(i - first) * 2 + 1];
  char now[40];
  format_value(now, sizeof now, ov.s_type, v);
  snprintf(buf, cap, "%08lX  %s", (unsigned long)ov.hits[i].va, ok ? now : "(gone)");
  *color = ok ? COLOR_WHITE : DIM;
}

static int results_screen(void) {
  static int sel;
  for (;;) {
    values_first = -1;
    uint32_t key;
    char title[48];
    snprintf(title, sizeof title, "Results: %lu%s", (unsigned long)ov.count, ov.overflow ? "+" : "");
    int i = list(title, "A: edit  X: freeze  Y: more  B: back", (int)ov.count, &sel, result_label, &key);
    if (i < 0) return i;
    uint32_t va = ov.hits[i].va, v = values[(i % LIST_ROWS) * 2];
    if (key == KEY_A) {
      char t[48];
      snprintf(t, sizeof t, "%s at %08lX", type_names[ov.s_type], (unsigned long)va);
      if (edit_number(t, &v, ov.s_type, false) && !mem_write(va, &v, width_of(ov.s_type)))
        snprintf(toast, sizeof toast, "That address could not be written.");
    } else if (key == KEY_X) {
      if (freeze(va, ov.s_type, v)) snprintf(toast, sizeof toast, "%08lX frozen.", (unsigned long)va);
    } else {
      static const char *const acts[] = {"Freeze at a new value", "Save as a cheat", "Open in the hex editor"};
      static int asel;
      uint32_t k2;
      for (;;) {
        header("This result", "A: choose   B: back");
        for (int r = 0; r < 3; r++) {
          int y = LIST_Y + r * LIST_H;
          if (r == asel) gfx_round_rect(SCREEN_BOTTOM, 4, y, 312, LIST_H - 2, 5, accent());
          gfx_print(SCREEN_BOTTOM, 10, y + 2, FONT_REGULAR, r == asel ? COLOR_BLACK : COLOR_WHITE, "%s", acts[r]);
        }
        frame();
        k2 = pad();
        if (k2 & KEY_DOWN) asel = (asel + 1) % 3;
        if (k2 & KEY_UP) asel = (asel + 2) % 3;
        if (k2 & (KEY_A | KEY_B)) break;
      }
      if (k2 & KEY_B) continue;
      if (asel == 0) {
        if (edit_number("Freeze at", &v, ov.s_type, false) && freeze(va, ov.s_type, v))
          snprintf(toast, sizeof toast, "%08lX frozen.", (unsigned long)va);
      } else if (asel == 1) {
        if (edit_number("The cheat writes", &v, ov.s_type, false)) save_cheat(va, ov.s_type, v);
      } else {
        hex_editor(va);
        if (ov.hex_va == 0xFFFFFFFFu) return -2;
      }
    }
  }
}

static void do_search(bool first) {
  if (!ov.hits && !(ov.hits = ARM9_MALLOC(HIT_CAP * sizeof(JobHit)))) {
    snprintf(toast, sizeof toast, "Not enough memory for the results.");
    return;
  }
  header(first ? "Searching..." : "Refining...", NULL);
  gfx_print(SCREEN_BOTTOM, 10, 40, FONT_REGULAR, COLOR_WHITE, "Looking through the program's memory.");
  frame();
  memset(&job->mem, 0, sizeof job->mem);
  job->mem.op = first ? MEM_SEARCH : MEM_REFINE;
  job->mem.type = ov.s_type;
  job->mem.cmp = ov.s_cmp;
  job->mem.value = ov.s_value;
  job->mem.flags = ov.s_flags;
  job->mem.hits = ARM9_PA(ov.hits);
  job->mem.cap = HIT_CAP;
  job->mem.count = first ? 0 : ov.count;
  uint32_t total = 0;
  for (int bar = 0;; bar++) {
    if (!mem_op()) {
      snprintf(toast, sizeof toast, "The search did not run.");
      return;
    }
    if (first ? !job->mem.va : !job->mem.first)
      break;
    total = job->mem.count;
    header("Searching...", NULL);
    gfx_print(SCREEN_BOTTOM, 10, 40, FONT_REGULAR, COLOR_WHITE, "At %08lX: %lu found so far.",
              (unsigned long)job->mem.va, (unsigned long)total);
    gfx_rect(SCREEN_BOTTOM, 10, 70, 300, 6, PANEL);
    gfx_rect(SCREEN_BOTTOM, 10, 70, (bar * 12) % 300 + 6, 6, accent());
    frame();
    if(hid_keys_down()&KEY_B) { ov.count=0; ov.searched=false; snprintf(toast,sizeof toast,"Search cancelled."); return; }
  }
  ov.count = job->mem.count < HIT_CAP ? job->mem.count : HIT_CAP;
  ov.overflow = job->mem.count > HIT_CAP;
  ov.searched = true;
  if (ov.overflow)
    snprintf(toast, sizeof toast, "Over %lu: the first are kept. Refine to narrow it.", (unsigned long)HIT_CAP);
  else
    snprintf(toast, sizeof toast, "%lu found.", (unsigned long)ov.count);
}

static void search_label(int i, char *buf, size_t cap, uint32_t *color) {
  char v[40];
  switch (i) {
  case 0: snprintf(buf, cap, "Type: %s", type_names[ov.s_type]); break;
  case 1: snprintf(buf, cap, "Compare: %s", cmp_names[ov.s_cmp]); break;
  case 2:
    format_value(v, sizeof v, ov.s_type, ov.s_value);
    snprintf(buf, cap, "Value: %s", v);
    if (ov.s_cmp >= CMP_CHANGED && ov.s_cmp <= CMP_DEC) *color = DIM;
    break;
  case 3: snprintf(buf, cap, "New search"); break;
  case 4:
    snprintf(buf, cap, "Next search");
    if (!ov.searched) *color = DIM;
    break;
  case 5:
    snprintf(buf, cap, "Results (%lu%s)", (unsigned long)ov.count, ov.overflow ? "+" : "");
    if (!ov.count) *color = DIM;
    break;
  default: snprintf(buf, cap, "Code and rodata too: %s", ov.s_flags ? "yes" : "no"); break;
  }
}

static int search_screen(void) {
  static int sel;
  for (;;) {
    uint32_t key;
    int i = list("Search memory", "A: choose / change   B: back", 7, &sel, search_label, &key);
    if (i < 0) return i;
    switch (i) {
    case 0:
      if (ov.searched && ov.count) {
        snprintf(toast, sizeof toast, "Start a new search to change the type.");
        ov.searched = false, ov.count = 0;
      }
      ov.s_type = (ov.s_type + 1) % 4;
      break;
    case 1:
      ov.s_cmp = (ov.s_cmp + 1) % (ov.searched ? 10u : 4u);
      break;
    case 2: {
      uint32_t v = ov.s_value;
      if (edit_number("Value", &v, ov.s_type, false)) ov.s_value = v;
      break;
    }
    case 3:
      if (ov.s_cmp >= CMP_CHANGED) ov.s_cmp = CMP_EQ;
      do_search(true);
      break;
    case 4:
      if (ov.searched) do_search(false);
      break;
    case 5:
      if (ov.count && results_screen() == -2) return -2;
      break;
    default: ov.s_flags ^= 1u; break;
    }
  }
}

static void freeze_label(int i, char *buf, size_t cap, uint32_t *color) {
  char v[40];
  format_value(v, sizeof v, ov.freezes[i].type, ov.freezes[i].value);
  snprintf(buf, cap, "%08lX = %s (%s)", (unsigned long)ov.freezes[i].va, v, type_names[ov.freezes[i].type]);
  (void)color;
}

static int freezes_screen(void) {
  static int sel;
  for (;;) {
    uint32_t key;
    int i = list("Frozen values", "A: change   X: let go   B: back", ov.n_freezes, &sel, freeze_label, &key);
    if (i < 0) return i;
    if (key == KEY_X) {
      ov.freezes[i] = ov.freezes[--ov.n_freezes];
      push_cheats();
    } else if (key == KEY_A) {
      uint32_t v = ov.freezes[i].value;
      if (edit_number("Frozen at", &v, ov.freezes[i].type, false)) {
        ov.freezes[i].value = v;
        push_cheats();
      }
    }
  }
}

static JobRegionInfo regions[JOB_MEM_REGIONS];
static int n_regions;

static void region_label(int i, char *buf, size_t cap, uint32_t *color) {
  snprintf(buf, cap, "%-12s %08lX  %lu KB", regions[i].name, (unsigned long)regions[i].va,
           (unsigned long)(regions[i].size >> 10));
  (void)color;
}

static int map_screen(void) {
  static int sel;
  memset(&job->mem, 0, sizeof job->mem);
  job->mem.op = MEM_REGIONS;
  if (!mem_op()) return 0;
  n_regions = (int)job->mem.n_regions;
  memcpy(regions, job->mem.regions, sizeof regions);
  for (;;) {
    uint32_t key;
    int i = list("Memory map", "A: open in the hex editor   B: back", n_regions, &sel, region_label, &key);
    if (i < 0) return i;
    hex_editor(regions[i].va);
    if (ov.hex_va == 0xFFFFFFFFu) return -2;
  }
}

static void main_label(int i, char *buf, size_t cap, uint32_t *color) {
  int on = 0;
  for (int k = 0; k < ov.cheats.n; k++) on += ov.cheats.c[k].enabled;
  switch (i) {
  case 0: snprintf(buf, cap, "Cheats (%d on of %d)", on, ov.cheats.n); break;
  case 1: snprintf(buf, cap, "Search memory"); break;
  case 2:
    snprintf(buf, cap, "Results (%lu)", (unsigned long)ov.count);
    if (!ov.count) *color = DIM;
    break;
  case 3: snprintf(buf, cap, "Hex editor"); break;
  case 4: snprintf(buf, cap, "Memory map"); break;
  case 5: snprintf(buf, cap, "Frozen values (%d)", ov.n_freezes); break;
  case 6: {
    const char *name = "SELECT";
    for (int k = 0; k < N_HOTKEYS; k++)
      if (hotkeys[k].keys == hotkey) name = hotkeys[k].name;
    snprintf(buf, cap, "Hotkey: %s", name);
    break;
  }
  case 7: snprintf(buf, cap, "Cheats and freezes: %s", job->cheats_on ? "running" : "stopped"); break;
  default: snprintf(buf, cap, "Resume"); break;
  }
}

void overlay_open(void) {
  if (!ov.active) return;
  toast[0]=0;
  top_banner();
  static int sel;
  for (bool open = true; open;) {
    uint32_t key;
    int i = list("3DSX Runner: cheats and tools", "A: choose   B or START: resume", 9, &sel, main_label, &key);
    int r = 0;
    switch (i) {
    case -1:
    case -2:
    case 8: open = false; break;
    case 0: r = cheats_screen(); break;
    case 1: r = search_screen(); break;
    case 2:
      if (ov.count) r = results_screen();
      break;
    case 3:
      hex_editor(ov.hex_va && ov.hex_va != 0xFFFFFFFFu ? ov.hex_va : 0x00100000u);
      if (ov.hex_va == 0xFFFFFFFFu) r = -2;
      break;
    case 4: r = map_screen(); break;
    case 5: r = freezes_screen(); break;
    case 6: {
      int k = 0;
      for (int j = 0; j < N_HOTKEYS; j++)
        if (hotkeys[j].keys == hotkey) k = j;
      hotkey = hotkeys[(k + 1) % N_HOTKEYS].keys;
      settings_save();
      break;
    }
    case 7: job->cheats_on = !job->cheats_on; break;
    }
    if (r == -2) open = false;
  }
  toast[0] = 0;
  masked = hid_keys_held();
  os_cache_sync();
}
