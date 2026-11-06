#include <aurora_app.h>
#include "ui.h"
#include "ui_audio.h"
#include "settings.h"
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
  return hid_keys_down();
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
  gfx_print(SCREEN_TOP,18,42,FONT_SMALL,MUTED,"Choose how this software appears and starts.");
  gfx_round_rect(SCREEN_TOP,12,65,376,124,10,PANEL);
  icon(SCREEN_TOP,24,78,2,rgb,mono);
  fit(SCREEN_TOP,140,79,232,FONT_BOLD,COLOR_WHITE,info->title);
  char author[80]; snprintf(author,sizeof author,"By %s",info->author[0]?info->author:"unknown developer");
  fit(SCREEN_TOP,140,103,232,FONT_SMALL,MUTED,author);
  gfx_print(SCREEN_TOP,140,131,FONT_SMALL,TEAL,"Menu title preview");
  char name[59]; menu_name(info,name);
  fit(SCREEN_TOP,140,150,230,FONT_SMALL,COLOR_WHITE,name);
  gfx_print(SCREEN_TOP,18,201,FONT_SMALL,MUTED,"With arguments passes the software's file path.\nSome apps need this to locate their files.");
}
static void erase_char(char *s) {
  size_t n=strlen(s);
  if(!n) return;
  do { n--; } while(n && ((unsigned char)s[n]&0xc0)==0x80);
  s[n]=0;
}
static size_t utf8_chars(const char *s) {
  size_t n=0;
  for(;*s;s++) if(((unsigned char)*s&0xc0)!=0x80) n++;
  return n;
}

int ui_keyboard_run(const UiKeyboard *k,char *value,size_t cap,const UiIo *io) {
  static const char *keys[]={
    "qwertyuiopasdfghjkl-zxcvbnm_.'1234567890",
    "QWERTYUIOPASDFGHJKL-ZXCVBNM_.'1234567890",
    "!@#$%^&*()[]{}<>+=:;/?_~`|\\,-.1234567890",
    "1234567890"
  };
  int nb=k->buttons<1?1:k->buttons>3?3:k->buttons;
  int nact=3+nb, aw=(304-(nact-1)*3)/nact;
  int nkeys=k->digits?10:40, pages=k->digits?1:3;
  char draft[1024]; snprintf(draft,sizeof draft,"%s",value);
  if(cap>sizeof draft) cap=sizeof draft;
  int focus=0,page=k->digits?3:0;
  const char *error="";
  for(;;) {
    if(k->dim_top) {
      gfx_rect(SCREEN_TOP,0,0,400,30,PANEL);
      gfx_print(SCREEN_TOP,10,6,FONT_BOLD,COLOR_WHITE,"%s",k->title);
      if(k->hint && *k->hint) fit(SCREEN_TOP,10,36,380,FONT_SMALL,TEAL,k->hint);
    }
    gfx_clear(SCREEN_BOTTOM,BG);
    fit(SCREEN_BOTTOM,10,5,300,FONT_BOLD,COLOR_WHITE,k->hint && *k->hint && !k->dim_top?k->hint:k->title);
    gfx_round_rect(SCREEN_BOTTOM,8,27,304,32,6,PANEL);
    char shown_buf[1024];
    if(k->password) { size_t n=utf8_chars(draft); if(n>sizeof shown_buf-1) n=sizeof shown_buf-1; memset(shown_buf,'*',n); shown_buf[n]=0; }
    else snprintf(shown_buf,sizeof shown_buf,"%s",draft);
    const char *shown=shown_buf;
    while(*shown && gfx_text_width(FONT_REGULAR,shown)>282) {
      shown++; while(((unsigned char)*shown&0xc0)==0x80) shown++;
    }
    gfx_print(SCREEN_BOTTOM,15,35,FONT_REGULAR,COLOR_WHITE,"%s_",shown);
    if(*error) gfx_print(SCREEN_BOTTOM,10,64,FONT_SMALL,RED,"%s",error);
    else gfx_print(SCREEN_BOTTOM,10,64,FONT_SMALL,MUTED,k->digits?"X: erase   L: clear   START: done":"X: erase   L: clear   Y: case   START: done");
    for(int i=0;i<nkeys;i++) {
      char c[2]={keys[page][i],0};
      box(8+(i%10)*31,81+(i/10)*29,28,26,c,focus==i,false);
    }
    const char *labels[6]={"Case","Space","Erase",0,0,0};
    for(int i=0;i<nb;i++) labels[3+i]=k->button[i]&&*k->button[i]?k->button[i]:(nb==1?"OK":i==nb-1?"Done":"Cancel");
    for(int i=0;i<nact;i++) box(8+i*(aw+3),204,aw,29,labels[i],focus==40+i,false);
    io->present();
    uint32_t down=io->input();
    int press=-2;
    if(down&KEY_B) { if(nb>1) press=0; else if(k->can_cancel) { ui_sound(UI_BACK); return -1; } }
    if(down&KEY_START) press=nb-1;
    int before=focus;
    int last_key=nkeys-1, row_keys=nkeys>10?30:0;
    if(down&(KEY_LEFT|KEY_CPAD_LEFT)) focus=focus==0?40+nact-1:focus==40?last_key:focus-1;
    if(down&(KEY_RIGHT|KEY_CPAD_RIGHT)) focus=focus==last_key?40:focus==40+nact-1?0:focus+1;
    if(down&(KEY_UP|KEY_CPAD_UP)) focus=focus>=40?row_keys+(focus-40)*10/nact:focus>=10?focus-10:40+focus*nact/10;
    if(down&(KEY_DOWN|KEY_CPAD_DOWN)) focus=focus>=40?(focus-40)*10/nact:focus+10>last_key?40+(focus%10)*nact/10:focus+10;
    if(down&KEY_X) { erase_char(draft); ui_sound(UI_BACK); error=""; }
    if(down&KEY_L) { draft[0]=0; ui_sound(UI_BACK); error=""; }
    if((down&KEY_Y) && !k->digits) { page=(page+1)%pages; ui_sound(UI_MOVE); }
    bool activate=down&KEY_A;
    if(hid_touch_down()) {
      int x=hid_touch_x(),y=hid_touch_y();
      if(x>=8 && x<316 && y>=81 && y<197) {
        int col=(x-8)/31,row=(y-81)/29;
        if(col<10 && (x-8)%31<28 && (y-81)%29<26 && row*10+col<nkeys) { focus=row*10+col; activate=true; }
      } else if(x>=8 && x<312 && y>=204 && y<233) {
        int col=(x-8)/(aw+3);
        if(col<nact && (x-8)%(aw+3)<aw) { focus=40+col; activate=true; }
      }
    }
    if(focus!=before) ui_sound(UI_MOVE);
    if(activate) {
      size_t n=strlen(draft);
      error="";
      if(focus>=43) press=focus-43;
      else if(focus==40) { if(!k->digits) page=(page+1)%pages; ui_sound(UI_SELECT); }
      else if(focus==42) { erase_char(draft); ui_sound(UI_SELECT); }
      else if(n+1<cap && utf8_chars(draft)<k->max_chars) {
        draft[n]=focus==41?' ':keys[page][focus]; draft[n+1]=0; ui_sound(UI_SELECT);
      } else ui_sound(UI_ERROR);
    }
    if(press>=0) {
      bool confirm=press==nb-1;
      size_t len=utf8_chars(draft);
      bool blank=true; for(const char *c=draft;*c;c++) if(*c!=' ') blank=false;
      if(confirm && (k->valid==1||k->valid==2) && !len) error="Please enter something.";
      else if(confirm && (k->valid==2||k->valid==3) && len && blank) error="That's only spaces.";
      else if(confirm && k->valid==4 && len!=k->max_chars) error="Needs to be exactly the full length.";
      if(*error) { ui_sound(UI_ERROR); continue; }
      snprintf(value,cap,"%s",draft);
      ui_sound(confirm?UI_SELECT:UI_BACK);
      return press;
    }
  }
}

void ui_message_run(const char *title,const char *text,const UiIo *io) {
  for(;;) {
    gfx_rect(SCREEN_TOP,0,0,400,30,PANEL);
    gfx_print(SCREEN_TOP,10,6,FONT_BOLD,COLOR_WHITE,"%s",title);
    gfx_clear(SCREEN_BOTTOM,BG);
    gfx_print(SCREEN_BOTTOM,10,8,FONT_BOLD,RED,"%s",title);
    int y=34; const char *p=text;
    while(*p && y<190) {
      char line[64]; int n=0;
      while(p[n] && p[n]!='\n' && n<63) n++;
      int cut=n;
      if(p[n] && p[n]!='\n') for(int i=n;i>20;i--) if(p[i]==' ') { cut=i; break; }
      memcpy(line,p,(size_t)cut); line[cut]=0;
      fit(SCREEN_BOTTOM,10,y,300,FONT_SMALL,COLOR_WHITE,line);
      p+=cut; while(*p==' '||*p=='\n') p++;
      y+=14;
    }
    box(116,204,88,29,"OK",true,false);
    io->present();
    uint32_t down=io->input();
    if((down&(KEY_A|KEY_B|KEY_START)) || (hid_touch_down() && hid_touch_in(116,204,88,29))) { ui_sound(UI_SELECT); return; }
  }
}

static void menu_present(void) {}
static uint32_t menu_input(void) { return input(); }
static const UiIo menu_io={menu_input,menu_present};

static bool keyboard(char value[64],const char *label) {
  UiKeyboard k={.title=label,.max_chars=63,.buttons=2,.button={"Cancel","Done"},.can_cancel=true};
  return ui_keyboard_run(&k,value,64,&menu_io)==1;
}
static bool has_text(const char *s) {
  while(*s) if(!isspace((unsigned char)*s++)) return true;
  return false;
}
bool ui_install_popup(MenuInfo *info,unsigned *fps,const uint16_t *rgb,const unsigned char *mono) {
  MenuInfo draft=*info;
  unsigned lock=*fps;
  static const int order[]={0,1,2,3,4,7,5,6};
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
      box(12,178,166,23,draft.flags&MENU_PLUGIN?"Plugin / cheats: On":"Plugin / cheats: Off",focus==4,false);
      box(184,178,124,23,lock==60?"Frame lock: Off":lock==45?"Frame lock: 45":lock==30?"Frame lock: 30":lock==20?"Frame lock: 20":lock==15?"Frame lock: 15":"Frame lock: Default",focus==7,false);
      box(12,207,90,28,"Cancel",focus==6,false);
      box(116,207,192,28,"Add / update",focus==5,false);
      if(*error) gfx_print(SCREEN_TOP,18,223,FONT_SMALL,RED,"%s",error);
      redraw=false;
    }
    uint32_t down=input();
    if(down&KEY_B) { ui_sound(UI_BACK); return false; }
    int before=focus;
    int at=0; while(order[at]!=focus) at++;
    if(down&(KEY_UP|KEY_CPAD_UP)) focus=order[(at+7)%8];
    if(down&(KEY_DOWN|KEY_CPAD_DOWN)) focus=order[(at+1)%8];
    bool activate=down&KEY_A;
    if((down&(KEY_LEFT|KEY_RIGHT|KEY_CPAD_LEFT|KEY_CPAD_RIGHT)) && ((focus>=2 && focus<=4) || focus==7)) activate=true;
    if(hid_touch_down()) {
      const int xs[]={80,80,12,12,12,116,12,184},ys[]={39,91,126,152,178,207,207,178};
      const int ws[]={224,224,296,296,166,192,90,124},hs[]={30,30,23,23,23,28,28,23};
      for(int i=0;i<8;i++) if(hid_touch_in(xs[i],ys[i],ws[i],hs[i])) { focus=i; activate=true; }
    }
    if(before!=focus) { ui_sound(UI_MOVE); redraw=true; }
    if(activate) {
      redraw=true; error="";
      if(focus==0 || focus==1) keyboard(focus?draft.author:draft.title,focus?"Developer name":"Display name");
      else if(focus==2) { draft.flags^=MENU_NO_ARGS; ui_sound(UI_SELECT); }
      else if(focus==3) { draft.flags^=MENU_CREDIT_IN_TITLE; ui_sound(UI_SELECT); }
      else if(focus==4) { draft.flags^=MENU_PLUGIN; ui_sound(UI_SELECT); }
      else if(focus==7) { lock=lock==0?60:lock==60?45:lock==45?30:lock==30?20:lock==20?15:0; ui_sound(UI_SELECT); }
      else if(focus==6) { ui_sound(UI_BACK); return false; }
      else if(!has_text(draft.title)) { error="Enter a display name first."; focus=0; ui_sound(UI_ERROR); }
      else { *info=draft; *fps=lock; ui_sound(UI_SELECT); return true; }
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

enum { SET_LOCK, SET_GPU, SET_SOUNDS, SET_MUSIC, SET_VOLUME, SET_REMEMBER, SET_GROUP, SET_TRACE, SET_RESET, SET_COUNT };
static const char *const set_help[SET_COUNT]={
  "Shows every game's newest frame at a steady rate.\nThe game itself still runs at full speed. 30, 20 and 15\ndivide the screen's 60 Hz evenly; 45 can't, so it\nalternates. Each game's Y popup can override this.",
  "Asynchronous lets the game keep running while the\nGPU draws. Synchronous waits for the GPU every time,\nlike r14. Only switch if a game glitches or freezes.",
  "Menu music and sound effects in the runner.\nGames aren't affected.",
  "Just the background loop. Effects stay on.",
  "How loud the runner's own sounds are.\nGames aren't affected. Max can clip a little.",
  "Start with the cursor on the last software you ran.",
  "Sorts the list into folders by file type:\n3DSX, 3DS, CIA and CXI. B goes back out.",
  "Logs every file a game opens, reads and writes,\nevery thread it starts, and what each thread was\ndoing when it stopped. CIA logs go in the game's\nown folder, in Logs.",
  "Puts every setting above back to its default.\nPer-game choices from the Y popup are kept.",
};
static void settings_label(int i,char *out,size_t cap) {
  const RunnerSettings *s=&settings;
  switch(i) {
  case SET_LOCK:
    if(s->frame_lock==60) snprintf(out,cap,"Frame lock: Off");
    else snprintf(out,cap,"Frame lock: %u fps",s->frame_lock);
    break;
  case SET_GPU: snprintf(out,cap,"GPU commands: %s",s->sync_gx?"Synchronous":"Asynchronous"); break;
  case SET_SOUNDS: snprintf(out,cap,"Menu sounds: %s",ui_audio_enabled()?"On":"Off"); break;
  case SET_MUSIC: snprintf(out,cap,"Menu music: %s",s->menu_music?"On":"Off"); break;
  case SET_VOLUME: snprintf(out,cap,"Menu volume: %s",s->menu_volume==200?"Max (2x)":s->menu_volume==150?"Loud (1.5x)":"Normal"); break;
  case SET_REMEMBER: snprintf(out,cap,"Remember last software: %s",s->remember_last?"On":"Off"); break;
  case SET_GROUP: snprintf(out,cap,"Sort into folders: %s",s->group_by_type?"On":"Off"); break;
  case SET_TRACE: snprintf(out,cap,"Detailed logs: %s",s->detailed_logs?"On":"Off"); break;
  default: snprintf(out,cap,"Reset to defaults"); break;
  }
}
static void settings_change(int i,int dir) {
  RunnerSettings *s=&settings;
  switch(i) {
  case SET_LOCK: { static const unsigned v[]={60,45,30,20,15}; int k=0;
    while(k<4 && v[k]!=s->frame_lock) k++;
    s->frame_lock=v[(k+5+dir)%5]; break; }
  case SET_GPU: s->sync_gx=!s->sync_gx; break;
  case SET_SOUNDS: ui_audio_toggle(); break;
  case SET_MUSIC: s->menu_music=!s->menu_music; ui_audio_refresh(); break;
  case SET_VOLUME: { static const unsigned v[]={100,150,200}; int k=s->menu_volume==150?1:s->menu_volume==200?2:0;
    s->menu_volume=v[(k+3+dir)%3]; ui_audio_refresh(); break; }
  case SET_REMEMBER: s->remember_last=!s->remember_last; break;
  case SET_GROUP: s->group_by_type=!s->group_by_type; break;
  case SET_TRACE: s->detailed_logs=!s->detailed_logs; break;
  default: { char last[sizeof s->last]; memcpy(last,s->last,sizeof last);
    settings_defaults(s); memcpy(s->last,last,sizeof last); ui_audio_refresh(); break; }
  }
  settings_save();
}
void ui_settings(void) {
  int focus=0;
  bool redraw=true;
  for(;;) {
    if(redraw) {
      gfx_clear(SCREEN_TOP,BG);
      gfx_print(SCREEN_TOP,18,14,FONT_TITLE,COLOR_WHITE,"Settings");
      gfx_print(SCREEN_TOP,18,42,FONT_SMALL,MUTED,"Saved as you change them. B or SELECT: back");
      gfx_round_rect(SCREEN_TOP,12,65,376,124,10,PANEL);
      char label[64]; settings_label(focus,label,sizeof label);
      gfx_print(SCREEN_TOP,26,78,FONT_BOLD,TEAL,"%s",label);
      gfx_print(SCREEN_TOP,26,104,FONT_SMALL,COLOR_WHITE,"%s",set_help[focus]);
      gfx_print(SCREEN_TOP,18,201,FONT_SMALL,MUTED,"A or left/right: change.\nThe frame lock and GPU mode apply from the next launch.");
      gfx_clear(SCREEN_BOTTOM,BG);
      for(int i=0;i<SET_COUNT;i++) {
        settings_label(i,label,sizeof label);
        box(12,4+i*26,296,23,label,focus==i,i==SET_RESET);
      }
      redraw=false;
    }
    uint32_t down=input();
    if(down&(KEY_B|KEY_SELECT)) { ui_sound(UI_BACK); return; }
    int before=focus;
    if(down&(KEY_UP|KEY_CPAD_UP)) focus=(focus+SET_COUNT-1)%SET_COUNT;
    if(down&(KEY_DOWN|KEY_CPAD_DOWN)) focus=(focus+1)%SET_COUNT;
    int dir=0;
    if(down&(KEY_A|KEY_RIGHT|KEY_CPAD_RIGHT)) dir=1;
    if(down&(KEY_LEFT|KEY_CPAD_LEFT)) dir=-1;
    if(hid_touch_down())
      for(int i=0;i<SET_COUNT;i++) if(hid_touch_in(12,4+i*26,296,23)) { focus=i; dir=1; }
    if(before!=focus) { ui_sound(UI_MOVE); redraw=true; }
    if(dir) { settings_change(focus,dir); ui_sound(focus==SET_RESET?UI_SUCCESS:UI_SELECT); redraw=true; }
  }
}
