#ifndef RUNNER_PACING_H
#define RUNNER_PACING_H
#include <stdint.h>
#include <stdbool.h>

#define RUNNER_REFRESH_US 16667ull
typedef struct { uint64_t next_vblank, next_present, present_us; } RunnerPacing;

static inline void runner_pacing_init(RunnerPacing *p, uint64_t now) {
  p->next_vblank=now;
  p->next_present=now+RUNNER_REFRESH_US;
  if (!p->present_us) p->present_us=RUNNER_REFRESH_US;
}

static inline void runner_pacing_lock(RunnerPacing *p, unsigned fps) {
  p->present_us = fps>=15 && fps<60 ? 1000000ull/fps : RUNNER_REFRESH_US;
}

static inline bool runner_deadline_every(uint64_t *deadline, uint64_t now, uint64_t period) {
  if (now<*deadline) return false;
  *deadline+=((now-*deadline)/period+1u)*period;
  return true;
}

static inline bool runner_deadline(uint64_t *deadline, uint64_t now) {
  return runner_deadline_every(deadline,now,RUNNER_REFRESH_US);
}

static inline uint32_t runner_vblank(RunnerPacing *p, uint64_t now) {
  return runner_deadline(&p->next_vblank,now)?1u:0u;
}

static inline bool runner_present_due(RunnerPacing *p, uint64_t now) {
  return runner_deadline_every(&p->next_present,now,p->present_us?p->present_us:RUNNER_REFRESH_US);
}

static inline uint64_t runner_next_wake(const RunnerPacing *p, uint64_t guest_wake) {
  uint64_t next=p->next_vblank<p->next_present?p->next_vblank:p->next_present;
  return guest_wake<next?guest_wake:next;
}
#endif
