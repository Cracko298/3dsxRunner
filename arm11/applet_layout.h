#ifndef X3_APPLET_LAYOUT_H
#define X3_APPLET_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SR_OUTOFMEM = -3, SR_INVALID = -2, SR_NONE = -1, SR_D0_CLICK = 0, SR_D1_CLICK0, SR_D1_CLICK1,
  SR_D2_CLICK0, SR_D2_CLICK1, SR_D2_CLICK2, SR_HOME = 10, SR_RESET, SR_POWER,
  SR_PARENTAL_OK = 20, SR_PARENTAL_FAIL, SR_BANNED = 30
} SwkbdResult;

typedef struct {
  int type, num_buttons_m1, valid_input, password_mode, is_parental_screen, darken_top_screen;
  u32 filter_flags, save_state_flags;
  u16 max_text_len, dict_word_count, max_digits;
  u16 button_text[3][17];
  u16 numpad_keys[2];
  u16 hint_text[65];
  bool predictive_input, multiline, fixed_width, allow_home, allow_reset, allow_power, unknown,
      default_qwerty;
  bool button_submits_text[4];
  u16 language;
  int initial_text_offset, dict_offset, initial_status_offset, initial_learning_offset;
  u32 shared_memory_size;
  u32 version;
  SwkbdResult result;
  int status_offset, learning_offset, text_offset;
  u16 text_length;
  int callback_result;
  u16 callback_msg[257];
  bool skip_at_check;
  union { u8 reserved[171]; struct { void *a, *b, *c, *d, *e, *f; } extra; } u;
} Swkbd;
_Static_assert(sizeof(Swkbd) == 0x400 || sizeof(void *) != 4, "SwkbdState is 0x400 bytes, like the real applet's parameter");

typedef enum { ET_CODE = 0, ET_TEXT = 1, ET_MAX = 0x201 } ErrType;
typedef enum { ES_NORMAL = 0, ES_STEREO = 1 } ErrScreen;
typedef enum { ER_UNKNOWN = -1, ER_NONE = 0, ER_SUCCESS = 1, ER_POWER = 12 } ErrReturn;
typedef struct {
  ErrType type;
  int error_code;
  ErrScreen upper_screen;
  u16 language;
  u16 text[1900];
  bool home_button, software_reset, app_jump;
  ErrReturn return_code;
  u16 eula_version;
} ErrConf;

#endif
