#ifndef RUNNER_TITLECFG_H
#define RUNNER_TITLECFG_H
#include <stdbool.h>
#include <stddef.h>
bool title_path(const char *target,const char *leaf,char out[256]);
bool title_prepare(const char *target);
bool title_plugin_read(const char *target,bool fallback);
bool title_plugin_write(const char *target,bool enabled);
#endif
