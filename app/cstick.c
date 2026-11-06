#include "cstick.h"
#include <stdbool.h>

#define I2C_BUS ((volatile uint8_t *)0x10144000)
#define DATA (I2C_BUS[0])
#define CNT  (I2C_BUS[1])
enum { ENABLE = 0x80, IRQ = 0x40, READ = 0x20, ACK = 0x10, START = 0x02, STOP = 0x01, ERROR = 0x04 };
#define DEVICE 0x54u

static int failures;

static bool wait(void) {
  for (uint32_t i = 0; i < 200000u; i++)
    if (!(CNT & ENABLE)) return true;
  return false;
}

static bool read_bytes(uint8_t *out, int n) {
  if (!wait()) return false;
  DATA = DEVICE | 1u;
  CNT = ENABLE | IRQ | START;
  if (!wait()) return false;
  if (!(CNT & ACK)) {
    CNT = ENABLE | IRQ | ERROR | STOP;
    wait();
    return false;
  }
  for (int i = 0; i < n; i++) {
    CNT = ENABLE | IRQ | READ | (i == n - 1 ? STOP : ACK);
    if (!wait()) return false;
    out[i] = DATA;
  }
  return true;
}

bool cstick_read(CStick *s) {
  s->x = s->y = 0;
  s->zl = s->zr = false;
  if (failures >= 8) return false;
  uint8_t b[7];
  if (!read_bytes(b, sizeof b) || (b[0] & 0x80u) == 0) {
    failures++;
    return false;
  }
  failures = 0;
  s->zl = (b[1] & 0x04u) != 0;
  s->zr = (b[1] & 0x02u) != 0;
  s->x = (int16_t)((int8_t)b[5] * 156 / 127);
  s->y = (int16_t)((int8_t)b[6] * 156 / 127);
  if (s->x > -8 && s->x < 8) s->x = 0;
  if (s->y > -8 && s->y < 8) s->y = 0;
  return true;
}
