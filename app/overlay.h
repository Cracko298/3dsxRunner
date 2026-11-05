#ifndef RUNNER_OVERLAY_H
#define RUNNER_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

bool runner_job(uint32_t cmd);
bool runner_overlay_frame(void);

void overlay_settings_load(void);

void overlay_game_start(const char *title, const char *path);
void overlay_game_end(void);
bool overlay_hotkey(uint32_t down, uint32_t held);
uint32_t overlay_mask(uint32_t held);
void overlay_open(void);

#endif
