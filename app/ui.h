#ifndef RUNNER_UI_H
#define RUNNER_UI_H
#include "apps.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
enum { UI_MANAGER_BACK, UI_MANAGER_REMOVE, UI_MANAGER_EDIT };
bool ui_install_popup(MenuInfo *info,unsigned *fps,const uint16_t *color_icon,const unsigned char *mono_icon);
int ui_manager(const AppCatalog *apps,size_t *selection,const char *note);
bool ui_uninstall_popup(const AppCatalog *apps,size_t index,bool *erase_data);
void ui_working(const char *title);
void ui_settings(void);

typedef struct { uint32_t (*input)(void); void (*present)(void); } UiIo;
typedef struct {
  const char *title, *hint;
  const char *button[3];
  int buttons;
  size_t max_chars;
  unsigned valid;
  bool password, digits, dim_top, can_cancel;
} UiKeyboard;
int ui_keyboard_run(const UiKeyboard *k,char *value,size_t cap,const UiIo *io);
void ui_message_run(const char *title,const char *text,const UiIo *io);
#endif
