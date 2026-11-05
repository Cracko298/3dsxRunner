#ifndef RUNNER_PACING_H
#define RUNNER_PACING_H
#include <stdint.h>
#include <stdbool.h>

#define RUNNER_REFRESH_US 16667ull
typedef struct { uint64_t next_vblank, next_present; } RunnerPacing;

static inline void runner_pacing_init(RunnerPacing *p, uint64_t now) {
  p->next_vblank=now;
  p->next_present=now+RUNNER_REFRESH_US;
}

static inline bool runner_deadline(uint64_t *deadline, uint64_t now) {
  if (now<*deadline) return false;
  *deadline+=((now-*deadline)/RUNNER_REFRESH_US+1u)*RUNNER_REFRESH_US;
  return true;
}

static inline uint32_t runner_vblank(RunnerPacing *p, uint64_t now) {
  return runner_deadline(&p->next_vblank,now)?1u:0u;
}

/* Delivering a vblank to the game must never postpone copying its finished
 * frame. Two deadlines, not one. */
static inline bool runner_present_due(RunnerPacing *p, uint64_t now) {
  return runner_deadline(&p->next_present,now);
}

static inline uint64_t runner_next_wake(const RunnerPacing *p, uint64_t guest_wake) {
  uint64_t next=p->next_vblank<p->next_present?p->next_vblank:p->next_present;
  return guest_wake<next?guest_wake:next;
}
#endif
