#ifndef RUNNER_HTTPBRIDGE_H
#define RUNNER_HTTPBRIDGE_H
#include "job.h"
void httpbridge_do(JobHttp *h);
void httpbridge_drop(uint32_t mask);
#endif
