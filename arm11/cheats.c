#include "cheats.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* Cheats run once per vblank with a shared operation budget. A runaway loop
 * stops itself instead of freezing the game, which is more than I can say
 * for some of my own loops. */
uint32_t cheat_run_budget(const uint32_t *codes, uint32_t pairs, CheatEnv *env, uint32_t *budget) {
  u32 offset = 0, data = 0;
  u32 if_stack = 0, if_count = 0;
  int32_t loop_line = -1;
  u32 loop_count = 0, loop_stack = 0, loop_ifs = 0;

  for (u32 i = 0; i < pairs; i++) {
    if(!*budget) return i+1;
    --*budget;
    u32 a = codes[i * 2], b = codes[i * 2 + 1];
    if (!a && !b)
      return 0;
    bool skip = (if_stack & 1u) != 0;
    u32 type = a >> 28, sub = (a >> 24) & 0xFu, addr = (a & 0x0FFFFFFFu) + offset;

#define PUSH(cond)                                       \
  do {                                                   \
    if(if_count>=32) return i+1;                  \
    if_stack = (if_stack << 1) | ((!(cond) || skip) ? 1u : 0u); \
    if_count++;                                          \
  } while (0)

    switch (type) {
    case 0x0:
    case 0x1:
    case 0x2:
      if (!skip) {
        u32 n = type == 0 ? 4u : type == 1 ? 2u : 1u;
        u8 bytes[4] = {(u8)b, (u8)(b >> 8), (u8)(b >> 16), (u8)(b >> 24)};
        if (!env->write(addr, bytes, n))
          return i + 1;
        env->wrote = true;
      }
      break;
    case 0x3:
    case 0x4:
    case 0x5:
    case 0x6: {
      u8 m[4]={0};
      if (!skip && !env->read(addr, m, 4))
        return i + 1;
      u32 v = m[0] | (u32)m[1] << 8 | (u32)m[2] << 16 | (u32)m[3] << 24;
      PUSH(type == 3 ? v < b : type == 4 ? v > b : type == 5 ? v == b : v != b);
      break;
    }
    case 0x7:
    case 0x8:
    case 0x9:
    case 0xA: {
      u8 m[2]={0};
      if (!skip && !env->read(addr, m, 2))
        return i + 1;
      u16 mask = (u16)(b >> 16), want = (u16)b;
      u16 v = (u16)((m[0] | m[1] << 8) & ~mask);
      PUSH(type == 7 ? v < want : type == 8 ? v > want : type == 9 ? v == want : v != want);
      break;
    }
    case 0xB:
      if (!skip) {
        u8 m[4];
        if (!env->read(addr, m, 4))
          return i + 1;
        offset = m[0] | (u32)m[1] << 8 | (u32)m[2] << 16 | (u32)m[3] << 24;
      }
      break;
    case 0xC:
      if (sub != 0 || loop_line>=0)
        return i + 1;
      loop_line = (int32_t)i;
      loop_count = skip ? 1u : b;
      loop_stack = if_stack;
      loop_ifs = if_count;
      break;
    case 0xD:
      switch (sub) {
      case 0x0:
        if (loop_line >= 0 && if_count <= loop_ifs) {
          if (loop_count > 1) {
            loop_count--;
            i = (u32)loop_line;
            if_stack = loop_stack;
          } else {
            loop_line = -1;
          }
        } else if (if_count) {
          if_stack >>= 1;
          if_count--;
        }
        break;
      case 0x1:
        if (loop_line >= 0) {
          if (loop_count > 1) {
            loop_count--;
            i = (u32)loop_line;
            if_stack = loop_stack;
            if_count = loop_ifs;
          } else {
            loop_line = -1;
          }
        }
        break;
      case 0x2:
        if (loop_line >= 0 && loop_count > 1) {
          loop_count--;
          i = (u32)loop_line;
          if_stack = loop_stack;
          if_count = loop_ifs;
          break;
        }
        loop_line = -1;
        if_stack = if_count = 0;
        offset = data = 0;
        break;
      case 0x3:
        if (!skip)
          offset = b;
        break;
      case 0x4:
        if (!skip)
          data += b;
        break;
      case 0x5:
        if (!skip)
          data = b;
        break;
      case 0x6:
      case 0x7:
      case 0x8:
        if (!skip) {
          u32 n = sub == 6 ? 4u : sub == 7 ? 2u : 1u;
          u8 bytes[4] = {(u8)data, (u8)(data >> 8), (u8)(data >> 16), (u8)(data >> 24)};
          if (!env->write(b + offset, bytes, n))
            return i + 1;
          env->wrote = true;
          offset += n;
        }
        break;
      case 0x9:
      case 0xA:
      case 0xB:
        if (!skip) {
          u32 n = sub == 9 ? 4u : sub == 0xA ? 2u : 1u;
          u8 m[4] = {0};
          if (!env->read(b + offset, m, n))
            return i + 1;
          data = m[0] | (u32)m[1] << 8 | (u32)m[2] << 16 | (u32)m[3] << 24;
        }
        break;
      case 0xC:
        if (!skip)
          offset += b;
        break;
      case 0xD:
        PUSH(b == 0 || (env->keys & b) == b);
        break;
      case 0xE: {
        u32 hi = b >> 16, lo = b & 0xFFFFu, at = (a & 1u) ? env->touch_y : env->touch_x;
        if ((a & 0xFu) > 1u)
          return i + 1;
        PUSH(lo <= at && at <= hi);
        break;
      }
      default:
        return i + 1;
      }
      break;
    case 0xE: {
      u32 lines = b/8u+(b%8u!=0);
      if (lines>pairs-i-1u || b>0xFFFFFFFFu-addr)
        return i + 1;
      if (!skip) {
        if(b>*budget) return i+1;
        *budget-=b;
        for (u32 k = 0; k < b; k++) {
          u32 w = codes[(i + 1) * 2 + k / 4];
          u8 byte = (u8)(w >> ((k % 4) * 8));
          if (!env->write(addr + k, &byte, 1))
            return i + 1;
        }
        env->wrote = true;
      }
      i += lines;
      break;
    }
    default:
      return i + 1;
    }
#undef PUSH
  }
  return 0;
}

uint32_t cheat_run(const uint32_t *codes,uint32_t pairs,CheatEnv *env) {
  uint32_t budget=16384;
  return cheat_run_budget(codes,pairs,env,&budget);
}
