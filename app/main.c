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
#include "settings.h"
#include "ncch.h"
#include "cstick.h"

#include <errno.h>
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

static char program_services[700];

static void stamp(char *out, size_t cap, bool for_file) {
  SysTime t;
  if (!sys_time(&t)) {
    snprintf(out, cap, for_file ? "unknown-time" : "time unknown");
    return;
  }
  snprintf(out, cap, for_file ? "%04d-%02d-%02d_%02d-%02d" : "%04d-%02d-%02d %02d:%02d",
           t.year, t.month, t.day, t.hour, t.minute);
}

static void write_crash(FILE *f) {
  if (job->yield!=YIELD_CRASH) return;
  const JobCrash *c=&job->crash;
  fprintf(f,"crash: kind=%lu supervisor=%lu thread=%lu PC=%08lX LR=%08lX SP=%08lX CPSR=%08lX FAR=%08lX FSR=%08lX\n",
          (unsigned long)c->kind,(unsigned long)c->in_sup,(unsigned long)c->thread,
          (unsigned long)c->r[15],(unsigned long)c->r[14],(unsigned long)c->r[13],
          (unsigned long)c->cpsr,(unsigned long)c->far,(unsigned long)c->fsr);
  for (unsigned i=0;i<13;i++) fprintf(f,"r%u=%08lX%c",i,(unsigned long)c->r[i],i%4==3?'\n':' ');
  fputc('\n',f);
  fprintf(f,"thread entry=%08lX\ncode at %08lX:",(unsigned long)c->entry,(unsigned long)((c->r[15]&~3u)-32u));
  for (unsigned i=0;i<12;i++) fprintf(f,"%s%08lX",i==8?" >":" ",(unsigned long)c->code[i]);
  fprintf(f,"\nstack:");
  for (unsigned i=0;i<20;i++) fprintf(f,"%s%08lX",i%10?" ":"\n  ",(unsigned long)c->stack[i]);
  fprintf(f,"\nlast requests (oldest first):\n");
  for (unsigned i=0;i<8;i++)
    if (c->recent[i].header)
      fprintf(f,"  thread %lu: %.8s 0x%lX (header %08lX)\n",(unsigned long)c->recent[i].thread,
              c->recent[i].svc,(unsigned long)(c->recent[i].header>>16),(unsigned long)c->recent[i].header);
}

static void exit_trace(const char *stage, bool fresh) {
  char path[300];
  snprintf(path,sizeof path,"%s/last-exit.txt",app_dir());
  FILE *f=fopen(path,fresh?"w":"a");
  if (!f) return;
  bool idle=corelink_idle(&core);
  char when[24];
  stamp(when,sizeof when,false);
  fprintf(f,"r16u %s, %lu ms: %s; HOME=%u seq=%lu ack=%lu idle=%u\n",
          when,(unsigned long)sys_millis(),stage,core.home_pending,
          (unsigned long)ctrl->cmd_seq,(unsigned long)ctrl->ack_seq,idle);
  if (fresh) fprintf(f,"%s\n%s",active_path,program_services);
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

static const void *active_entry;
static void save_active_log(void);

static void leave(void) {
  leaving=true;
  core.loading_tick=NULL;
  exit_trace("leave.begin",false);
  await_safe_core();
  stop_guest();
  fsbridge_close_all();
  save_active_log();
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
  uint8_t kind;
} Entry;

enum { KIND_3DSX, KIND_3DS, KIND_CIA, KIND_CXI, KINDS };
static const char *const kind_names[KINDS] = {"3DSX", "3DS", "CIA", "CXI"};

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
  NcchInfo ncch = {0};
  bool packaged = got < 0x20 || memcmp(hdr, "3DSX", 4);
  if (packaged && !ncch_open(f, &ncch, NULL, 0)) {
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
  {
    size_t n = strlen(path);
    e->kind = n > 4 && !strcasecmp(path + n - 4, ".cia") ? KIND_CIA
            : n > 4 && !strcasecmp(path + n - 4, ".cxi") ? KIND_CXI
            : n > 4 && !strcasecmp(path + n - 4, ".3ds") ? KIND_3DS : KIND_3DSX;
  }
  const char *base = strrchr(path, '/');
  snprintf(e->title, sizeof e->title, "%s", base ? base + 1 : path);
  char *dot = strrchr(e->title, '.');
  if (dot)
    *dot = 0;
  uint16_t header_size = (uint16_t)(hdr[4] | hdr[5] << 8);
  static uint8_t s[0x36C0];
  bool have_smdh = false;
  if (packaged) {
    have_smdh = ncch_smdh(f, &ncch, s);
  } else if (got >= 0x2C && header_size > 0x20) {
    uint32_t smdh = hdr[0x20] | hdr[0x21] << 8 | hdr[0x22] << 16 | (uint32_t)hdr[0x23] << 24;
    uint32_t smdh_size = hdr[0x24] | hdr[0x25] << 8 | hdr[0x26] << 16 | (uint32_t)hdr[0x27] << 24;
    have_smdh = smdh && smdh_size >= sizeof s && !fseek(f, (long)smdh, SEEK_SET) &&
                fread(s, 1, sizeof s, f) == sizeof s && !memcmp(s, "SMDH", 4);
  }
  {
    if (have_smdh) {
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

static bool has_ext(const char *name, const char *ext) {
  size_t n = strlen(name), k = strlen(ext);
  return n > k && !strcasecmp(name + n - k, ext);
}
static bool ends_3dsx(const char *name) { return has_ext(name, ".3dsx") || has_ext(name, ".3ds"); }
static bool ends_packaged(const char *name) {
  return has_ext(name, ".cia") || has_ext(name, ".cxi") || has_ext(name, ".3ds");
}

static int by_title(const void *a, const void *b) {
  const Entry *x = a, *y = b;
  if (settings.group_by_type && x->kind != y->kind)
    return (int)x->kind - (int)y->kind;
  return strcasecmp(x->title, y->title);
}

static int folder = -1, fsel;
static bool grouped(void) { return settings.group_by_type && n_entries; }
static int kind_lo(int k) { int i = 0; while (i < n_entries && entries[i].kind < k) i++; return i; }
static int kind_hi(int k) { int i = kind_lo(k); while (i < n_entries && entries[i].kind == k) i++; return i; }
static int roots(int out[KINDS]) {
  int n = 0;
  for (int k = 0; k < KINDS; k++) if (kind_hi(k) > kind_lo(k)) out[n++] = k;
  return n;
}
static void range(int *lo, int *hi) {
  if (grouped() && folder >= 0) *lo = kind_lo(folder), *hi = kind_hi(folder);
  else *lo = 0, *hi = n_entries;
}
static void open_folder_of(int i) {
  if (!grouped() || i < 0 || i >= n_entries) return;
  folder = entries[i].kind;
  int r[KINDS], n = roots(r);
  for (int j = 0; j < n; j++) if (r[j] == folder) fsel = j;
  sel = scroll = i;
}
static void resort(void) {
  if (!n_entries) return;
  char keep[200];
  snprintf(keep, sizeof keep, "%s", entries[sel].path);
  qsort(entries, (size_t)n_entries, sizeof *entries, by_title);
  folder = -1;
  for (int i = 0; i < n_entries; i++) if (!strcmp(entries[i].path, keep)) { sel = scroll = i; break; }
  if (grouped()) open_folder_of(sel), folder = -1;
}

static void scan_dir(const char *dir, bool (*wanted)(const char *)) {
  FsDir *d = fs_dir_open(dir);
  if (!d)
    return;
  FsEntry e;
  char path[200];
  while (fs_dir_read(d, &e))
    if (!e.is_dir && wanted(e.name) &&
        snprintf(path, sizeof path, "%s/%s", dir, e.name) < (int)sizeof path)
      add_entry(path, e.size);
  fs_dir_close(d);
}

static void scan(void) {
  for (int i=0;i<n_entries;i++) free(entries[i].icon);
  free(entries); entries=NULL; n_entries=cap_entries=sel=scroll=0; folder=-1; fsel=0;
  scan_dir("/3ds", ends_3dsx);
  scan_dir("/cias", ends_packaged);
  scan_dir("/cia", ends_packaged);
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
  gfx_print(SCREEN_TOP, 16, 12, FONT_TITLE, COLOR_WHITE, "3DSX Runner r16u");
  gfx_print(SCREEN_TOP, 18 + gfx_text_width(FONT_TITLE, "3DSX Runner r16u"), 18, FONT_SMALL, DIM,
            "3DS software on AuroraOS");
  if (!n_entries) {
    gfx_print(SCREEN_TOP, 16, 70, FONT_REGULAR, COLOR_WHITE,
              "No software was found.\n\nPut .3dsx and .3ds files in SD:/3ds, and unencrypted\n.cia, .cxi and .3ds files in SD:/cias (or SD:/cia).");
  } else if (grouped() && folder < 0) {
    int r[KINDS], n = roots(r);
    int k = r[fsel < n ? fsel : 0];
    gfx_round_rect(SCREEN_TOP, 12, 48, 376, 120, 10, PANEL);
    gfx_round_rect(SCREEN_TOP, 24, 60, 96, 96, 12, accent());
    gfx_print(SCREEN_TOP, 72 - gfx_text_width(FONT_TITLE, kind_names[k]) / 2, 96, FONT_TITLE, COLOR_BLACK, "%s",
              kind_names[k]);
    gfx_print(SCREEN_TOP, 136, 64, FONT_BOLD, COLOR_WHITE, "%s files", kind_names[k]);
    gfx_print(SCREEN_TOP, 136, 86, FONT_REGULAR, DIM, "%d in this folder", kind_hi(k) - kind_lo(k));
    gfx_print(SCREEN_TOP, 136, 120, FONT_SMALL, DIM, "A: open   B: back out of a folder");
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
  gfx_print(SCREEN_TOP,16,226,FONT_SMALL,DIM,grouped()&&folder>=0?"L + launch: cheats   B: back to folders   SELECT: settings":"L + launch: cheats   SELECT: settings   HOME: exit");
}

static void draw_bottom(void) {
  gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 240, BG);
  if (grouped() && folder < 0) {
    int r[KINDS], n = roots(r);
    for (int i = 0; i < n; i++) {
      int y = 8 + i * (ROW_H + 2), k = r[i];
      uint32_t ink = i == fsel ? COLOR_BLACK : COLOR_WHITE;
      if (i == fsel)
        gfx_round_rect(SCREEN_BOTTOM, 6, y, 308, ROW_H, 6, accent());
      gfx_print(SCREEN_BOTTOM, 14, y + 5, FONT_BOLD, ink, "%s", kind_names[k]);
      char count[16];
      snprintf(count, sizeof count, "%d", kind_hi(k) - kind_lo(k));
      gfx_print(SCREEN_BOTTOM, 300 - gfx_text_width(FONT_REGULAR, count), y + 5, FONT_REGULAR, ink, "%s", count);
    }
  } else {
  int lo, hi;
  range(&lo, &hi);
  if (scroll < lo)
    scroll = lo;
  if (sel < scroll)
    scroll = sel;
  if (sel >= scroll + ROWS)
    scroll = sel - ROWS + 1;
  for (int i = 0; i < ROWS && scroll + i < hi; i++) {
    int k = scroll + i, y = 8 + i * (ROW_H + 2);
    if (k == sel)
      gfx_round_rect(SCREEN_BOTTOM, 6, y, 308, ROW_H, 6, accent());
    gfx_print(SCREEN_BOTTOM, 14, y + 5, FONT_REGULAR, k == sel ? COLOR_BLACK : COLOR_WHITE, "%.36s",
              entries[k].title);
  }
  }
  gfx_round_rect(SCREEN_BOTTOM,6,208,150,28,6,PANEL);
  gfx_round_rect(SCREEN_BOTTOM,164,208,150,28,6,PANEL);
  gfx_print(SCREEN_BOTTOM,16,216,FONT_SMALL,COLOR_WHITE,"START: Manage apps");
  gfx_print(SCREEN_BOTTOM,174,216,FONT_SMALL,COLOR_WHITE,"SELECT: Settings");
}

static void write_log(FILE *f, const Entry *e, const char *outcome, const char *when) {
  fprintf(f, "3DSX Runner r16u: %s\n%s\nSD:%s\n%s\n\n", outcome, e->title, e->path, when);
  fprintf(f, "kernel calls %u, service requests %u, GPU commands %u, time slices run out %u\n\n",
          (unsigned)job->svc_count, (unsigned)job->ipc_count, (unsigned)job->gx_count, (unsigned)job->preempts);
  if (program_services[0])
    fprintf(f, "%s\n", program_services);
  fprintf(f,"launch=%s; display=%08lX size=%lX; swaps=%lu/%lu\n",
          job->launch_flags&1u?"X/no-arguments":"A/path-argument",
          (unsigned long)job->display.pa,(unsigned long)job->display.size,
          (unsigned long)job->top.serial,(unsigned long)job->bottom.serial);
  write_crash(f);
  uint32_t head = job->log_head, n = head < JOB_LOG_SIZE ? head : JOB_LOG_SIZE;
  for (uint32_t i = head - n; i != head; i++)
    fputc(job->log[i % JOB_LOG_SIZE], f);
  if (job->trace_pa && job->trace_size && job->trace_head) {
    const char *t = (const char *)(uintptr_t)job->trace_pa;
    uint32_t th = job->trace_head, tn = th < job->trace_size ? th : job->trace_size;
    fprintf(f, "\n---- detailed log%s ----\n", th > job->trace_size ? " (most recent part)" : "");
    for (uint32_t i = th - tn; i != th; i++)
      fputc(t[i % job->trace_size], f);
  }
}

static void save_log(const Entry *e, const char *outcome) {
  char path[300], when[24], file_when[24], name[48];
  active_entry = NULL;
  stamp(when, sizeof when, false);
  stamp(file_when, sizeof file_when, true);
  size_t k = 0;
  for (const char *c = e->title; *c && k + 1 < sizeof name; c++) {
    unsigned char ch = (unsigned char)*c;
    if (ch < 0x20 || strchr("\\/:*?\"<>|", ch))
      ch = ' ';
    if (ch == ' ' && (!k || name[k - 1] == ' '))
      continue;
    name[k++] = (char)ch;
  }
  while (k && (name[k - 1] == ' ' || name[k - 1] == '.'))
    k--;
  name[k] = 0;
  char dir[200];
  bool packaged = !strncmp(e->path, "/cias/", 6) || !strncmp(e->path, "/cia/", 5);
  if (packaged) {
    if ((job->launch_flags & 4u) && job->save_dir[0]) snprintf(dir, sizeof dir, "%s", job->save_dir);
    else snprintf(dir, sizeof dir, "/Aurora/Apps/C/%s", k ? name : "untitled");
    fs_mkdir("/Aurora/Apps");
    fs_mkdir("/Aurora/Apps/C");
    fs_mkdir(dir);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "/Logs");
  } else {
    snprintf(dir, sizeof dir, "%s/runs", app_dir());
  }
  fs_mkdir(dir);
  for (int n = 1; n < 100; n++) {
    if (n == 1)
      snprintf(path, sizeof path, "%s/%s %s.txt", dir, file_when, k ? name : "untitled");
    else
      snprintf(path, sizeof path, "%s/%s %s (%d).txt", dir, file_when, k ? name : "untitled", n);
    FILE *probe = fopen(path, "r");
    if (!probe)
      break;
    fclose(probe);
  }
  FILE *f = fopen(path, "w");
  bool copied = false;
  int err = f ? 0 : errno;
  if (f) {
    write_log(f, e, outcome, when);
    copied = fclose(f) == 0;
    if (!copied) err = errno;
  }
  {
    char line[360];
    snprintf(line, sizeof line, copied ? "log saved: SD:%s" : "log NOT saved: SD:%s (error %d)", path, err);
    exit_trace(line, false);
  }
  char last[300];
  snprintf(last, sizeof last, "%s/last-run.txt", app_dir());
  f = fopen(last, "w");
  if (f) {
    write_log(f, e, outcome, when);
    if (copied) fprintf(f, "\n(also saved as SD:%s)\n", path);
    else fprintf(f, "\n(couldn't save a copy as SD:%s)\n", path);
    fclose(f);
  }
}

static void save_active_log(void) {
  if (active_entry) save_log((const Entry *)active_entry, "you pressed HOME");
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

static uint32_t program_romfs;
static bool program_retail;
static uint32_t program_stack, program_kernel;
static NcchInfo program_ncch;
static uint64_t program_title;
static char program_name[64];

static void describe_services(const NcchInfo *n) {
  static const char *const have[] = {
    "srv:", "APT:U", "APT:S", "APT:A", "gsp::Gpu", "hid:USER", "hid:SPVR", "ir:rst", "fs:USER",
    "cfg:u", "cfg:s", "cfg:i", "ptm:u", "ptm:sysm", "ptm:gets", "ptm:sets", "ptm:s", "ptm:play",
    "ac:u", "ac:i", "dsp::DSP", "mic:u", "http:C",
  };
  char names[34][9];
  int count = ncch_services(n, names);
  char yes[34 * 9 + 1] = "", no[34 * 9 + 1] = "";
  for (int i = 0; i < count; i++) {
    bool real = false;
    for (unsigned k = 0; k < sizeof have / sizeof have[0]; k++)
      if (!strcmp(names[i], have[k])) real = true;
    char *dst = real ? yes : no;
    size_t len = strlen(dst), k = strlen(names[i]);
    dst[len] = ' ';
    memcpy(dst + len + 1, names[i], k + 1);
  }
  snprintf(program_services, sizeof program_services,
           "services the title may use (%d):\n  implemented:%s\n  stand-in (answers OK):%s\n",
           count, yes[0] ? yes : " none", no[0] ? no : " none");
}
static void save_folder(char *out, size_t cap) {
  char name[64];
  size_t k = 0;
  for (const char *c = program_name; *c && k + 1 < sizeof name; c++) {
    unsigned char ch = (unsigned char)*c;
    if (ch < 0x20 || strchr("\\/:*?\"<>|", ch))
      ch = ' ';
    if (ch == ' ' && (!k || name[k - 1] == ' '))
      continue;
    name[k++] = (char)ch;
  }
  while (k && (name[k - 1] == ' ' || name[k - 1] == '.'))
    k--;
  name[k] = 0;
  if (!k)
    snprintf(name, sizeof name, "%016llX", (unsigned long long)program_title);
  snprintf(out, cap, "/Aurora/Apps/C/%s", name);
}

static uint8_t *read_program(const char *path, uint32_t *bytes, char *why, size_t cap) {
  FILE *f = fopen(path, "rb");
  uint8_t h[0x100];
  program_romfs = 0;
  program_retail = false;
  program_stack = program_kernel = 0;
  program_title = 0;
  program_name[0] = 0;
  program_services[0] = 0;
  if (f && fread(h, 1, 0x20, f) == 0x20 && memcmp(h, "3DSX", 4)) {
    NcchInfo n;
    uint8_t *image = NULL;
    if (ncch_open(f, &n, why, cap) && n.platform == 2 && !n3ds) {
      snprintf(why, cap, "This game is made for the New 3DS only, so it can't run on this console.");
      fclose(f);
      return NULL;
    }
    if (n.ok) {
      loading_progress(0, 1);
      bool compressed;
      int prm = 1;
      if (ncch_code_size(&n, &compressed) > (4u << 20) && compressed)
        image = ncch_build_packed(f, &n, bytes, &prm, why, cap);
      else
        image = ncch_build_3dsx(f, &n, bytes, why, cap);
      program_romfs = image ? ncch_romfs_level3(f, &n) : 0;
      bool packed = image && (image[0x0F] & 0x80u);
      if (image && (packed ? prm != 1 : (*bytes >= 0x38 + 8 && memcmp(image + 0x38 + 4, "_prm", 4)))) {
        program_retail = true;
        program_stack = ncch_stack_size(&n);
        program_kernel = ncch_kernel_version(&n);
        program_title = n.title_id;
        program_ncch = n;
        describe_services(&n);
        static uint8_t smdh[0x36C0];
        if (ncch_smdh(f, &n, smdh))
          utf16_to_utf8((const uint16_t *)(const void *)(smdh + 8 + 0x200), 0x40, program_name, sizeof program_name);
      }
      loading_progress(1, 1);
    }
    fclose(f);
    if (image && *bytes > FILE_PA_LIMIT) {
      snprintf(why, cap, "The program is too big to load (%u KB).", (unsigned)(*bytes >> 10));
      free(image);
      return NULL;
    }
    return image;
  }
  if (!f || memcmp(h, "3DSX", 4)) {
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
  if (n3ds) {
    CStick c;
    cstick_read(&c);
    job->cstick_x = c.x;
    job->cstick_y = c.y;
    job->zlzr = (c.zl ? 1u : 0u) | (c.zr ? 2u : 0u);
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
  active_entry = NULL;
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
  if (settings.remember_last && strcmp(settings.last,e->path)) {
    snprintf(settings.last,sizeof settings.last,"%s",e->path);
    settings_save();
  }
  exit_trace("launch",true);
  memcpy((void *)JOB_CODE, sup_blob, (size_t)(sup_blob_end - sup_blob));
  memset(job, 0, sizeof *job);
  job->magic = JOB_MAGIC;
  job->version = JOB_VERSION;
  job->file_pa = (uint32_t)file;
  job->file_size = bytes;
  job->display=(JobRegion){((uint32_t)display_allocation+0xFFFu)&~0xFFFu,DISPLAY_POOL_SIZE};
  job->launch_flags = (no_args ? 1u : 0u) | (settings.detailed_logs ? 8u : 0u);
  job->self_romfs = program_romfs;
  if (program_retail) {
    job->launch_flags |= 4u;
    job->retail_stack = program_stack;
    job->retail_kernel = program_kernel;
    job->title_lo = (uint32_t)program_title;
    job->title_hi = (uint32_t)(program_title >> 32);
    save_folder(job->save_dir, sizeof job->save_dir);
    char dir[128];
    fs_mkdir("/Aurora/Apps");
    fs_mkdir("/Aurora/Apps/C");
    fs_mkdir(job->save_dir);
    snprintf(dir, sizeof dir, "%s/SaveData", job->save_dir);
    fs_mkdir(dir);
    snprintf(dir, sizeof dir, "%s/ExtData", job->save_dir);
    fs_mkdir(dir);
  }
  {
    char sync_path[300];
    snprintf(sync_path,sizeof sync_path,"%s/sync-gx.txt",app_dir());
    FILE *sync=fopen(sync_path,"r");
    if (sync) { job->launch_flags |= 2u; fclose(sync); }
    if (settings.sync_gx) job->launch_flags |= 2u;
  }
  job->plugin_enabled=plugins;
  snprintf(job->path, sizeof job->path, "sdmc:%s", e->path);
  bool big_mode = n3ds && (program_ncch.platform == 2 || ncch_code_size(&program_ncch, NULL) >= (3584u << 10));
  job->retail_appmem = program_retail ? ncch_app_memory(&program_ncch, big_mode) : 0;
  uint32_t appmem = job->retail_appmem & ~0xFu;
  if (n3ds && program_retail && program_kernel >= 0x22Cu && appmem && appmem <= 0x06000000u) {
    const uint8_t *x = program_ncch.exheader;
    uint32_t pages = (x[0x14] | x[0x15] << 8 | x[0x16] << 16) + (x[0x24] | x[0x25] << 8 | x[0x26] << 16) +
                     (x[0x34] | x[0x35] << 8 | x[0x36] << 16);
    uint32_t bss = x[0x3C] | x[0x3D] << 8 | x[0x3E] << 16 | (uint32_t)x[0x3F] << 24;
    uint32_t image = pages * 0x1000u + bss + program_stack + 0x100000u;
    uint32_t code = ((image + 0xFFFFFu) & ~0xFFFFFu) + appmem;
    if (code > 0x05000000u) code = 0x05000000u;
    job->code = (JobRegion){0x28000000u, code};
    job->linear = (JobRegion){0x28000000u + code, 0x08000000u - code};
  } else if (n3ds && program_retail && program_kernel >= 0x22Cu && appmem > 0x06000000u) {
    job->code = (JobRegion){0x28000000u, 0x08000000u};
    job->linear = (JobRegion){0x28000000u, 0x08000000u};
    job->launch_flags |= 0x10u;
  } else if (n3ds && program_retail && program_kernel && program_kernel < 0x22Cu) {
    job->code = (JobRegion){0x28000000u, 0x08000000u};
    job->linear = (JobRegion){0x23400000u, 0x00A00000u};
  } else if (n3ds) {
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
  supervisor_started=true;
  active_entry = e;
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

  uint64_t t0 = sys_micros();
  RunnerPacing pace={0};
  unsigned fps=title_fps_read(e->path);
  runner_pacing_lock(&pace,fps?fps:settings.frame_lock);
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
      if (job->yield == YIELD_APPLET) {
        extern bool applet_show(JobParams *job);
        uint64_t pause_begin = sys_micros();
        going = applet_show(job);
        t0 += sys_micros() - pause_begin;
        runner_pacing_init(&pace, sys_micros());
        if (!going) break;
        hid_scan(); input();
        continue;
      }
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
  unsigned fps=title_fps_read(info.target);
  if(!ui_install_popup(&info,&fps,entry?entry->icon:NULL,
                       installed && installed->has_icon?installed->icon:NULL)) return;
  fs_mkdir(directory);
  splash_begin(true);
  bool added=menu_install_ex(app_path(),directory,&info,entry?entry->icon:NULL,
                            previous,note,cap,loading_progress);
  splash_end(added && !core.home_pending);
  if(core.home_pending) exit(0);
  if(added && !title_plugin_write(info.target,(info.flags&MENU_PLUGIN)!=0))
    snprintf(note,cap,"Tile saved; could not save shared plugin setting.");
  if(added && !title_fps_write(info.target,fps))
    snprintf(note,cap,"Tile saved; could not save the frame lock.");
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
  gfx_print(SCREEN_TOP, 16, 16, FONT_TITLE, COLOR_WHITE, "3DSX Runner r16u");
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
  settings_load();
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
  if (settings.remember_last && settings.last[0])
    for (int i = 0; i < n_entries; i++)
      if (!strcmp(entries[i].path, settings.last)) { sel = i; open_folder_of(i); break; }

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
    if(down&KEY_SELECT) {
      bool was=settings.group_by_type;
      ui_sound(UI_SELECT); ui_settings();
      if(was!=settings.group_by_type) resort();
      redraw=true; continue;
    }
    if(down&KEY_START) {
      ui_sound(UI_SELECT); manage_apps(note,sizeof note); redraw=true; continue;
    }
    if (!n_entries)
      continue;
    if (grouped() && folder < 0) {
      int r[KINDS], n = roots(r), was = fsel;
      if (down & (KEY_DOWN | KEY_CPAD_DOWN)) fsel = (fsel + 1) % n;
      if (down & (KEY_UP | KEY_CPAD_UP)) fsel = (fsel + n - 1) % n;
      if (hid_touch_down()) {
        int row = (hid_touch_y() - 8) / (ROW_H + 2);
        if (row >= 0 && row < n) {
          if (row == fsel) down |= KEY_A;
          fsel = row;
        }
      }
      if (fsel != was) { ui_sound(UI_MOVE); note[0] = 0; redraw = true; }
      if (down & KEY_A) {
        ui_sound(UI_SELECT);
        folder = r[fsel];
        sel = scroll = kind_lo(folder);
        redraw = true;
      }
      continue;
    }
    if (grouped() && (down & KEY_B)) {
      ui_sound(UI_BACK);
      folder = -1;
      redraw = true;
      continue;
    }
    int lo, hi;
    range(&lo, &hi);
    int before = sel;
    if (down & (KEY_DOWN | KEY_CPAD_DOWN))
      sel = sel + 1 < hi ? sel + 1 : lo;
    if (down & (KEY_UP | KEY_CPAD_UP))
      sel = sel > lo ? sel - 1 : hi - 1;
    if (down & (KEY_RIGHT | KEY_R))
      sel = sel + ROWS < hi ? sel + ROWS : hi - 1;
    if (down & KEY_LEFT)
      sel = sel - ROWS >= lo ? sel - ROWS : lo;
    if (hid_touch_down()) {
      int row = (hid_touch_y() - 8) / (ROW_H + 2);
      if (row >= 0 && row < ROWS && scroll + row < hi) {
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
