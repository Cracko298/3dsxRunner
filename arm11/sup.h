#ifndef X3_SUP_H
#define X3_SUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "job.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define P ((JobParams *)JOB_PARAMS)

#define VA_CODE         0x00100000u
#define VA_HEAP         0x08000000u
#define VA_STACK_TOP    0x10000000u
#define VA_STACK        0x0FF00000u
#define VA_MAP_BEGIN    0x10000000u
#define VA_MAP_END      0x14000000u
#define LINEAR_DELTA    0x10000000u
#define VA_VRAM         0x1F000000u
#define PA_VRAM         0x18000000u
#define VRAM_SIZE       0x00600000u
#define VA_IO           0x1EC00000u
#define PA_IO           0x10100000u
#define IO_SIZE         0x00400000u
#define VA_CONFIG       0x1FF80000u
#define VA_SHARED       0x1FF81000u
#define VA_TLS          0x1FF82000u
#define TLS_SIZE        0x200u
#define VA_VECTORS      0xFFFF0000u
#define VA_FONT_SOURCE  0x1D000000u
#define PA_MPCORE       0x17E00000u

#define IO(pa) (*(volatile u32 *)((pa) - PA_IO + VA_IO))

void cheats_take(void);
void cheats_frame(void);
void mem_do(void);
void memtools_reset(void);
void gsp_overlay_frame(void);
void gsp_overlay_end(void);
void dsp_overlay_pause(bool paused);

#define RES_OK            0
#define RES_TIMEOUT       0x09401BFE
#define RES_INVALID_HANDLE 0xD8E007F7
#define RES_INVALID_ENUM  0xD8E007ED
#define RES_INVALID_ADDR  0xE0E01BF5
#define RES_OUT_OF_MEMORY 0xD86007F3
#define RES_NOT_FOUND     0xD88007FA
#define RES_UNIMPL        0xD900182F
#define RES_OUT_OF_HANDLES 0xD8600413
#define RES_FS_NOT_FOUND  0xC8804478
#define RES_FS_EXISTS     0xC82044BE
#define RES_FS_FAIL       0xC8A04554

typedef struct {
  u32 r[13];
  u32 sp, lr, pc, cpsr;
  u32 fpscr;
  u64 d[16];
  u32 tls;
} Ctx;
_Static_assert(offsetof(Ctx, pc) == 60 && offsetof(Ctx, d) == 72 && offsetof(Ctx, tls) == 200,
               "start.s offsets");

enum {
  OBJ_NONE, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_TIMER, OBJ_ARBITER, OBJ_THREAD,
  OBJ_PROCESS, OBJ_PORT, OBJ_SESSION, OBJ_MEMBLOCK, OBJ_RESLIMIT,
};
enum { RESET_ONESHOT, RESET_STICKY, RESET_PULSE };

typedef struct Thread Thread;
typedef struct Object Object;
typedef struct Service Service;

struct Object {
  u8 type, used;
  u16 refs;
  union {
    struct { u8 signaled, reset; } ev;
    struct { Thread *owner; u32 count; } mx;
    struct { s32 count, max; } sem;
    struct { u8 signaled, reset, armed; u64 next, interval; } tm;
    struct { Thread *t; } th;
    struct { const Service *svc; u32 data[6]; } se;
    struct { u32 pa, size, va; u8 kind; } mb;
    struct { const Service *svc; } port;
  } u;
};

enum { TS_FREE, TS_READY, TS_WAIT, TS_DEAD };
enum { WAIT_NONE, WAIT_SYNC, WAIT_ARB, WAIT_SLEEP, WAIT_ARM9, WAIT_VBLANK };

#define MAX_WAIT 32

struct Thread {
  Ctx ctx;
  u8 state, wait, wall, nw;
  s32 prio, core;
  u32 id, order;
  Object *obj;
  Object *wobj[MAX_WAIT];
  u32 waddr;
  u64 deadline;
  void (*arm9_done)(Thread *t);
};
#define NO_DEADLINE (~0ull)
#define MAX_THREADS 32

struct Service {
  const char *name;
  void (*request)(Thread *t, Object *session, u32 *cmd);
  void (*closed)(Object *session);
};

void sup_resume(Ctx *c) __attribute__((noreturn));
void sup_yield(void) __attribute__((noreturn));
extern Ctx *volatile sup_current_ctx;

enum { MAP_USER = 1, MAP_PRIV = 2, MAP_DEVICE = 4, MAP_UNCACHED = 8, MAP_RO = 16 };
void mmu_reset(void);
bool mmu_map(u32 va, u32 pa, u32 size, u32 flags);
void mmu_unmap(u32 va, u32 size);
u32 mmu_pa(u32 va);
bool mmu_user_write_ok(u32 va, u32 size);
bool mmu_user_ok(u32 va, u32 size);
void mmu_enter(void);
void mmu_leave(void);
void cache_clean_all(void);
void gpu_scanout(u32 top, u32 bottom);
void gpu_job_entry(void);
void gpu_lcd_sample(void);
extern bool gpu_lcd_watch;
void gpu_scanout_report(void);
void gpu_fill_start(int n, u32 start, u32 end, u32 value, u32 control);
bool gpu_fill_done(int n);
void gpu_fill_finish(int n);
void gpu_transfer_start(u32 in, u32 out, u32 indim, u32 outdim, u32 flags);
void gpu_texcopy_start(u32 in, u32 out, u32 size, u32 inline_, u32 outline, u32 flags);
bool gpu_ppf_done(void);
void gpu_ppf_finish(void);
void gpu_cmdlist_start(u32 pa, u32 size);
bool gpu_cmdlist_done(void);
void gpu_cmdlist_finish(void);
void gpu_async_timeout(const char *what) __attribute__((noreturn));
extern bool gsp_gx_inflight;
void gsp_gx_poll(void);
void gsp_gx_wait(void);
void gsp_gx_drain(void);
void gsp_gx_forget(void);
void gsp_gx_settle(void);
void gpu_scanout_restore(void);
void gpu_restore_config(void);
void gpu_report(void);
void gsp_report(void);
void gsp_present(void);
void tlb_flush(void);
void *page_alloc(void);
void page_free(void *p);
u32 sup_pa(const void *p);

extern Thread threads[MAX_THREADS];
extern Thread *cur;
u64 ticks(void);
u64 ns_to_ticks(s64 ns);
u32 handle_new(Object *o);
Object *handle_obj(u32 h);
Object *obj_new(int type);
void obj_ref(Object *o);
void obj_unref(Object *o);
Thread *thread_new(u32 entry, u32 arg, u32 sp, s32 prio, s32 core);
void thread_block(Thread *t, int kind, u64 deadline);
void thread_wake(Thread *t, s32 result);
void event_signal(Object *ev);
void kernel_tick(void);
Ctx *schedule(void);
void kernel_init(void);
Ctx *trap_svc(Ctx *c);
void trap_fault(u32 kind, Ctx *frame);
Ctx *trap_irq(Ctx *c);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sup_fail(const char *why) __attribute__((noreturn));
void job_check_cancel(void);
void gpu_abort_transfers(void);
s32 arm9_request(Thread *t, void (*done)(Thread *t));

static inline u32 ipc_header(u32 id, u32 normal, u32 translate) {
  return (id << 16) | (normal << 6) | translate;
}
void ipc_session_request(Thread *t, Object *session);
u32 session_new(const Service *svc, Object **out);
const Service *service_find(const char *name);
void ipc_unknown(const char *svc, u32 *cmd);
void ipc_reset(void);
u32 *static_buffer(Thread *t, u32 index, u32 *size);

extern const Service srv_service, apt_service, gsp_service, hid_service, fs_service,
    cfg_service, ptm_service, irrst_service, ac_service, dsp_service, http_service, stub_services[];
extern const Service fs_file_service, fs_dir_service;
void gsp_vblank(void);
void gsp_framebuffers(void);
void hid_update(void);
void fs_arm9_done(Thread *t);
void http_network_request(Thread *t, u32 op, void (*done)(Thread *));
JobHttp *http_network_result(void);

void gpu_init(void);
void gpu_fill(int engine, u32 start, u32 end, u32 value, u32 control);
void gpu_transfer(u32 in, u32 out, u32 indim, u32 outdim, u32 flags);
void gpu_texcopy(u32 in, u32 out, u32 size, u32 inline_, u32 outline, u32 flags);
void gpu_cmdlist(u32 pa, u32 size);

bool load_3dsx(void);
void shared_page_update(void);
extern u32 heap_size, linear_size, linear_va;
extern u32 heap_pa, heap_avail, linear_pa, linear_avail;
extern u32 font_pa, font_va;
#define FONT_RESERVE 0x48000u
u32 font_build(u8 *dst, u32 dst_va, u32 cap, const u32 source[3]);

void gsp_reset(void);
void hid_reset(void);
void fs_reset(void);
void misc_reset(void);
void dsp_reset(void);
void dsp_stop(void);
void dsp_tick(u64 now);
u64 dsp_deadline(void);
bool dsp_active(void);
bool dsp_signal(Object *o);
u32 dsp_resource(void);
bool mmu_base(void);
void mmu_tables_on(void);
void job_run(void) __attribute__((noreturn));
void guest_exit(s32 code) __attribute__((noreturn));

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
char *strncpy(char *d, const char *s, size_t n);
int vsnprintf(char *out, size_t cap, const char *fmt, __builtin_va_list ap);
int snprintf(char *out, size_t cap, const char *fmt, ...);

#endif
