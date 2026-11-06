#ifndef X3_JOB_H
#define X3_JOB_H

#include <stdint.h>

#define JOB_PARAMS 0x25100000u
#define JOB_CODE   0x25101000u
#define SUP_END    0x25300000u

#define JOB_MAGIC   0x58335355u
#define JOB_VERSION 16u

#define DISPLAY_SLOT_SIZE 0x80000u
#define DISPLAY_POOL_SIZE (3u * DISPLAY_SLOT_SIZE)

#define JOB_CANCEL_ADDR (JOB_CODE - 32u)
enum { JOB_CANCEL_NONE, JOB_CANCEL_HOME, JOB_CANCEL_TIMEOUT };

enum {
  JOB_LOAD = 1,
  JOB_RUN,
  JOB_STOP,
  JOB_PRESENT,
  JOB_CHEATS, JOB_MEM, JOB_OVERLAY_BEGIN, JOB_OVERLAY_FRAME, JOB_OVERLAY_END,
};

enum {
  MEM_READ = 1,
  MEM_WRITE,
  MEM_SEARCH,

  MEM_REFINE,
  MEM_VALUES,
  MEM_REGIONS,
};
enum { MT_U8, MT_U16, MT_U32, MT_FLOAT };
enum {
  CMP_EQ, CMP_NE, CMP_GT, CMP_LT,
  CMP_CHANGED, CMP_UNCHANGED, CMP_INC, CMP_DEC,
  CMP_INC_BY, CMP_DEC_BY,
};
typedef struct {
  uint32_t va, old;
} JobHit;
typedef struct {
  uint32_t va, size;
  char name[16];
} JobRegionInfo;
#define JOB_MEM_REGIONS 8
typedef struct {
  uint32_t op, type, cmp, value, flags;
  uint32_t va, size, buf;
  uint32_t hits, cap;
  uint32_t count;
  uint32_t first, n;
  uint32_t overflow;
  int32_t result;
  uint32_t n_regions;
  JobRegionInfo regions[JOB_MEM_REGIONS];
} JobMem;

enum {
  YIELD_NONE = 0,
  YIELD_FRAME,
  YIELD_FS,
  YIELD_BUDGET,

  YIELD_EXIT,
  YIELD_CRASH,
  YIELD_ERROR,
  YIELD_HTTP,
  YIELD_APPLET,
};

enum { APPLET_KEYBOARD = 1, APPLET_ERROR = 2 };
#define APPLET_TEXT 1024u
typedef struct {
  uint32_t kind;
  uint32_t max_len;
  uint32_t buttons;
  uint32_t password;
  uint32_t numpad;
  uint32_t valid;
  uint32_t error_code;
  int32_t pressed;
  char hint[264];
  char button[3][52];
  char text[APPLET_TEXT];
} JobApplet;

typedef struct {
  uint32_t pa, size;
} JobRegion;

typedef struct {
  uint32_t pa;
  uint32_t format;
  uint32_t stride;
  uint32_t serial;
} JobFb;

enum {
  FSOP_OPEN = 1,
  FSOP_READ,
  FSOP_WRITE,
  FSOP_SIZE,
  FSOP_SETSIZE,
  FSOP_CLOSE,
  FSOP_DELETE,
  FSOP_RENAME,
  FSOP_MKDIR,
  FSOP_RMDIR,
  FSOP_CREATE,
  FSOP_OPENDIR,
  FSOP_READDIR,
  FSOP_CLOSEDIR,
  FSOP_STAT,
  FSOP_FREE,
};

typedef struct {
  char name[256];
  uint32_t is_dir;
  uint32_t pad;
  uint64_t size;
} JobDirent;

typedef struct {
  uint32_t op, file, flags, size;
  uint64_t offset, value64;
  uint32_t buf;
  int32_t result;
  uint32_t value;
  uint32_t pad;
  char path[256], path2[256];
} JobFs;

typedef struct {
  uint32_t r[16], cpsr;
  uint32_t kind;
  uint32_t far, fsr;
  uint32_t thread;
  uint32_t in_sup;
  uint32_t entry;
  uint32_t code[12];
  uint32_t stack[20];
  struct { uint32_t header, thread; char svc[8]; } recent[8];
} JobCrash;

#define HTTP_SLOTS 4u
#define HTTP_CHUNK 4096u
enum { HTTPOP_REQUEST=1, HTTPOP_READ, HTTPOP_HEADER, HTTPOP_NET_STATUS, HTTPOP_NET_CONNECT };
typedef struct {
  uint32_t op, slot, method, body_size, offset, size, status, length;
  int32_t result;
  char url[1024], headers[1024], error[128];
  uint8_t body[1024], data[HTTP_CHUNK];
} JobHttp;

#define JOB_LOG_SIZE 2048u

typedef struct {
  uint32_t magic, version, cmd;
  uint32_t done;

  uint32_t file_pa, file_size;
  char path[256];
  JobRegion code;
  JobRegion linear;
  uint32_t new3ds, cpu_mhz;
  uint64_t unix_ms;

  uint32_t font_head_pa, font_glyphs_pa, font_atlas_pa;

  uint32_t vblanks;
  uint64_t elapsed_us;
  uint32_t held;
  int32_t cpad_x, cpad_y;
  uint32_t touch_x, touch_y;
  uint32_t slider;

  uint32_t yield;
  JobFb top, bottom;
  uint32_t top_right_pa;
  uint64_t wake_us;
  JobFs fs;
  uint32_t close_files;
  uint32_t http_pa, close_http;
  uint32_t close_dirs;
  int32_t exit_code;
  JobCrash crash;
  char error[128];
  char title[64];
  uint32_t log_head;
  char log[JOB_LOG_SIZE];
  uint32_t svc_count, ipc_count, gx_count, threads;
  uint32_t launch_flags;
  uint32_t preempts;
  uint32_t display_pending;
  JobRegion display;
  uint32_t plugin_enabled, cheat_pa, cheat_bytes, cheats_on, cheat_failed, cheat_runs;
  JobMem mem;

  uint64_t wall_us, prev_done_us;
  uint32_t applet_pa;
  uint32_t self_romfs;
  uint32_t retail_stack, retail_kernel, title_lo, title_hi;
  char save_dir[96];
  uint32_t retail_appmem;
  uint32_t trace_pa, trace_size, trace_head;
  int16_t cstick_x, cstick_y;
  uint32_t zlzr;
} JobParams;

_Static_assert(sizeof(JobParams) <= JOB_CANCEL_ADDR - JOB_PARAMS, "JobParams must not overlap cancellation");

#endif
