#include <aurora_app.h>
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RunnerSettings settings;

void settings_defaults(RunnerSettings *s) {
  memset(s,0,sizeof *s);
  s->frame_lock=60;
  s->menu_volume=150;
  s->menu_music=true;
  s->remember_last=true;
  s->group_by_type=true;
  s->detailed_logs=true;
}

static void settings_path(char out[320]) { snprintf(out,320,"%s/settings.cfg",app_dir()); }

void settings_load(void) {
  settings_defaults(&settings);
  char path[320],line[300];
  settings_path(path);
  FILE *f=fopen(path,"rb");
  if(!f) return;
  while(fgets(line,sizeof line,f)) {
    char *eq=strchr(line,'='); if(!eq) continue;
    *eq=0; char *value=eq+1;
    value[strcspn(value,"\r\n")]=0;
    unsigned n=(unsigned)atoi(value);
    if(!strcmp(line,"frame_lock") && (n==60 || n==45 || n==30 || n==20 || n==15)) settings.frame_lock=n;
    else if(!strcmp(line,"sync_gx")) settings.sync_gx=n!=0;
    else if(!strcmp(line,"menu_volume") && (n==100 || n==150 || n==200)) settings.menu_volume=n;
    else if(!strcmp(line,"menu_music")) settings.menu_music=n!=0;
    else if(!strcmp(line,"remember_last")) settings.remember_last=n!=0;
    else if(!strcmp(line,"group_by_type")) settings.group_by_type=n!=0;
    else if(!strcmp(line,"detailed_logs")) settings.detailed_logs=n!=0;
    else if(!strcmp(line,"last")) snprintf(settings.last,sizeof settings.last,"%s",value);
  }
  fclose(f);
}

void settings_save(void) {
  char path[320];
  settings_path(path);
  FILE *f=fopen(path,"wb");
  if(!f) return;
  fprintf(f,"frame_lock=%u\nsync_gx=%u\nmenu_volume=%u\nmenu_music=%u\nremember_last=%u\n"
          "group_by_type=%u\ndetailed_logs=%u\nlast=%s\n",
          settings.frame_lock,settings.sync_gx?1u:0u,settings.menu_volume,
          settings.menu_music?1u:0u,settings.remember_last?1u:0u,
          settings.group_by_type?1u:0u,settings.detailed_logs?1u:0u,
          settings.remember_last?settings.last:"");
  fclose(f);
}
