#ifndef RUNNER_MENU_H
#define RUNNER_MENU_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define MENU_NO_ARGS 1u
#define MENU_CREDIT_IN_TITLE 2u
#define MENU_PLUGIN 4u
typedef struct {
  char target[200];
  char title[64];
  char author[64];
  uint32_t flags;
} MenuInfo;
int menu_read(const char *file, MenuInfo *info);
int menu_target(const char *file, char target[200]);
void menu_name(const MenuInfo *info, char name[59]);
typedef bool (*MenuProgress)(size_t done, size_t total);
int menu_install_ex(const char *source, const char *directory, const MenuInfo *info,
                    const uint16_t *icon, const char *previous,
                    char *note, size_t capacity, MenuProgress progress);
int menu_install(const char *source, const char *directory, const char *target,
                 const char *title, const uint16_t *icon, char *note, size_t capacity,
                 MenuProgress progress);
#endif
