#ifndef RUNNER_GLUE_H
#define RUNNER_GLUE_H

#include <stdbool.h>
#include <stdint.h>

bool glue_gpu(void);
void glue_loading(bool active);
void glue_loading_present(void);
void glue_clear_display(void);
void glue_guest_display(bool owned);
bool glue_copy(uint32_t src, uint32_t dst, uint32_t bytes);
bool glue_font(uint32_t *head, uint32_t *glyphs, uint32_t *atlas);

void glue_overlay_begin(uint32_t top,uint32_t bottom);
#endif
