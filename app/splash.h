#ifndef RUNNER_SPLASH_H
#define RUNNER_SPLASH_H
#include <stdbool.h>
#include <stddef.h>

void splash_begin(bool installing);
void splash_progress(size_t done, size_t total);
void splash_preparing(void);
void splash_tick(void);
void splash_end(bool success);
#endif
