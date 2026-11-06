#ifndef RUNNER_CSTICK_H
#define RUNNER_CSTICK_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { int16_t x, y; bool zl, zr; } CStick;
bool cstick_read(CStick *s);
#endif
