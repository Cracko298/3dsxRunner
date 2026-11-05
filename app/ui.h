#ifndef RUNNER_UI_H
#define RUNNER_UI_H
#include "apps.h"
enum { UI_MANAGER_BACK, UI_MANAGER_REMOVE, UI_MANAGER_EDIT };
bool ui_install_popup(MenuInfo *info,const uint16_t *color_icon,const unsigned char *mono_icon);
int ui_manager(const AppCatalog *apps,size_t *selection,const char *note);
bool ui_uninstall_popup(const AppCatalog *apps,size_t index,bool *erase_data);
void ui_working(const char *title);
#endif
