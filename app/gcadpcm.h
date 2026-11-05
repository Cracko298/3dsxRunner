#ifndef RUNNER_GCADPCM_H
#define RUNNER_GCADPCM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t rate, samples; } GcAdpcmInfo;
bool gcadpcm_info(const uint8_t *data,size_t bytes,GcAdpcmInfo *info);
bool gcadpcm_decode(const uint8_t *data,size_t bytes,int16_t *output,size_t capacity);
#endif
