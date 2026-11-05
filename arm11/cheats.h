#ifndef X3_CHEATS_H
#define X3_CHEATS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool (*read)(uint32_t va, void *out, uint32_t n);
  bool (*write)(uint32_t va, const void *in, uint32_t n);
  uint32_t keys;
  uint32_t touch_x, touch_y;
  bool wrote;
} CheatEnv;

uint32_t cheat_run(const uint32_t *codes, uint32_t pairs, CheatEnv *env);

uint32_t cheat_run_budget(const uint32_t *codes,uint32_t pairs,CheatEnv *env,uint32_t *budget);
#endif
