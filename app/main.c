#include <aurora_app.h>

#include "audio.h"

#include "fsbridge.h"
#include "httpbridge.h"
#include "glue.h"
#include "job.h"
#include "corelink.h"
#include "pacing.h"
#include "menu.h"
#include "splash.h"
#include "apps.h"
#include "ui.h"
#include "ui_audio.h"
#include "overlay.h"
#include "titlecfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void os_cache_sync(void);
extern const uint8_t sup_blob[], sup_blob_end[];

#define TOP_BYTES      (400u * 240u * 3u)
#define BOTTOM_BYTES   (320u * 240u * 3u)
#define CLKCNT_MODE_804 5u
#define FILE_PA_LIMIT  (12u << 20)

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;
static JobParams *const job = (JobParams *)JOB_PARAMS;
static bool n3ds, clock_changed, guest_loaded, supervisor_started, leaving, direct_launch;
static uint32_t clock_before;
static void *display_allocation;
static bool snapshot_requested, snapshot_chord, plugin_request;
static char active_path[256];
extern bool app_home_pressed(void);
static uint64_t link_micros(void) { return (uint64_t)sys_micros(); }
static CoreLink core = {
  .ctrl=(AudioCtrl *)AUDIO_CTRL_ADDR, .job=(JobParams *)JOB_PARAMS,
  .cancel=(volatile uint32_t *)JOB_CANCEL_ADDR,
  .sync=os_cache_sync, .millis=sys_millis, .home=app_home_pressed,
  .micros=link_micros
};

static void write_crash(FILE *f) {
  if (job->yield!=YIELD_CRASH) return;
  const JobCrash *c=&job->crash;
  fprintf(f,"crash: kind=%lu supervisor=%lu thread=%lu PC=%08lX LR=%08lX SP=%08lX CPSR=%08lX FAR=%08lX FSR=%08lX\n",
          (unsigned long)c->kind,(unsigned long)c->in_sup,(unsigned long)c->thread,
          (unsigned long)c->r[15],(unsigned long)c->r[14],(unsigned long)c->r[13],
          (unsigned long)c->cpsr,(unsigned long)c->far,(unsigned long)c->fsr);
  for (unsigned i=0;i<13;i++) fprintf(f,"r%u=%08lX%c",i,(unsigned long)c->r[i],i%4==3?'\n':' ');
  fputc('\n',f);
}

static void exit_trace(const char *stage, bool fresh) {
  char path[300];
  snprintf(path,sizeof path,"%s/last-exit.txt",app_dir());
  FILE *f=fopen(path,fresh?"w":"a");
  if (!f) return;
  bool idle=corelink_idle(&core);
  fprintf(f,"r14b %lu ms: %s; HOME=%u seq=%lu ack=%lu idle=%u\n",
          (unsigned long)sys_millis(),stage,core.home_pending,
          (unsigned long)ctrl->cmd_seq,(unsigned long)ctrl->ack_seq,idle);
  if (fresh) fprintf(f,"%s\n",active_path);
  if (idle && supervisor_started) {
    fprintf(f,"cmd=%lu done=%lu yield=%lu gx=%lu LCD-pending=%lu error=%.127s\n",
            (unsigned long)job->cmd,(unsigned long)job->done,
            (unsigned long)job->yield,(unsigned long)job->gx_count,(unsigned long)job->display_pending,job->error);
    fprintf(f,"launch=%s; display=%08lX size=%lX; swaps=%lu/%lu\n",
            job->launch_flags&1u?"X/no-arguments":"A/path-argument",
            (unsigned long)job->display.pa,(unsigned long)job->display.size,
            (unsigned long)job->top.serial,(unsigned long)job->bottom.serial);
    if (!strcmp(stage,"stop.begin") || !strcmp(stage,"stop.returned")) {
      write_crash(f);
      uint32_t head=job->log_head, n=head<JOB_LOG_SIZE?head:JOB_LOG_SIZE;
      for(uint32_t i=head-n;i!=head;i++) fputc(job->log[i%JOB_LOG_SIZE],f);
      fputc('\n',f);
    }
  }
  fclose(f);
}

/* Never let SDK cleanup scribble over the mailbox while the ARM11 might still
 * be using it. If it answers late, great. If not, we wait. */
static void await_safe_core(void) {
  if (corelink_idle(&core)) return;
  exit_trace("core.unresponsive: waiting; no commands overwritten",false);
  while (!corelink_wait(&core,500)) {  }
  exit_trace("core.returned-late",false);
}

static bool core_wait(uint32_t ms) {
  bool ok=corelink_wait(&core,ms);
  if (!ok) await_safe_core();
  return ok;
}

static bool core_post(uint32_t cmd, uint32_t a0, uint32_t a1) {
  await_safe_core();
  return corelink_post(&core,cmd,a0,a1);
}

static bool run(uint32_t cmd) {
  bool ok=corelink_run(&core,cmd);
  await_safe_core();
  if (core.home_pending && !leaving) exit(0);
  return ok;
}

bool runner_job(uint32_t cmd) { return run(cmd); }
bool runner_overlay_frame(void) {
  os_cache_sync();
  if(!run(JOB_OVERLAY_FRAME) || job->yield==YIELD_ERROR || job->yield==YIELD_CRASH) return false;
  hid_scan();
  return true;
}
static bool show_overlay(void) {
  if(!run(JOB_OVERLAY_BEGIN)) return false;
  glue_overlay_begin(job->top.pa,job->bottom.pa);
  overlay_open();
  return run(JOB_OVERLAY_END) && job->yield!=YIELD_ERROR && job->yield!=YIELD_CRASH;
}

static void clock_fast(void) {
  if (!n3ds || clock_changed) return;
  await_safe_core();
  ctrl->n3ds_status = AUDIO_N3DS_PENDING;
  if (!core_post(AUDIO_CMD_N3DS, CLKCNT_MODE_804, 0)) return;
  core_wait(3000);
  if (ctrl->n3ds_status == AUDIO_N3DS_APPLIED) {
    clock_before = (ctrl->n3ds_before >> 16) & 7u;
    clock_changed = true;
  }
  if (core.home_pending && !leaving) exit(0);
}

static void clock_restore(void) {
  if (clock_changed && core_post(AUDIO_CMD_N3DS, clock_before, 0)) core_wait(3000);
  clock_changed = false;
  if (core.home_pending && !leaving) exit(0);
}

static void stop_guest(void) {
  if (!supervisor_started) return;
  exit_trace("stop.begin",false);
  run(JOB_STOP);
  if (job->display_pending) {
    exit_trace("display.pending: retaining guest RAM and clock",false);

    do { run(JOB_STOP); } while (job->display_pending);
  }
  exit_trace("stop.returned",false);
  if (display_allocation)
    memset((void *)(uintptr_t)job->display.pa,0,DISPLAY_POOL_SIZE);
  overlay_game_end();
  glue_clear_display();
  exit_trace("display.cleared",false);
  if (guest_loaded) glue_guest_display(false);
  guest_loaded=false;
  supervisor_started=false;
  free(display_allocation);
  display_allocation=NULL;
}

static void leave(void) {
  leaving=true;
  core.loading_tick=NULL;
  exit_trace("leave.begin",false);
  await_safe_core();
  stop_guest();
  splash_end(false);
  ui_audio_suspend();
  await_safe_core();
  fsbridge_close_all();
  httpbridge_drop(0xFFFFFFFFu);
  exit_trace("files.closed",false);
  clock_restore();
  glue_clear_display();
  exit_trace("leave.ready: handing control to Aurora",false);
}

typedef struct {
  char path[200];
  char title[64];
  char author[64];
  uint32_t size;
  uint16_t *icon;
} Entry;

static Entry *entries;
static int n_entries, cap_entries, sel, scroll;

static void utf16_to_utf8(const uint16_t *in, int n, char *out, int cap) {
  int k = 0;
  for (int i = 0; i < n && in[i] && k + 4 < cap; i++) {
    uint32_t c = in[i];
    if (c < 0x80)
      out[k++] = (char)c;
    else if (c < 0x800)
      out[k++] = (char)(0xC0 | c >> 6), out[k++] = (char)(0x80 | (c & 63));
    else
      out[k++] = (char)(0xE0 | c >> 12), out[k++] = (char)(0x80 | ((c >> 6) & 63)),
      out[k++] = (char)(0x80 | (c & 63));
  }
  out[k] = 0;
  for (int i = 0; i < k; i++)
    if (out[i] == '\n')
      out[i] = ' ';
}

static void add_entry(const char *path, uint32_t size) {
  if(strlen(path)>=sizeof(((Entry *)0)->path)) return;
  FILE *f = fopen(path, "rb");
  if (!f)
    return;
  uint8_t hdr[0x2C];
  size_t got = fread(hdr, 1, sizeof hdr, f);
  if (got < 0x20 || memcmp(hdr, "3DSX", 4)) {
    fclose(f);
    return;
  }
  if (n_entries == cap_entries) {
    int new_cap = cap_entries ? cap_entries * 2 : 32;
    Entry *grown = realloc(entries, (size_t)new_cap * sizeof *entries);
    if (!grown) {
      fclose(f);
      return;
    }
    entries = grown; cap_entries=new_cap;
  }
  Entry *e = &entries[n_entries];
  memset(e, 0, sizeof *e);
  snprintf(e->path, sizeof e->path, "%s", path);
  e->size = size;
  const char *base = strrchr(path, '/');
  snprintf(e->title, sizeof e->title, "%s", base ? base + 1 : path);
  char *dot = strrchr(e->title, '.');
  if (dot)
    *dot = 0;
  uint16_t header_size = (uint16_t)(hdr[4] | hdr[5] << 8);
  if (got >= 0x2C && header_size > 0x20) {
    uint32_t smdh = hdr[0x20] | hdr[0x21] << 8 | hdr[0x22] << 16 | (uint32_t)hdr[0x23] << 24;
    uint32_t smdh_size = hdr[0x24] | hdr[0x25] << 8 | hdr[0x26] << 16 | (uint32_t)hdr[0x27] << 24;
    static uint8_t s[0x36C0];
    if (smdh && smdh_size >= sizeof s && !fseek(f, (long)smdh, SEEK_SET) && fread(s, 1, sizeof s, f) == sizeof s &&
        !memcmp(s, "SMDH", 4)) {
      const uint8_t *t = s + 8 + 1 * 0x200;
      utf16_to_utf8((const uint16_t *)(const void *)t, 0x40, e->title, sizeof e->title);
      utf16_to_utf8((const uint16_t *)(const void *)(t + 0x180), 0x40, e->author, sizeof e->author);
      if (!e->title[0])
        snprintf(e->title, sizeof e->title, "%s", base ? base + 1 : path);
      e->icon = malloc(48 * 48 * 2);
      if (e->icon)
        memcpy(e->icon, s + 0x24C0, 48 * 48 * 2);
    }
  }
  fclose(f);
  n_entries++;
}

static bool ends_3dsx(const char *name) {
  size_t n = strlen(name);
  return n > 5 && !strcasecmp(name + n - 5, ".3dsx");
}

static int by_title(const void *a, const void *b) {
  const Entry *x = a, *y = b;
  return strcasecmp(x->title, y->title);
}

static void scan(void) {
  for (int i=0;i<n_entries;i++) free(entries[i].icon);
  free(entries); entries=NULL; n_entries=cap_entries=sel=scroll=0;
  FsDir *d = fs_dir_open("/3ds");
  if (!d)
    return;
  FsEntry e;
  char path[200];
  char (*subdirs)[96] = malloc(128 * 96);
  int n_sub = 0;
  while (fs_dir_read(d, &e)) {
    if (!e.is_dir && ends_3dsx(e.name)) {
      if(snprintf(path, sizeof path, "/3ds/%s", e.name)<(int)sizeof path)
        add_entry(path, e.size);
    } else if (e.is_dir && subdirs && n_sub < 128 && strlen(e.name) < 96) {
      snprintf(subdirs[n_sub++], 96, "%s", e.name);
    }
  }
  fs_dir_close(d);
  for (int i = 0; i < n_sub; i++) {
    snprintf(path, sizeof path, "/3ds/%s", subdirs[i]);
    FsDir *s = fs_dir_open(path);
    if (!s)
      continue;
    while (fs_dir_read(s, &e))
      if (!e.is_dir && ends_3dsx(e.name)) {
        char full[200];
        if(snprintf(full, sizeof full, "/3ds/%s/%s", subdirs[i], e.name)<(int)sizeof full)
          add_entry(full, e.size);
      }
    fs_dir_close(s);
  }
  free(subdirs);
  AppCatalog installed={0};
  apps_scan(&installed,"/Aurora/Apps");
  for (int i=0;i<n_entries;i++) for (size_t j=0;j<installed.count;j++) {
    const InstalledApp *app=&installed.items[j];
    if (app->linked && app->info.title[0] && !strcmp(app->info.target,entries[i].path)) {
      snprintf(entries[i].title,sizeof entries[i].title,"%s",app->info.title);
      snprintf(entries[i].author,sizeof entries[i].author,"%s",app->info.author);
      break;
    }
  }
  apps_free(&installed);
  if (n_entries) qsort(entries, (size_t)n_entries, sizeof *entries, by_title);
}

#define BG     RGB(16, 20, 30)
#define PANEL  RGB(28, 34, 48)
#define DIM    RGB(150, 160, 180)
#define ROW_H  26
#define ROWS   7

static uint32_t accent(void) {
  uint32_t a = sys_accent();
  return a ? a : COLOR_AURORA;
}

static void draw_icon(const uint16_t *icon, int x0, int y0, int scale) {
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 48; x++) {
      int tile = (y / 8) * 6 + x / 8;
      int m = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
      uint16_t p = icon[tile * 64 + m];
      uint32_t r = (p >> 11) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
      gfx_rect(SCREEN_TOP, x0 + x * scale, y0 + y * scale, scale, scale, RGB(r, g, b));
    }
}

static void draw_top(const char *note) {
  gfx_rect(SCREEN_TOP, 0, 0, 400, 240, BG);
  gfx_print(SCREEN_TOP, 16, 12, FONT_TITLE, COLOR_WHITE, "3DSX Runner r14b");
  gfx_print(SCREEN_TOP, 18 + gfx_text_width(FONT_TITLE, "3DSX Runner r14b"), 18, FONT_SMALL, DIM,
            "libctru homebrew on AuroraOS");
  if (!n_entries) {
    gfx_print(SCREEN_TOP, 16, 70, FONT_REGULAR, COLOR_WHITE,
              "No .3dsx programs were found.\n\nPut them in SD:/3ds, or in their own folders\ninside it, as the Homebrew Launcher keeps them.");
  } else {
    const Entry *e = &entries[sel];
    gfx_round_rect(SCREEN_TOP, 12, 48, 376, 120, 10, PANEL);
    if (e->icon)
      draw_icon(e->icon, 24, 60, 2);
    else
      gfx_round_rect(SCREEN_TOP, 24, 60, 96, 96, 12, RGB(60, 70, 90));
    gfx_print(SCREEN_TOP, 136, 64, FONT_BOLD, COLOR_WHITE, "%.40s", e->title);
    if (e->author[0])
      gfx_print(SCREEN_TOP, 136, 86, FONT_REGULAR, DIM, "%.40s", e->author);
    gfx_print(SCREEN_TOP, 136, 120, FONT_SMALL, DIM, "SD:%.44s", e->path);
    gfx_print(SCREEN_TOP, 136, 136, FONT_SMALL, DIM, "%u KB", (unsigned)(e->size >> 10));
  }
  if (note && note[0])
    gfx_print(SCREEN_TOP, 16, 178, FONT_SMALL, COLOR_YELLOW, "%s", note);
  gfx_print(SCREEN_TOP,16,210,FONT_SMALL,DIM,"A: start   X: no arguments   Y: add to menu   START: apps");
  gfx_print(SCREEN_TOP,16,226,FONT_SMALL,DIM,"L + launch: cheats   SELECT: sound %s   HOME: exit",ui_audio_enabled()?"on":"off");
}

static void draw_bottom(void) {
  gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 240, BG);
  if (sel < scroll)
    scroll = sel;
  if (sel >= scroll + ROWS)
    scroll = sel - ROWS + 1;
  for (int i = 0; i < ROWS && scroll + i < n_entries; i++) {
    int k = scroll + i, y = 8 + i * (ROW_H + 2);
    if (k == sel)
      gfx_round_rect(SCREEN_BOTTOM, 6, y, 308, ROW_H, 6, accent());
    gfx_print(SCREEN_BOTTOM, 14, y + 5, FONT_REGULAR, k == sel ? COLOR_BLACK : COLOR_WHITE, "%.36s",
              entries[k].title);
  }
  gfx_round_rect(SCREEN_BOTTOM,6,208,150,28,6,PANEL);
  gfx_round_rect(SCREEN_BOTTOM,164,208,150,28,6,PANEL);
  gfx_print(SCREEN_BOTTOM,16,216,FONT_SMALL,COLOR_WHITE,"START: Manage apps");
  gfx_print(SCREEN_BOTTOM,174,216,FONT_SMALL,COLOR_WHITE,"SELECT: Sound %s",ui_audio_enabled()?"on":"off");
}

static void save_log(const Entry *e, const char *outcome) {
  char path[256];
  snprintf(path, sizeof path, "%s/last-run.txt", app_dir());
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "3DSX Runner r14b: %s\n%s\nSD:%s\n\n", outcome, e->title, e->path);
  fprintf(f, "kernel calls %u, service requests %u, GPU commands %u, time slices run out %u\n\n",
          (unsigned)job->svc_count, (unsigned)job->ipc_count, (unsigned)job->gx_count, (unsigned)job->preempts);
  fprintf(f,"launch=%s; display=%08lX size=%lX; swaps=%lu/%lu\n",
          job->launch_flags&1u?"X/no-arguments":"A/path-argument",
          (unsigned long)job->display.pa,(unsigned long)job->display.size,
          (unsigned long)job->top.serial,(unsigned long)job->bottom.serial);
  write_crash(f);
  uint32_t head = job->log_head, n = head < JOB_LOG_SIZE ? head : JOB_LOG_SIZE;
  for (uint32_t i = head - n; i != head; i++)
    fputc(job->log[i % JOB_LOG_SIZE], f);
  fclose(f);
}

static void show_log(void) {
  gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 240, BG);
  static char text[JOB_LOG_SIZE + 1];
  uint32_t head = job->log_head, n = head < JOB_LOG_SIZE ? head : JOB_LOG_SIZE;
  for (uint32_t i = 0; i < n; i++)
    text[i] = job->log[(head - n + i) % JOB_LOG_SIZE];
  text[n] = 0;
  int lines = 0;
  char *start = text + n;
  while (start > text && lines < 27)
    if (*--start == '\n')
      lines++;
  if (*start == '\n')
    start++;
  int y = 4;
  for (char *line = start; *line && y < 236; y += 9) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = 0;
    gfx_text(SCREEN_BOTTOM, 2, y, DIM, "%.52s", line);
    if (!nl)
      break;
    line = nl + 1;
  }
}

static void report(const Entry *e, char *note, size_t cap) {
  static const char *kinds[] = {"a data abort", "a prefetch abort", "an undefined instruction", "svcBreak",
                                "a bad kernel call", "an unexpected exception"};
  switch (job->yield) {
  case YIELD_EXIT:
    if (direct_launch) note[0] = 0;
    else snprintf(note, cap, "%s ended.", e->title);
    save_log(e, "the program ended");
    return;
  case YIELD_CRASH: {
    JobCrash *c = &job->crash;
    save_log(e, "the program crashed");
    gfx_rect(SCREEN_TOP, 0, 0, 400, 240, RGB(40, 16, 20));
    gfx_print(SCREEN_TOP, 16, 12, FONT_TITLE, COLOR_WHITE, "%.32s stopped", e->title);
    gfx_print(SCREEN_TOP, 16, 44, FONT_REGULAR, COLOR_WHITE, "It ran into %s%s (thread %u).",
              c->kind < 6 ? kinds[c->kind] : "an exception", c->in_sup ? " in the runner itself" : "",
              (unsigned)c->thread);
    static const char *names[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                    "r8", "r13", "r13", "r13", "r13", "sp", "lr", "pc"};
    for (int i = 0; i < 16; i++)
      gfx_print(SCREEN_TOP, 8 + (i % 4) * 98, 76 + (i / 4) * 12, FONT_SMALL, COLOR_WHITE, "%s %08X", names[i],
               (unsigned)c->r[i]);
    gfx_text(SCREEN_TOP, 16, 130, COLOR_WHITE, "cpsr %08X  address %08X  status %08X", (unsigned)c->cpsr,
             (unsigned)c->far, (unsigned)c->fsr);
    gfx_print(SCREEN_TOP, 16, 156, FONT_SMALL, DIM,
              "The supervisor's log is on the bottom screen and in\n%s/last-run.txt.", app_dir());
    gfx_print(SCREEN_TOP, 16, 218, FONT_SMALL, DIM, "A: %s", direct_launch ? "back to Aurora" : "back to the list");
    show_log();
    hid_wait(KEY_A | KEY_B);
    note[0] = 0;
    return;
  }
  default:
    save_log(e, "the runner stopped it");
    gfx_rect(SCREEN_TOP, 0, 0, 400, 240, RGB(40, 30, 16));
    gfx_print(SCREEN_TOP, 16, 12, FONT_TITLE, COLOR_WHITE, "%.32s could not run", e->title);
    gfx_print(SCREEN_TOP, 16, 48, FONT_REGULAR, COLOR_WHITE, "%.120s", job->error[0] ? job->error : "(no reason given)");
    gfx_print(SCREEN_TOP, 16, 218, FONT_SMALL, DIM, "A: %s", direct_launch ? "back to Aurora" : "back to the list");
    show_log();
    hid_wait(KEY_A | KEY_B);
    note[0] = 0;
  }
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool loading_progress(size_t done, size_t total) {
  if (app_home_pressed()) core.home_pending=true;
  if (core.home_pending) return false;
  ui_audio_tick();
  splash_progress(done,total);
  return true;
}

static uint8_t *read_program(const char *path, uint32_t *bytes, char *why, size_t cap) {
  FILE *f = fopen(path, "rb");
  uint8_t h[0x100];
  if (!f || fread(h, 1, 0x20, f) != 0x20 || memcmp(h, "3DSX", 4)) {
    snprintf(why, cap, "The file could not be read as a .3dsx.");
    if (f)
      fclose(f);
    return NULL;
  }
  uint32_t hs = h[4] | h[5] << 8, rhs = h[6] | h[7] << 8;
  uint32_t code = le32(h + 0x10), ro = le32(h + 0x14), data = le32(h + 0x18), bss = le32(h + 0x1C);
  if (hs < 0x20 || rhs < 8 || rhs > 0x40 || (rhs & 3u) || data < bss || hs + 3 * rhs > sizeof h ||
      fseek(f, 0, SEEK_SET) || fread(h, 1, hs + 3 * rhs, f) != hs + 3 * rhs) {
    snprintf(why, cap, "The .3dsx header is damaged.");
    fclose(f);
    return NULL;
  }
  uint64_t need = (uint64_t)hs + 3u * rhs + code + ro + (data - bss);
  for (uint32_t i = 0; i < 3; i++)
    for (uint32_t j = 0; j < rhs / 4; j++)
      need += 4ull * le32(h + hs + i * rhs + j * 4);
  if (need > FILE_PA_LIMIT) {
    snprintf(why, cap, "The program is too big to load (%u KB).", (unsigned)(need >> 10));
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)need);
  bool ok=buf && !fseek(f,0,SEEK_SET);
  for (size_t done=0; ok && done<(size_t)need;) {
    if (!loading_progress(done,(size_t)need)) { ok=false; break; }
    size_t amount=(size_t)need-done;
    if (amount>16384) amount=16384;
    if (fread(buf+done,1,amount,f)!=amount) { ok=false; break; }
    done+=amount;
  }
  if (!ok) {
    snprintf(why, cap, buf ? "The file is shorter than its header says." : "Not enough memory to read it.");
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *bytes = (uint32_t)need;
  return buf;
}

static void input(void) {
  int x, y;
  hid_cpad(&x, &y);
  job->held = hid_keys_held();
  if(job->plugin_enabled) {
    if(overlay_hotkey(hid_keys_down(),job->held)) plugin_request=true;
    job->held &= ~overlay_mask(job->held);
  }
  const uint32_t chord=KEY_L|KEY_R|KEY_SELECT;
  bool pressed=(job->held&chord)==chord;
  if (pressed && !snapshot_chord) snapshot_requested=true;
  snapshot_chord=pressed;
  job->cpad_x = x * 156 / 127;
  job->cpad_y = y * 156 / 127;
  if (hid_touch_x() >= 0) {
    job->touch_x = (uint32_t)hid_touch_x();
    job->touch_y = (uint32_t)hid_touch_y();
  }
}

static bool save_frame(const char *name, uint32_t pa, unsigned width) {
  uint8_t header[54]={0}, row[400*3];
  uint32_t bytes=width*240u*3u, size=54u+bytes;
  if (pa<job->display.pa || (uint64_t)pa+bytes>(uint64_t)job->display.pa+job->display.size)
    return false;
  header[0]='B'; header[1]='M';
  for (unsigned i=0;i<4;i++) {
    header[2+i]=(uint8_t)(size>>(8*i));
    header[18+i]=(uint8_t)(width>>(8*i));
    header[34+i]=(uint8_t)(bytes>>(8*i));
  }
  header[10]=54; header[14]=40; header[22]=240; header[26]=1; header[28]=24;
  char path[320]; snprintf(path,sizeof path,"%s/%s",app_dir(),name);
  FILE *f=fopen(path,"wb");
  if (!f) return false;
  bool ok=fwrite(header,1,sizeof header,f)==sizeof header;
  const uint8_t *pixels=(const uint8_t *)(uintptr_t)pa;
  for (unsigned y=0;ok && y<240;y++) {
    for (unsigned x=0;x<width;x++) memcpy(row+x*3,pixels+(x*240+y)*3,3);
    ok=fwrite(row,3,width,f)==width;
  }
  if (fclose(f)) ok=false;
  return ok;
}

static bool present(void) {
  if (run(JOB_PRESENT)) {
    if (job->yield==YIELD_ERROR || job->yield==YIELD_EXIT || job->yield==YIELD_CRASH) return false;
    if (snapshot_requested) {
      snapshot_requested=false;
      bool top=save_frame("last-frame-top.bmp",job->top.pa,400);
      bool bottom=save_frame("last-frame-bottom.bmp",job->bottom.pa,320);
      exit_trace(top && bottom?"snapshot.saved":"snapshot.write-failed",false);
    }
    return true;
  }
  snprintf(job->error,sizeof job->error,"The ARM11 did not finish presenting the frame.");
  job->yield=YIELD_ERROR;
  return false;
}

static void start(const Entry *e, char *note, size_t cap, bool no_args, bool plugins, bool open_plugin) {
  ui_audio_suspend();
  await_safe_core();
  splash_begin(false);
  snapshot_requested=snapshot_chord=plugin_request=false;
  char why[160];
  uint32_t bytes = 0;
  uint8_t *file = read_program(e->path, &bytes, why, sizeof why);
  if (!file) {
    splash_end(false);
    if (core.home_pending) exit(0);
    snprintf(note, cap, "%s", why);
    return;
  }
  display_allocation=malloc(DISPLAY_POOL_SIZE+0xFFFu);
  if (!display_allocation) {
    free(file);
    splash_end(false);
    snprintf(note,cap,"Not enough memory for the display buffers.");
    return;
  }
  await_safe_core();
  snprintf(active_path,sizeof active_path,"%s",e->path);
  exit_trace("launch",true);
  /* Every launch copies a fresh supervisor into place. Menu tiles carry their
   * own copy of the whole runner, which is why they need Y / Add-update after
   * an update. */
  memcpy((void *)JOB_CODE, sup_blob, (size_t)(sup_blob_end - sup_blob));
  memset(job, 0, sizeof *job);
  job->magic = JOB_MAGIC;
  job->version = JOB_VERSION;
  job->file_pa = (uint32_t)file;
  job->file_size = bytes;
  job->display=(JobRegion){((uint32_t)display_allocation+0xFFFu)&~0xFFFu,DISPLAY_POOL_SIZE};
  job->launch_flags = no_args ? 1u : 0u;
  {
    char sync_path[300];
    /* The escape hatch: drop sync-gx.txt on the SD card to get the old
     * synchronous GPU behaviour back. Please tell me if you ever need it. */
    snprintf(sync_path,sizeof sync_path,"%s/sync-gx.txt",app_dir());
    FILE *sync=fopen(sync_path,"r");
    if (sync) { job->launch_flags |= 2u; fclose(sync); }
  }
  job->plugin_enabled=plugins;
  snprintf(job->path, sizeof job->path, "sdmc:%s", e->path);
  /* Where the game lives. New 3DS: upper FCRAM, which Aurora leaves alone.
   * Old 3DS: scratch below the OS's copy, plus the sound pool. Yes, the Old
   * 3DS layout is tight. Coastline Chase's 9.6 MB of meshes don't fit, and I'm
   * sorry. */
  if (n3ds) {
    job->code = (JobRegion){0x28000000u, 0x05000000u};
    job->linear = (JobRegion){0x2D000000u, 0x03000000u};
  } else {
    job->code = (JobRegion){0x25300000u, 0x00D00000u};
    job->linear = (JobRegion){0x23400000u, 0x00A00000u};
  }
  job->new3ds = n3ds;
  clock_fast();
  job->cpu_mhz = n3ds && ((ctrl->n3ds_after >> 16) & 7u) == CLKCNT_MODE_804 ? 804u : 268u;
  job->unix_ms = (uint64_t)time(NULL) * 1000u;
  if (!glue_font(&job->font_head_pa, &job->font_glyphs_pa, &job->font_atlas_pa))
    job->font_head_pa = job->font_glyphs_pa = job->font_atlas_pa = 0;

  splash_preparing();
  core.loading_tick=splash_tick;
  /* From here on, even HOME during loading needs a proper JOB_STOP. */
  supervisor_started=true;
  bool loaded = run(JOB_LOAD) && job->yield != YIELD_ERROR;
  core.loading_tick=NULL;
  free(file);
  if (!loaded) {
    if (!job->error[0])
      snprintf(job->error, sizeof job->error, "The ARM11 did not answer.");
    job->yield = YIELD_ERROR;
    stop_guest();
    splash_end(false);
    report(e, note, cap);
    clock_restore();
    return;
  }

  splash_end(true);
  guest_loaded = true;
  glue_guest_display(true);
  bool going=present();
  if(plugins && going) {
    overlay_game_start(e->title,e->path);
    if(open_plugin) going=show_overlay();
  }

  /* Guest vblanks, timer wakeups and actual presents each get their own
   * deadline. When they shared one, audio wakeups kept pushing the screen
   * copy back forever (Coastline's famous blue screen, r6). */
  uint64_t t0 = sys_micros();
  RunnerPacing pace;
  runner_pacing_init(&pace,t0);
  uint32_t last_scan = sys_millis();
  for (;going;) {
    hid_scan();
    last_scan = sys_millis();
    if(plugins && (plugin_request || overlay_hotkey(hid_keys_down(),hid_keys_held()))) {
      plugin_request=false;
      uint64_t pause_begin=sys_micros();
      going=show_overlay();
      t0+=sys_micros()-pause_begin;
      runner_pacing_init(&pace,sys_micros());
      if(!going) break;
    }
    input();
    for (;;) {
      if(plugin_request) break;
      uint64_t before=sys_micros();
      job->vblanks = runner_vblank(&pace,before);
      job->elapsed_us = before-t0;
      if (!run(JOB_RUN)) {
        snprintf(job->error, sizeof job->error, "The ARM11 stopped answering.");
        job->yield = YIELD_ERROR;
        going = false;
        break;
      }
      fsbridge_drop(job->close_files, job->close_dirs);
      job->close_files = job->close_dirs = 0;
      httpbridge_drop(job->close_http);
      job->close_http = 0;
      if (job->yield == YIELD_HTTP) {
        httpbridge_do((JobHttp *)(uintptr_t)job->http_pa);
        hid_scan(); input();
        continue;
      }
      if (job->yield == YIELD_FS) {
        fsbridge_do(&job->fs);
        if (sys_millis() - last_scan > 100) {
          hid_scan();
          last_scan = sys_millis();
          input();
        }
        continue;
      }
      if (job->yield == YIELD_FRAME) {
        uint64_t now = sys_micros();
        if (runner_present_due(&pace,now)) {
          if (!present()) { going=false; break; }
          now=sys_micros();
        }
        uint64_t wake=runner_next_wake(&pace,job->wake_us?t0+job->wake_us:UINT64_MAX);
        if (wake>now+1000u) sys_sleep((uint32_t)((wake-now)/1000u));
        break;
      }
      if (job->yield == YIELD_BUDGET) {
        if (runner_present_due(&pace,sys_micros()) && !present()) going=false;
        break;
      }
      going = false;
      break;
    }
  }
  stop_guest();
  fsbridge_close_all();
  httpbridge_drop(0xFFFFFFFFu);
  clock_restore();
  report(e, note, cap);
}

static Entry *find_homebrew(const char *path) {
  for(int i=0;i<n_entries;i++) if(!strcmp(entries[i].path,path)) return &entries[i];
  return NULL;
}

static void configure_launcher(Entry *entry,const InstalledApp *installed,char *note,size_t cap) {
  MenuInfo info={0};
  char directory[256]="/Aurora/Apps/C";
  const char *previous=NULL;
  if(installed) {
    info=installed->info;
    previous=installed->path;
    snprintf(directory,sizeof directory,"%s",previous);
    char *slash=strrchr(directory,'/'); if(slash) *slash=0;
  } else if(entry) snprintf(info.target,sizeof info.target,"%s",entry->path);
  if(!info.title[0]) {
    snprintf(info.title,sizeof info.title,"%s",entry?entry->title:installed->title);
    if(entry) snprintf(info.author,sizeof info.author,"%s",entry->author);
    if(info.author[0]) info.flags|=MENU_CREDIT_IN_TITLE;
  }
  if(title_plugin_read(info.target,(info.flags&MENU_PLUGIN)!=0)) info.flags|=MENU_PLUGIN;
  else info.flags&=~MENU_PLUGIN;
  if(!ui_install_popup(&info,entry?entry->icon:NULL,
                       installed && installed->has_icon?installed->icon:NULL)) return;
  fs_mkdir(directory);
  splash_begin(true);
  bool added=menu_install_ex(app_path(),directory,&info,entry?entry->icon:NULL,
                            previous,note,cap,loading_progress);
  splash_end(added && !core.home_pending);
  if(core.home_pending) exit(0);
  if(added && !title_plugin_write(info.target,(info.flags&MENU_PLUGIN)!=0))
    snprintf(note,cap,"Tile saved; could not save shared plugin setting.");
  if(added && entry) {
    snprintf(entry->title,sizeof entry->title,"%s",info.title);
    snprintf(entry->author,sizeof entry->author,"%s",info.author);
  }
  ui_sound(added?UI_SUCCESS:UI_ERROR);
}

static void install_homebrew(Entry *entry,char *note,size_t cap) {
  AppCatalog apps={0};
  apps_scan(&apps,"/Aurora/Apps");
  const InstalledApp *prior=NULL;
  for(size_t i=0;i<apps.count;i++) if(apps.items[i].linked &&
      !strcmp(apps.items[i].info.target,entry->path)) { prior=&apps.items[i]; break; }
  configure_launcher(entry,prior,note,cap);
  apps_free(&apps);
}

static void manage_apps(char *note,size_t cap) {
  size_t selected=0;
  AppCatalog apps={0};
  for(;;) {
    apps_scan(&apps,"/Aurora/Apps");
    int action=ui_manager(&apps,&selected,note);
    if(action==UI_MANAGER_BACK) break;
    InstalledApp *app=&apps.items[selected];
    if(action==UI_MANAGER_EDIT) {
      configure_launcher(find_homebrew(app->info.target),app,note,cap);
      continue;
    }
    bool erase_data=false;
    if(!ui_uninstall_popup(&apps,selected,&erase_data)) continue;
    ui_audio_suspend();
    await_safe_core();
    ui_working("Removing application...");
    bool removed=apps_uninstall(&apps,selected,erase_data,note,cap);
    ui_audio_resume();
    ui_sound(removed?UI_SUCCESS:UI_ERROR);
  }
  apps_free(&apps);
  scan();
}

static void fatal(const char *why) {
  gfx_rect(SCREEN_TOP, 0, 0, 400, 240, BG);
  gfx_print(SCREEN_TOP, 16, 16, FONT_TITLE, COLOR_WHITE, "3DSX Runner r14b");
  gfx_print(SCREEN_TOP, 16, 56, FONT_REGULAR, COLOR_WHITE, "%s", why);
  gfx_print(SCREEN_TOP, 16, 218, FONT_SMALL, DIM, "HOME: back to Aurora");
  for (;;)
    app_loop();
}

int main(void) {
  char note[200] = "";
  if (!app_from_home())
    fatal("Start the runner from Aurora (the Home Menu,\nFiles or the Terminal): programs run on the ARM11,\nwhich Aurora's core lends to the apps it starts.");
  os_cache_sync();
  if (ctrl->version < AUDIO_RUN11_VERSION) {
    snprintf(note, sizeof note, "This needs AuroraOS with ARM11 core %u or later\n(this one is %u). Copy a newer AURORAOS.BIN.",
             (unsigned)AUDIO_RUN11_VERSION, (unsigned)ctrl->version);
    fatal(note);
  }
  if (!glue_gpu())
    fatal("The GPU is not available.");
  n3ds = sys_is_new3ds();
  if (!fs_mkdir(app_dir()))
    snprintf(note, sizeof note, "SD card: cannot create the run-log folder.");
  atexit(leave);
  MenuInfo launch;
  int linked = menu_read(app_path(), &launch);
  const char *target=launch.target;
  if (linked < 0)
    fatal("Cannot read this launcher. Reinstall it using\n"
          "the latest 3dsxRunner and its Y button.");
  if (linked) {
    direct_launch = true;
    FILE *file = fopen(target, "rb");
    long size = -1;
    if (file) {
      if (!fseek(file, 0, SEEK_END)) size = ftell(file);
      fclose(file);
    }
    if (size >= 0) add_entry(target, (uint32_t)size);
    if (!n_entries)
      fatal("The linked .3dsx is missing or unreadable.\n"
            "Restore the file to its original SD location,\n"
            "or remove this tile and add the game again.");
    if(launch.title[0]) {
      snprintf(entries[0].title,sizeof entries[0].title,"%s",launch.title);
      snprintf(entries[0].author,sizeof entries[0].author,"%s",launch.author);
    }
    start(&entries[0], note, sizeof note, (launch.flags&MENU_NO_ARGS)!=0,
          title_plugin_read(target,(launch.flags&MENU_PLUGIN)!=0),false);
    if (note[0]) fatal(note);
    return 0;
  }
  ui_audio_init();
  scan();

  bool redraw = true;
  for (;;) {
    if (redraw) {
      draw_top(note);
      draw_bottom();
      redraw = false;
    }
    if (!app_loop())
      break;
    ui_audio_tick();
    uint32_t down = hid_keys_down();
    if(hid_touch_down()) {
      if(hid_touch_in(6,208,150,28)) down|=KEY_START;
      if(hid_touch_in(164,208,150,28)) down|=KEY_SELECT;
    }
    if(down&KEY_SELECT) { ui_audio_toggle(); redraw=true; }
    if(down&KEY_START) {
      ui_sound(UI_SELECT); manage_apps(note,sizeof note); redraw=true; continue;
    }
    if (!n_entries)
      continue;
    int before = sel;
    if (down & (KEY_DOWN | KEY_CPAD_DOWN))
      sel = (sel + 1) % n_entries;
    if (down & (KEY_UP | KEY_CPAD_UP))
      sel = (sel + n_entries - 1) % n_entries;
    if (down & (KEY_RIGHT | KEY_R))
      sel = sel + ROWS < n_entries ? sel + ROWS : n_entries - 1;
    if (down & KEY_LEFT)
      sel = sel >= ROWS ? sel - ROWS : 0;
    if (hid_touch_down()) {
      int row = (hid_touch_y() - 8) / (ROW_H + 2);
      if (row >= 0 && row < ROWS && scroll + row < n_entries) {
        if (scroll + row == sel)
          down |= KEY_A;
        sel = scroll + row;
      }
    }
    if (sel != before) {
      ui_sound(UI_MOVE);
      note[0] = 0;
      redraw = true;
    }
    if (down & KEY_Y) {
      ui_sound(UI_SELECT);
      install_homebrew(&entries[sel],note,sizeof note);
      redraw = true;
    } else if (down & (KEY_A | KEY_X)) {
      bool held_l=(hid_keys_held()&KEY_L)!=0;
      start(&entries[sel], note, sizeof note, (down & KEY_X) != 0,
            held_l || title_plugin_read(entries[sel].path,false),held_l);
      ui_audio_resume();
      redraw = true;
    }
  }
  return 0;
}
