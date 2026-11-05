#include <aurora_app.h>
#include "ui.h"
#include "ui_audio.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define BG RGB(12,18,29)
#define PANEL RGB(25,35,49)
#define MUTED RGB(152,170,190)
#define TEAL RGB(100,232,200)
#define RED RGB(240,112,120)

static uint32_t input(void) {
  if (!app_loop()) return KEY_B;
  ui_audio_tick();
  uint32_t down=hid_keys_down();
  if (down&KEY_SELECT) { ui_audio_toggle(); down&=~KEY_SELECT; }
  return down;
}
static void fit(Screen screen,int x,int y,int width,GfxFont font,uint32_t color,const char *text) {
  char out[192]; snprintf(out,sizeof out,"%s",text);
  size_t n=strlen(out);
  bool shortened=false;
  while(n && gfx_text_width(font,out)>width-(shortened?gfx_text_width(font,"..."):0)) {
    do { n--; } while(n && ((unsigned char)out[n]&0xc0)==0x80);
    out[n]=0; shortened=true;
  }
  if(shortened && n+3<sizeof out) strcat(out,"...");
  gfx_print(screen,x,y,font,color,"%s",out);
}
static void icon(Screen screen,int x,int y,int scale,const uint16_t *rgb,const unsigned char *mono) {
  int side=48*scale;
  gfx_round_rect(screen,x-4,y-4,side+8,side+8,8,RGB(40,55,72));
  if (rgb) {
    for(int yy=0;yy<48;yy++) for(int xx=0;xx<48;xx++) {
      int m=(xx&1)|(yy&1)<<1|(xx&2)<<1|(yy&2)<<2|(xx&4)<<2|(yy&4)<<3;
      uint16_t c=rgb[((yy/8)*6+xx/8)*64+m];
      gfx_rect(screen,x+xx*scale,y+yy*scale,scale,scale,
               RGB((c>>11)*255/31,((c>>5)&63)*255/63,(c&31)*255/31));
    }
  } else if(mono) {
    for(int yy=0;yy<side;yy++) for(int xx=0;xx<side;xx++) {
      int sx=xx*32/side,sy=yy*32/side;
      if(mono[sy*4+sx/8]&(0x80>>(sx%8))) gfx_pixel(screen,x+xx,y+yy,TEAL);
    }
  } else {
    gfx_print(screen,x+side/2-16,y+side/2-8,FONT_BOLD,TEAL,"3DX");
  }
}
static void box(int x,int y,int w,int h,const char *text,bool selected,bool danger) {
  uint32_t a=danger?RED:TEAL;
  gfx_round_rect(SCREEN_BOTTOM,x,y,w,h,6,selected?a:PANEL);
  fit(SCREEN_BOTTOM,x+9,y+7,w-18,FONT_SMALL,selected?BG:COLOR_WHITE,text);
}
static void preview(const MenuInfo *info,const uint16_t *rgb,const unsigned char *mono) {
  gfx_clear(SCREEN_TOP,BG);
  gfx_print(SCREEN_TOP,18,14,FONT_TITLE,COLOR_WHITE,"Add to Home Menu");
  gfx_print(SCREEN_TOP,18,42,FONT_SMALL,MUTED,"Choose how this homebrew appears and starts.");
  gfx_round_rect(SCREEN_TOP,12,65,376,124,10,PANEL);
  icon(SCREEN_TOP,24,78,2,rgb,mono);
  fit(SCREEN_TOP,140,79,232,FONT_BOLD,COLOR_WHITE,info->title);
  char author[80]; snprintf(author,sizeof author,"By %s",info->author[0]?info->author:"unknown developer");
  fit(SCREEN_TOP,140,103,232,FONT_SMALL,MUTED,author);
  gfx_print(SCREEN_TOP,140,131,FONT_SMALL,TEAL,"Menu title preview");
  char name[59]; menu_name(info,name);
  fit(SCREEN_TOP,140,150,230,FONT_SMALL,COLOR_WHITE,name);
  gfx_print(SCREEN_TOP,18,201,FONT_SMALL,MUTED,"With arguments passes the homebrew's file path.\nSome apps need this to locate their files.");
}
static void erase_char(char *s) {
  size_t n=strlen(s);
  if(!n) return;
  do { n--; } while(n && ((unsigned char)s[n]&0xc0)==0x80);
  s[n]=0;
}
static bool keyboard(char value[64],const char *label) {
  static const char *keys[]={
    "qwertyuiopasdfghjkl-zxcvbnm_.'1234567890",
    "QWERTYUIOPASDFGHJKL-ZXCVBNM_.'1234567890",
    "!@#$%^&*()[]{}<>+=:;/?_~`|\\,-.1234567890"
  };
  char draft[64]; strcpy(draft,value);
  int focus=0,page=0;
  bool redraw=true;
  for(;;) {
    if(redraw) {
      gfx_clear(SCREEN_BOTTOM,BG);
      gfx_print(SCREEN_BOTTOM,10,5,FONT_BOLD,COLOR_WHITE,"%s",label);
      gfx_round_rect(SCREEN_BOTTOM,8,27,304,32,6,PANEL);
      const char *shown=draft;
      while(*shown && gfx_text_width(FONT_REGULAR,shown)>282) {
        shown++; while(((unsigned char)*shown&0xc0)==0x80) shown++;
      }
      gfx_print(SCREEN_BOTTOM,15,35,FONT_REGULAR,COLOR_WHITE,"%s_",shown);
      gfx_print(SCREEN_BOTTOM,10,64,FONT_SMALL,MUTED,"X: erase   L: clear   Y: case   START: done");
      for(int k=0;k<40;k++) {
        char c[2]={keys[page][k],0};
        box(8+(k%10)*31,81+(k/10)*29,28,26,c,focus==k,false);
      }
      const char *actions[]={"Case","Space","Erase","Cancel","Done"};
      for(int k=0;k<5;k++) box(8+k*61,204,58,29,actions[k],focus==40+k,false);
      redraw=false;
    }
    uint32_t down=input();
    if(down&KEY_B) { ui_sound(UI_BACK); return false; }
    if(down&KEY_START) { strcpy(value,draft); ui_sound(UI_SELECT); return true; }
    int before=focus;
    if(down&(KEY_LEFT|KEY_CPAD_LEFT)) focus=focus==0?44:focus-1;
    if(down&(KEY_RIGHT|KEY_CPAD_RIGHT)) focus=(focus+1)%45;
    if(down&(KEY_UP|KEY_CPAD_UP)) focus=focus>=40?30+(focus-40)*2:focus>=10?focus-10:40+focus/2;
    if(down&(KEY_DOWN|KEY_CPAD_DOWN)) focus=focus>=40?(focus-40)*2:focus>=30?40+(focus%10)/2:focus+10;
    if(down&KEY_X) { erase_char(draft); ui_sound(UI_BACK); redraw=true; }
    if(down&KEY_L) { draft[0]=0; ui_sound(UI_BACK); redraw=true; }
    if(down&KEY_Y) { page=(page+1)%3; ui_sound(UI_MOVE); redraw=true; }
    bool activate=down&KEY_A;
    if(hid_touch_down()) {
      int x=hid_touch_x(),y=hid_touch_y();
      if(x>=8 && x<316 && y>=81 && y<197) {
        int col=(x-8)/31,row=(y-81)/29;
        if(col<10 && (x-8)%31<28 && (y-81)%29<26) { focus=row*10+col; activate=true; }
      } else if(x>=8 && x<310 && y>=204 && y<233) {
        int col=(x-8)/61;
        if(col<5 && (x-8)%61<58) { focus=40+col; activate=true; }
      }
    }
    if(focus!=before) { ui_sound(UI_MOVE); redraw=true; }
    if(activate) {
      size_t n=strlen(draft);
      if(focus==43) { ui_sound(UI_BACK); return false; }
      if(focus==44) { strcpy(value,draft); ui_sound(UI_SELECT); return true; }
      if(focus==40) page=(page+1)%3;
      else if(focus==42) erase_char(draft);
      else if(n<63) { draft[n]=focus==41?' ':keys[page][focus]; draft[n+1]=0; }
      else ui_sound(UI_ERROR);
      ui_sound(UI_SELECT); redraw=true;
    }
  }
}
static bool has_text(const char *s) {
  while(*s) if(!isspace((unsigned char)*s++)) return true;
  return false;
}
bool ui_install_popup(MenuInfo *info,const uint16_t *rgb,const unsigned char *mono) {
  MenuInfo draft=*info;
  int focus=5;
  bool redraw=true;
  const char *error="";
  for(;;) {
    if(redraw) {
      preview(&draft,rgb,mono);
      gfx_clear(SCREEN_BOTTOM,BG);
      gfx_print(SCREEN_BOTTOM,12,6,FONT_BOLD,COLOR_WHITE,"Launcher details");
      icon(SCREEN_BOTTOM,18,49,1,rgb,mono);
      gfx_print(SCREEN_BOTTOM,84,25,FONT_SMALL,MUTED,"Display name");
      box(80,39,224,30,draft.title,focus==0,false);
      gfx_print(SCREEN_BOTTOM,84,77,FONT_SMALL,MUTED,"Developer");
      box(80,91,224,30,draft.author[0]?draft.author:"Not supplied - tap to edit",focus==1,false);
      box(12,126,296,23,draft.flags&MENU_NO_ARGS?"Launch: without arguments":"Launch: with path argument",focus==2,false);
      box(12,152,296,23,draft.flags&MENU_CREDIT_IN_TITLE?"Credit in menu title: Yes":"Credit in menu title: No",focus==3,false);
      box(12,178,296,23,draft.flags&MENU_PLUGIN?"Plugin / cheats: On":"Plugin / cheats: Off",focus==4,false);
      box(12,207,90,28,"Cancel",focus==6,false);
      box(116,207,192,28,"Add / update",focus==5,false);
      if(*error) gfx_print(SCREEN_TOP,18,223,FONT_SMALL,RED,"%s",error);
      redraw=false;
    }
    uint32_t down=input();
    if(down&KEY_B) { ui_sound(UI_BACK); return false; }
    int before=focus;
    if(down&(KEY_UP|KEY_CPAD_UP)) focus=(focus+6)%7;
    if(down&(KEY_DOWN|KEY_CPAD_DOWN)) focus=(focus+1)%7;
    bool activate=down&KEY_A;
    if((down&(KEY_LEFT|KEY_RIGHT|KEY_CPAD_LEFT|KEY_CPAD_RIGHT)) && (focus>=2 && focus<=4)) activate=true;
    if(hid_touch_down()) {
      const int xs[]={80,80,12,12,12,116,12},ys[]={39,91,126,152,178,207,207};
      const int ws[]={224,224,296,296,296,192,90},hs[]={30,30,23,23,23,28,28};
      for(int i=0;i<7;i++) if(hid_touch_in(xs[i],ys[i],ws[i],hs[i])) { focus=i; activate=true; }
    }
    if(before!=focus) { ui_sound(UI_MOVE); redraw=true; }
    if(activate) {
      redraw=true; error="";
      if(focus==0 || focus==1) keyboard(focus?draft.author:draft.title,focus?"Developer name":"Display name");
      else if(focus==2) { draft.flags^=MENU_NO_ARGS; ui_sound(UI_SELECT); }
      else if(focus==3) { draft.flags^=MENU_CREDIT_IN_TITLE; ui_sound(UI_SELECT); }
      else if(focus==4) { draft.flags^=MENU_PLUGIN; ui_sound(UI_SELECT); }
      else if(focus==6) { ui_sound(UI_BACK); return false; }
      else if(!has_text(draft.title)) { error="Enter a display name first."; focus=0; ui_sound(UI_ERROR); }
      else { *info=draft; ui_sound(UI_SELECT); return true; }
    }
  }
}
static void app_details(const InstalledApp *app,const char *note) {
  gfx_clear(SCREEN_TOP,BG);
  gfx_print(SCREEN_TOP,16,12,FONT_TITLE,COLOR_WHITE,"Installed applications");
  if(app) {
    icon(SCREEN_TOP,24,62,2,NULL,app->has_icon?app->icon:NULL);
    fit(SCREEN_TOP,140,62,240,FONT_BOLD,COLOR_WHITE,app->title);
    fit(SCREEN_TOP,140,88,240,FONT_SMALL,MUTED,app->linked && app->info.author[0]?app->info.author:
        app->kind=='C'?"C SDK application":app->kind=='U'?"Auric application":"SD application");
    gfx_print(SCREEN_TOP,140,112,FONT_SMALL,TEAL,"%lu KB%s",(unsigned long)(app->size/1024),app->linked?" / 3DSX launcher":"");

    const char *path=app->path;
    for(int line=0;*path && line<6;line++) {
      size_t n=strlen(path); if(n>46) n=46;
      while(n && ((unsigned char)path[n]&0xc0)==0x80) n--;
      if(!n) n=1;
      char part[47]; memcpy(part,path,n); part[n]=0;
      gfx_print(SCREEN_TOP,16,166+line*9,FONT_MONO,MUTED,"%s",part);
      path+=n;
    }
    if(app->linked) gfx_print(SCREEN_TOP,140,139,FONT_SMALL,MUTED,"%s",app->info.flags&MENU_NO_ARGS?"No arguments":"With path argument");
  } else gfx_print(SCREEN_TOP,16,76,FONT_REGULAR,MUTED,"No applications found in /Aurora/Apps.");
  if(note && *note) fit(SCREEN_TOP,16,224,368,FONT_SMALL,TEAL,note);
}
int ui_manager(const AppCatalog *apps,size_t *selected,const char *note) {
  if(*selected>=apps->count) *selected=apps->count?apps->count-1:0;
  size_t scroll=*selected/7*7;
  bool redraw=true;
  for(;;) {
    if(redraw) {
      if(*selected<scroll) scroll=*selected;
      if(*selected>=scroll+7) scroll=*selected-6;
      app_details(apps->count?&apps->items[*selected]:NULL,note);
      gfx_clear(SCREEN_BOTTOM,BG);
      for(size_t i=0;i<7 && scroll+i<apps->count;i++)
        box(8,8+(int)i*27,304,25,apps->items[scroll+i].title,scroll+i==*selected,false);
      box(8,207,74,29,"B: Back",false,false);
      box(88,207,98,29,"Y: Edit",false,false);
      bool protected=apps->count && apps_protected(apps,*selected);
      box(192,207,120,29,protected?"Protected":"A: Uninstall",false,!protected);
      if(!apps->complete) gfx_print(SCREEN_BOTTOM,10,196,FONT_SMALL,RED,"Some folders could not be scanned.");
      redraw=false;
    }
    uint32_t down=input();
    if(down&KEY_B) { ui_sound(UI_BACK); return UI_MANAGER_BACK; }
    if(hid_touch_down() && hid_touch_in(8,207,74,29)) { ui_sound(UI_BACK); return UI_MANAGER_BACK; }
    if(!apps->count) continue;
    size_t before=*selected;
    if(down&(KEY_DOWN|KEY_CPAD_DOWN)) *selected=(*selected+1)%apps->count;
    if(down&(KEY_UP|KEY_CPAD_UP)) *selected=(*selected+apps->count-1)%apps->count;
    if(down&(KEY_RIGHT|KEY_R)) *selected=*selected+7<apps->count?*selected+7:apps->count-1;
    if(down&(KEY_LEFT|KEY_L)) *selected=*selected>=7?*selected-7:0;
    if(hid_touch_down()) {
      if(hid_touch_in(8,8,304,189)) {
        size_t row=(hid_touch_y()-8)/27;
        if(scroll+row<apps->count) *selected=scroll+row;
      }
      if(hid_touch_in(88,207,98,29)) down|=KEY_Y;
      if(hid_touch_in(192,207,120,29)) down|=KEY_A;
    }
    if(before!=*selected) { ui_sound(UI_MOVE); redraw=true; }
    if(down&KEY_A) {
      if(apps_protected(apps,*selected)) {
        note="3dsxRunner is protected and cannot be uninstalled.";
        ui_sound(UI_ERROR); redraw=true;
      } else { ui_sound(UI_SELECT); return UI_MANAGER_REMOVE; }
    }
    if(down&KEY_Y) {
      if(apps->items[*selected].linked) { ui_sound(UI_SELECT); return UI_MANAGER_EDIT; }
      note="Edit applies to 3DSX launcher titles and credits.";
      ui_sound(UI_ERROR); redraw=true;
    }
  }
}
bool ui_uninstall_popup(const AppCatalog *apps,size_t index,bool *erase_data) {
  if(apps_protected(apps,index)) { ui_sound(UI_ERROR); return false; }
  const InstalledApp *app=&apps->items[index];
  bool allowed=apps_data_allowed(apps,index),erase=false,redraw=true;
  int focus=0;
  for(;;) {
    if(redraw) {
      app_details(app,"Review the app path above before removing it.");
      gfx_clear(SCREEN_BOTTOM,BG);
      gfx_print(SCREEN_BOTTOM,16,15,FONT_TITLE,COLOR_WHITE,"Uninstall application?");
      gfx_print(SCREEN_BOTTOM,16,56,FONT_SMALL,MUTED,"Deletes this application file and its menu tile.");
      if(app->linked) gfx_print(SCREEN_BOTTOM,16,80,FONT_SMALL,MUTED,"The original .3dsx and its assets stay in place.");
      box(12,124,296,34,!allowed?"Keep data (shared or unavailable)":erase?"App data: delete too":"App data: keep saves and settings",focus==1,false);
      box(12,198,120,32,"Cancel",focus==0,false);
      box(148,198,160,32,"Uninstall",focus==2,true);
      redraw=false;
    }
    uint32_t down=input();
    if(down&KEY_B) { ui_sound(UI_BACK); return false; }
    int before=focus;
    if(down&(KEY_UP|KEY_LEFT|KEY_CPAD_UP|KEY_CPAD_LEFT)) focus=(focus+2)%3;
    if(down&(KEY_DOWN|KEY_RIGHT|KEY_CPAD_DOWN|KEY_CPAD_RIGHT)) focus=(focus+1)%3;
    bool activate=down&KEY_A;
    if(hid_touch_down()) {
      if(hid_touch_in(12,124,296,34)) { focus=1; activate=true; }
      if(hid_touch_in(12,198,120,32)) { focus=0; activate=true; }
      if(hid_touch_in(148,198,160,32)) { focus=2; activate=true; }
    }
    if(before!=focus) { ui_sound(UI_MOVE); redraw=true; }
    if(activate) {
      if(focus==0) { ui_sound(UI_BACK); return false; }
      if(focus==1) { if(allowed) erase=!erase; ui_sound(allowed?UI_SELECT:UI_ERROR); redraw=true; }
      if(focus==2) { *erase_data=erase; ui_sound(UI_SELECT); return true; }
    }
  }
}
void ui_working(const char *title) {
  gfx_clear(SCREEN_BOTTOM,BG);
  gfx_print(SCREEN_BOTTOM,16,100,FONT_REGULAR,COLOR_WHITE,"%s",title);
  gfx_present();
}
