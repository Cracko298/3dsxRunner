#ifndef RUNNER_SETTINGS_H
#define RUNNER_SETTINGS_H
#include <stdbool.h>

typedef struct {
  unsigned frame_lock;
  bool sync_gx;
  unsigned menu_volume;
  bool menu_music;
  bool remember_last;
  bool group_by_type;
  bool detailed_logs;
  char last[256];
} RunnerSettings;

extern RunnerSettings settings;
void settings_load(void);
void settings_save(void);
void settings_defaults(RunnerSettings *s);
#endif
