#ifndef RUNNER_FSBRIDGE_H
#define RUNNER_FSBRIDGE_H

#include "job.h"

void fsbridge_do(JobFs *f);
void fsbridge_drop(uint32_t files_mask, uint32_t dirs_mask);
void fsbridge_close_all(void);

#endif
