#include <stdarg.h>

#include "sup.h"

void *memcpy(void *d, const void *s, size_t n) {
  u8 *dp = d;
  const u8 *sp = s;
  if (!(((uintptr_t)dp | (uintptr_t)sp) & 3u)) {

    while (n >= 32) {
      u32 *dw = (u32 *)(void *)dp;
      const u32 *sw = (const u32 *)(const void *)sp;
      __builtin_prefetch(sp + 64);
      u32 a = sw[0], b = sw[1], c = sw[2], e = sw[3], f = sw[4], g = sw[5], h = sw[6], i = sw[7];
      dw[0] = a, dw[1] = b, dw[2] = c, dw[3] = e, dw[4] = f, dw[5] = g, dw[6] = h, dw[7] = i;
      dp += 32, sp += 32, n -= 32;
    }
    while (n >= 16) {
      u32 *dw = (u32 *)(void *)dp;
      const u32 *sw = (const u32 *)(const void *)sp;
      dw[0] = sw[0], dw[1] = sw[1], dw[2] = sw[2], dw[3] = sw[3];
      dp += 16, sp += 16, n -= 16;
    }
    while (n >= 4) {
      *(u32 *)(void *)dp = *(const u32 *)(const void *)sp;
      dp += 4, sp += 4, n -= 4;
    }
  }
  while (n--)
    *dp++ = *sp++;
  return d;
}

void *memmove(void *d, const void *s, size_t n) {
  u8 *dp = d;
  const u8 *sp = s;
  if (dp <= sp || dp >= sp + n)
    return memcpy(d, s, n);
  while (n--)
    dp[n] = sp[n];
  return d;
}

void *memset(void *d, int c, size_t n) {
  u8 *dp = d;
  u32 w = (u8)c * 0x01010101u;
  while (n && ((uintptr_t)dp & 3u))
    *dp++ = (u8)c, n--;
  while (n >= 16) {
    u32 *dw = (u32 *)(void *)dp;
    dw[0] = w, dw[1] = w, dw[2] = w, dw[3] = w;
    dp += 16, n -= 16;
  }
  while (n--)
    *dp++ = (u8)c;
  return d;
}

int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y)
      return *x - *y;
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

size_t strnlen(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n])
    n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return (u8)*a - (u8)*b;
}

char *strncpy(char *d, const char *s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; i++)
    d[i] = s[i];
  for (; i < n; i++)
    d[i] = 0;
  return d;
}

typedef struct {
  char *out;
  size_t cap, len;
} Sink;

static void put(Sink *k, char c) {
  if (k->len + 1 < k->cap)
    k->out[k->len] = c;
  k->len++;
}

static void put_num(Sink *k, u64 v, unsigned base, bool upper, bool neg, int width, bool zero,
                    bool left) {
  char tmp[24];
  int n = 0;
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    tmp[n++] = digits[v % base];
    v /= base;
  } while (v);
  int len = n + (neg ? 1 : 0);
  if (!left && !zero)
    for (; width > len; width--)
      put(k, ' ');
  if (neg)
    put(k, '-');
  if (!left && zero)
    for (; width > len; width--)
      put(k, '0');
  while (n)
    put(k, tmp[--n]);
  if (left)
    for (; width > len; width--)
      put(k, ' ');
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
  Sink k = {out, cap, 0};
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      put(&k, *fmt);
      continue;
    }
    fmt++;
    bool zero = false, left = false;
    for (; *fmt == '0' || *fmt == '-'; fmt++)
      *fmt == '0' ? (zero = true) : (left = true);
    int width = 0;
    for (; *fmt >= '0' && *fmt <= '9'; fmt++)
      width = width * 10 + (*fmt - '0');
    int longs = 0;
    for (; *fmt == 'l' || *fmt == 'z'; fmt++)
      longs += *fmt == 'l' ? 1 : 0;
    switch (*fmt) {
    case 'd':
    case 'i': {
      s64 v = longs >= 2 ? va_arg(ap, s64) : va_arg(ap, s32);
      put_num(&k, v < 0 ? (u64)-v : (u64)v, 10, false, v < 0, width, zero, left);
      break;
    }
    case 'u':
    case 'x':
    case 'X': {
      u64 v = longs >= 2 ? va_arg(ap, u64) : va_arg(ap, u32);
      put_num(&k, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, zero, left);
      break;
    }
    case 'p':
      put(&k, '0'), put(&k, 'x');
      put_num(&k, (u32)(uintptr_t)va_arg(ap, void *), 16, false, false, 8, true, false);
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int n = (int)strlen(s);
      if (!left)
        for (; width > n; width--)
          put(&k, ' ');
      while (*s)
        put(&k, *s++);
      if (left)
        for (; width > n; width--)
          put(&k, ' ');
      break;
    }
    case 'c':
      put(&k, (char)va_arg(ap, int));
      break;
    case '%':
      put(&k, '%');
      break;
    default:
      if (!*fmt)
        fmt--;
      break;
    }
  }
  if (cap)
    out[k.len < cap ? k.len : cap - 1] = 0;
  return (int)k.len;
}

int snprintf(char *out, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out, cap, fmt, ap);
  va_end(ap);
  return n;
}
