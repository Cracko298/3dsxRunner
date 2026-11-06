#ifndef X3_APPLET_H
#define X3_APPLET_H
#include "sup.h"
void applet_reset(void);
void applet_launch_wakeup(void);
void applet_set_resume_event(Object *ev);
bool applet_is_library(u32 appid);
void applet_send(u32 dest, u32 command);
bool applet_take(bool remove, u32 *sender, u32 *command, const u8 **data, u32 *size);
bool applet_start(Thread *t, u32 appid, const void *data, u32 size, Object *mem);
#endif
