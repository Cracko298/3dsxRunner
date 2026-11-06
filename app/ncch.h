#ifndef RUNNER_NCCH_H
#define RUNNER_NCCH_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  bool ok;
  uint64_t ncch, exefs, romfs;
  uint32_t unit, exefs_size;
  uint64_t title_id;
  uint8_t platform;
  uint8_t exheader[0x400];
} NcchInfo;

uint32_t ncch_stack_size(const NcchInfo *n);
uint32_t ncch_kernel_version(const NcchInfo *n);

uint32_t ncch_app_memory(const NcchInfo *n, bool new3ds);

int ncch_services(const NcchInfo *n, char names[34][9]);

bool ncch_open(FILE *f, NcchInfo *n, char *why, size_t cap);
bool ncch_exefs_file(FILE *f, const NcchInfo *n, const char *name, uint8_t **out, uint32_t *size);
bool ncch_smdh(FILE *f, const NcchInfo *n, uint8_t smdh[0x36C0]);
uint32_t ncch_romfs_level3(FILE *f, const NcchInfo *n);
uint8_t *ncch_build_3dsx(FILE *f, const NcchInfo *n, uint32_t *bytes, char *why, size_t cap);
uint8_t *ncch_build_packed(FILE *f, const NcchInfo *n, uint32_t *bytes, int *prm, char *why, size_t cap);
uint32_t ncch_code_size(const NcchInfo *n, bool *compressed);
uint8_t *blz_decompress(const uint8_t *in, uint32_t in_size, uint32_t *out_size);
#endif
