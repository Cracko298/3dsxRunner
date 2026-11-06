#include <aurora_app.h>
#include "splash.h"
#include "glue.h"

static GfxImage *logo;
static bool active, installing, preparing, complete;
static uint32_t started, last_frame;
static size_t copied, total_bytes;

static void draw(unsigned shade) {
  uint32_t elapsed = sys_millis() - started;
  float p = elapsed < 650 ? elapsed / 650.0f : 1.0f;
  float t = p - 1.0f;
  float settle = 1.0f + 2.70158f*t*t*t + 1.70158f*t*t;
  float scale = 0.3f + settle*0.7f;
  uint32_t white = RGB(shade, shade, shade);
  uint32_t accent = RGB(shade/3, shade*4/5, shade);
  gfx_clear(SCREEN_TOP, COLOR_BLACK);
  gfx_clear(SCREEN_BOTTOM, COLOR_BLACK);
  if (logo) {
    for (int i = 0; i < logo->w*logo->h; i++)
      logo->pixels[i] = (logo->pixels[i] & 0xFFFFFFu) | (shade<<24);
    int w = (int)(logo->w*scale), h = (int)(logo->h*scale);
    gfx_image_scaled(SCREEN_TOP, logo, (400-w)/2,
                     96+(int)((1.0f-settle)*70), w, h);
  } else {
    gfx_print(SCREEN_TOP, (400-gfx_text_width(FONT_TITLE,"3dsxRunner"))/2,
              96, FONT_TITLE, white, "3dsxRunner");
  }
  const char *label = installing ? (complete ? "Added to Home Menu" : "Adding to Home Menu")
                                : (preparing ? "Preparing software" : "Loading software");
  gfx_print(SCREEN_BOTTOM, (320-gfx_text_width(FONT_REGULAR,label))/2,
            88, FONT_REGULAR, white, "%s", label);
  gfx_rect(SCREEN_BOTTOM, 48, 124, 224, 5, RGB(shade/6,shade/6,shade/6));
  if (total_bytes) {
    int width = (int)((uint64_t)copied*224/total_bytes);
    gfx_rect(SCREEN_BOTTOM, 48, 124, width, 5, accent);
  } else {
    unsigned phase = (elapsed/4)%368;
    int x = phase < 184 ? (int)phase : (int)(368-phase);
    gfx_rect(SCREEN_BOTTOM, 48+x, 124, 40, 5, accent);
  }
  if (complete) {
    gfx_line(SCREEN_BOTTOM, 151, 155, 158, 162, accent);
    gfx_line(SCREEN_BOTTOM, 158, 162, 170, 148, accent);
  } else {
    for (unsigned i=0; i<3; i++) {
      unsigned v = i == (elapsed/160)%3 ? shade : shade/4;
      gfx_rect(SCREEN_BOTTOM, 147+(int)i*11, 154, 5, 5, RGB(v,v,v));
    }
  }
  glue_loading_present();
}

void splash_begin(bool adding) {
  splash_end(false);
  glue_loading(true);
  active = true;
  installing = adding;
  preparing = complete = false;
  copied = total_bytes = 0;
  started = sys_millis();
  last_frame = started - 33u;
  int w = gfx_text_width(FONT_TITLE,"3dsxRunner"), h = gfx_line_height(FONT_TITLE);
  logo = gfx_image_new(w,h);
  if (logo) {
    gfx_clear(SCREEN_TOP,COLOR_BLACK);
    gfx_print(SCREEN_TOP,0,0,FONT_TITLE,COLOR_WHITE,"3dsxRunner");
    for (int y=0; y<h; y++) for (int x=0; x<w; x++)
      logo->pixels[y*w+x] = gfx_get_pixel(SCREEN_TOP,x,y);
  }
  splash_tick();
}

void splash_tick(void) {
  if (!active) return;
  uint32_t now = sys_millis();
  if (now-last_frame < 33u) return;
  last_frame = now;
  draw(255);
}

void splash_progress(size_t done, size_t total) {
  copied = done < total ? done : total;
  total_bytes = total;
  splash_tick();
}

void splash_preparing(void) {
  preparing = true;
  copied = total_bytes = 0;
  last_frame = sys_millis()-33u;
  splash_tick();
}

void splash_end(bool success) {
  if (!active) return;
  if (success) {
    complete = installing;
    copied = total_bytes = 1;
    uint32_t end = sys_millis();
    unsigned hold = installing ? 400 : 0;
    for (;;) {
      uint32_t elapsed = sys_millis()-end;
      if (elapsed >= hold+180) break;
      draw(elapsed < hold ? 255 : (hold+180-elapsed)*255/180);
      sys_sleep(16);
    }
  }
  gfx_image_free(logo);
  logo = NULL;
  active = false;
  gfx_clear(SCREEN_TOP,COLOR_BLACK);
  gfx_clear(SCREEN_BOTTOM,COLOR_BLACK);
  glue_loading_present();
  glue_loading(false);
}
