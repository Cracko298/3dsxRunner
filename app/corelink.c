#include "corelink.h"

bool corelink_idle(CoreLink *c) {
  c->sync();
  bool idle = c->ctrl->ack_seq == c->ctrl->cmd_seq;
  if (idle) c->active_job = false;
  return idle;
}

static void cancel(CoreLink *c, uint32_t why) {
  if (!c->active_job) return;
  *c->cancel = why;
  c->sync();
}

bool corelink_wait(CoreLink *c, uint32_t ms) {
  uint32_t start = c->millis(), poll = start - 10u;
  while (!corelink_idle(c)) {
    uint32_t now = c->millis();

    if (!c->home_pending && now - poll >= 10u) {
      poll = now;
      if (c->home && c->home()) {
        c->home_pending = true;
        cancel(c, JOB_CANCEL_HOME);
      }
    }
    if (now - start >= ms) return false;
    if (c->loading_tick && !c->home_pending) c->loading_tick();
  }
  return true;
}

static void post_idle(CoreLink *c, uint32_t cmd, uint32_t a0, uint32_t a1) {
  c->ctrl->cmd = cmd;
  c->ctrl->arg0 = a0;
  c->ctrl->arg1 = a1;
  c->sync();
  c->ctrl->cmd_seq++;
  c->sync();
}

bool corelink_post(CoreLink *c, uint32_t cmd, uint32_t a0, uint32_t a1) {
  if (!corelink_wait(c, 2000)) return false;
  post_idle(c, cmd, a0, a1);
  return true;
}

/* Wait for the core to be idle BEFORE touching JobParams, even if the last
 * call timed out. Writing over a job that's still running is how you end up
 * debugging something that doesn't exist. */
bool corelink_run(CoreLink *c, uint32_t cmd) {
  if (!corelink_wait(c, 2000)) return false;
  c->job->cmd = cmd;
  c->job->done = 0;
  c->job->wall_us = c->micros ? c->micros() : 0;
  c->job->prev_done_us = c->last_done_us;
  c->last_done_us = 0;
  *c->cancel = c->home_pending ? JOB_CANCEL_HOME : JOB_CANCEL_NONE;
  c->timed_out = false;
  c->active_job = true;
  post_idle(c, AUDIO_CMD_RUN11, JOB_CODE, 0);
  /* If the ARM11 is truly wedged there's nothing an app can do to unwedge it.
   * Keep ownership and hope for a late answer; the alternative is the power
   * button. */
  if (!corelink_wait(c, 5000)) {
    c->timed_out = true;
    cancel(c, c->home_pending ? JOB_CANCEL_HOME : JOB_CANCEL_TIMEOUT);

    corelink_wait(c, 500);
  }
  bool ok = !c->active_job && !c->timed_out && c->job->done == cmd;
  if (ok && c->micros) c->last_done_us = c->micros();
  return ok;
}
