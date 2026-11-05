#ifndef RUNNER_CORELINK_H
#define RUNNER_CORELINK_H
#include <stdbool.h>
#include <stdint.h>
#include "audio.h"
#include "job.h"

typedef struct {
  AudioCtrl *ctrl;
  JobParams *job;
  volatile uint32_t *cancel;
  void (*sync)(void);
  uint32_t (*millis)(void);
  bool (*home)(void);
  bool home_pending, active_job, timed_out;
  void (*loading_tick)(void);
  uint64_t (*micros)(void);
  uint64_t last_done_us;
} CoreLink;
bool corelink_idle(CoreLink *c);
bool corelink_wait(CoreLink *c, uint32_t ms);
bool corelink_post(CoreLink *c, uint32_t cmd, uint32_t a0, uint32_t a1);
bool corelink_run(CoreLink *c, uint32_t cmd);
#endif
