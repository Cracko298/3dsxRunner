#ifndef RUNNER_UI_AUDIO_H
#define RUNNER_UI_AUDIO_H
#include <stdbool.h>
enum { UI_MOVE, UI_SELECT, UI_BACK, UI_SUCCESS, UI_ERROR, UI_SOUND_COUNT };
void ui_audio_init(void);
void ui_audio_resume(void);
void ui_audio_suspend(void);
void ui_audio_toggle(void);
void ui_audio_refresh(void);
void ui_audio_tick(void);
bool ui_audio_enabled(void);
void ui_sound(int effect);
#endif
