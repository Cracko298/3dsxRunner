#ifndef RUNNER_APPS_H
#define RUNNER_APPS_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "menu.h"
typedef struct {
  char path[256], data[256], title[64];
  uint32_t size;
  char kind;
  bool linked, has_icon;
  unsigned char icon[128];
  MenuInfo info;
} InstalledApp;
typedef struct {
  InstalledApp *items;
  size_t count;
  bool complete;
  char root[256];
} AppCatalog;
void apps_scan(AppCatalog *apps, const char *root);
void apps_free(AppCatalog *apps);
bool apps_protected(const AppCatalog *apps, size_t index);
bool apps_data_allowed(const AppCatalog *apps, size_t index);
int apps_uninstall(const AppCatalog *apps, size_t index, bool erase_data,
                   char *note, size_t capacity);
#endif
