#include "titlecfg.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef TITLE_ROOT
#define TITLE_ROOT "/cheats/3dsxRunner"
#endif

bool title_path(const char *target,const char *leaf,char out[256]) {
  if(!target || !leaf) return false;
  if(!strncmp(target,"sdmc:",5)) target+=5;
  size_t len=strlen(target);
  if(target[0]!='/' || len<2 || len>=200 || target[len-1]=='/' ||
     strchr(target,'\\') || strchr(target,':') || strchr(leaf,'/')) return false;
  for(const char *p=target+1;*p;) {
    size_t n=strcspn(p,"/");
    if(!n || (n==1 && p[0]=='.') || (n==2 && p[0]=='.' && p[1]=='.')) return false;
    p+=n; if(*p) p++;
  }
  if(snprintf(out,256,"%s%s/%s",TITLE_ROOT,target,leaf)>=256) return false;
  for(char *p=out+strlen(TITLE_ROOT);*p;p++) if(*p>='A' && *p<='Z') *p+=32;
  return true;
}
bool title_prepare(const char *target) {
  char path[256];
  if(!title_path(target,"plugin.cfg",path)) return false;
  for(char *p=path+1;*p;p++) if(*p=='/') {
    *p=0;
    struct stat st;
    if(stat(path,&st)) {
      if(mkdir(path,0777)) { *p='/'; return false; }
    } else if(!S_ISDIR(st.st_mode)) { *p='/'; return false; }
    *p='/';
  }
  return true;
}
bool title_plugin_read(const char *target,bool fallback) {
  char path[256];
  if(!title_path(target,"plugin.cfg",path)) return fallback;
  FILE *f=fopen(path,"rb"); if(!f) return fallback;
  int c=fgetc(f),tail=fgetc(f); fclose(f);
  return tail==EOF && (c=='0'||c=='1') ? c=='1' : fallback;
}
bool title_plugin_write(const char *target,bool enabled) {
  char path[256],tmp[264],backup[264];
  if(!title_prepare(target) || !title_path(target,"plugin.cfg",path)) return false;
  snprintf(tmp,sizeof tmp,"%s.tmp",path); snprintf(backup,sizeof backup,"%s.bak",path);
  struct stat st; if(!stat(tmp,&st) || !stat(backup,&st)) return false;
  FILE *f=fopen(tmp,"wb"); if(!f) return false;
  bool ok=fputc(enabled?'1':'0',f)!=EOF;
  if(fclose(f)) ok=false;
  bool prior=!stat(path,&st);
  if(ok && prior && rename(path,backup)) ok=false;
  if(ok && rename(tmp,path)) { if(prior) rename(backup,path); ok=false; }
  if(ok && prior) remove(backup);
  if(!ok) remove(tmp);
  return ok;
}
