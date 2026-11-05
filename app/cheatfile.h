#ifndef RUNNER_CHEATFILE_H
#define RUNNER_CHEATFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char name[64];
  char note[200];
  uint32_t *codes;
  uint32_t lines;
  bool enabled;
} Cheat;

typedef struct {
  Cheat *c;
  int n, cap;
} CheatList;

void cheatfile_parse(const char *text, CheatList *out);
void cheatfile_free(CheatList *l);

uint32_t *cheatfile_program(const CheatList *l, const uint32_t *extra, uint32_t extra_lines, uint32_t *bytes,
                            int *index_of, int max_index);

#endif
