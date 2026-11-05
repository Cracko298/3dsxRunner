#include <aurora_app.h>
#include "apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static bool canonical(const char *p) {
  if (*p!='/') return false;
  for (;*p;p++) {
    if (*p=='\\' || *p==':' || (unsigned char)*p<32) return false;
    if (*p=='/' && (!p[1] || p[1]=='/' ||
        (p[1]=='.' && (p[2]=='/' || !p[2] || (p[2]=='.' && (p[3]=='/' || !p[3]))))))
      return false;
  }
  return true;
}
static bool inside(const char *path,const char *dir) {
  size_t n=strlen(dir);
  return !strncasecmp(path,dir,n) && path[n]=='/';
}
static bool regular(const char *path) {
  struct stat st;
  return !stat(path,&st) && S_ISREG(st.st_mode);
}
static bool directory(const char *path) {
  struct stat st;
  return !stat(path,&st) && S_ISDIR(st.st_mode);
}
static uint32_t le32(const unsigned char *p) {
  return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void inspect(InstalledApp *app,const char *root) {
  const char *base=strrchr(app->path,'/')+1;
  snprintf(app->title,sizeof app->title,"%s",base);
  char *dot=strrchr(app->title,'.'); if (dot) *dot=0;
  unsigned char h[36];
  FILE *f=fopen(app->path,"rb");
  if (!f) return;
  size_t got=fread(h,1,sizeof h,f);
  if (got>=4) {
    if (!memcmp(h,"AURC",4)) app->kind='C';
    else if (!memcmp(h,"AUR1",4)) app->kind='U';
    else if (!memcmp(h,"AOS1",4)) app->kind='A';
    else if (!memcmp(h,"3DSX",4)) app->kind='H';
    else if (!memcmp(h,"\177ELF",4)) app->kind='E';
    else if (!memcmp(h,"FIRM",4)) app->kind='F';
  }
  if (got==36 && (app->kind=='C'||app->kind=='A'||app->kind=='U')) {
    uint32_t off=le32(h+4);
    unsigned char magic[8];
    if (off<=app->size && app->size-off>=140 && !fseek(f,off+4,SEEK_SET) &&
        fread(magic,1,8,f)==8 && !memcmp(magic,"AURICON1",8) &&
        fread(app->icon,1,128,f)==128) app->has_icon=true;
  }
  fclose(f);
  app->linked=app->kind=='C' && menu_read(app->path,&app->info)==1;
  if (app->linked && app->info.title[0])
    snprintf(app->title,sizeof app->title,"%s",app->info.title);

  if (app->kind=='C') {
    if (snprintf(app->data,sizeof app->data,"%s/C/%s",root,base)>=(int)sizeof app->data)
      app->data[0]=0;
  } else if (app->kind=='A' || app->kind=='U') {
    snprintf(app->data,sizeof app->data,"%s",app->path);
  }
  dot=strrchr(app->data,'.');
  if (dot && dot>strrchr(app->data,'/')) *dot=0;
  else app->data[0]=0;
  char cdir[260]; snprintf(cdir,sizeof cdir,"%s/C",root);
  if (!canonical(app->data) || !inside(app->data,root) || !strcasecmp(app->data,cdir))
    app->data[0]=0;
}
static void scan_dir(AppCatalog *apps,const char *path,unsigned depth) {
  if (depth>8 || apps->count>=512) { apps->complete=false; return; }
  FsDir *d=fs_dir_open(path);
  if (!d) { apps->complete=false; return; }
  FsEntry e;
  while (fs_dir_read(d,&e)) {
    char full[256];
    if (snprintf(full,sizeof full,"%s/%s",path,e.name)>=(int)sizeof full || !canonical(full)) {
      apps->complete=false; continue;
    }
    if (e.is_dir) { scan_dir(apps,full,depth+1); continue; }
    const char *ext=strrchr(e.name,'.');
    if (!ext || (strcasecmp(ext,".bin") && strcasecmp(ext,".3dsx") &&
                 strcasecmp(ext,".elf") && strcasecmp(ext,".firm"))) continue;
    InstalledApp app={0};
    strcpy(app.path,full); app.size=e.size;
    inspect(&app,apps->root);

    char cdir[260]; snprintf(cdir,sizeof cdir,"%s/C",apps->root);
    if (!app.kind && (strcasecmp(ext,".bin") ||
        (strcasecmp(path,apps->root) && strcasecmp(path,cdir)))) continue;
    if (apps->count>=512) { apps->complete=false; break; }
    InstalledApp *grown=realloc(apps->items,(apps->count+1)*sizeof app);
    if (!grown) { apps->complete=false; break; }
    apps->items=grown;
    apps->items[apps->count++]=app;
  }
  fs_dir_close(d);
}
static int compare(const void *a,const void *b) {
  const InstalledApp *x=a,*y=b;
  int n=strcasecmp(x->title,y->title);
  return n?n:strcasecmp(x->path,y->path);
}
void apps_free(AppCatalog *apps) {
  free(apps->items); memset(apps,0,sizeof *apps);
}
void apps_scan(AppCatalog *apps,const char *root) {
  apps_free(apps);
  if (!canonical(root) || strlen(root)>=sizeof apps->root) return;
  strcpy(apps->root,root); apps->complete=true;
  scan_dir(apps,root,0);
  if (apps->count) qsort(apps->items,apps->count,sizeof *apps->items,compare);
}
bool apps_protected(const AppCatalog *apps,size_t i) {
  if(i>=apps->count) return false;
  const char *path=apps->items[i].path;
  const char *base=strrchr(path,'/'); base=base?base+1:path;
  const char *running=app_path();
  return !strcasecmp(base,"3dsxRunner.bin") ||
         (running && *running && !strcasecmp(path,running));
}
bool apps_data_allowed(const AppCatalog *apps,size_t i) {
  if (i>=apps->count || !apps->complete) return false;
  if (apps_protected(apps,i)) return false;
  const char *data=apps->items[i].data;
  if (!*data || !canonical(data) || !inside(data,apps->root) || !directory(data)) return false;
  for (size_t j=0;j<apps->count;j++) if (j!=i &&
      (!strcasecmp(data,apps->items[j].data) || inside(apps->items[j].path,data))) return false;
  return true;
}
static bool erase_tree(const char *path,unsigned depth) {
  if (depth>12) return false;
  if (!directory(path)) return regular(path) && !remove(path);
  FsDir *d=fs_dir_open(path);
  if (!d) return false;
  bool ok=true; FsEntry e;
  while (ok && fs_dir_read(d,&e)) {
    char child[256];
    ok=snprintf(child,sizeof child,"%s/%s",path,e.name)<(int)sizeof child &&
       canonical(child) && erase_tree(child,depth+1);
  }
  fs_dir_close(d);
  return ok && !rmdir(path);
}
int apps_uninstall(const AppCatalog *apps,size_t i,bool erase_data,char *note,size_t cap) {
  if (i>=apps->count) { snprintf(note,cap,"Select an application first."); return 0; }
  if (apps_protected(apps,i)) {
    snprintf(note,cap,"3dsxRunner is protected and cannot be uninstalled."); return 0;
  }
  const InstalledApp *app=&apps->items[i];
  if (!canonical(app->path) || !inside(app->path,apps->root) || !regular(app->path)) {
    snprintf(note,cap,"This app changed or is missing. Refresh the list."); return 0;
  }
  if (erase_data && !apps_data_allowed(apps,i)) {
    snprintf(note,cap,"Data is shared, unavailable or protected. Keep app data."); return 0;
  }
  if (remove(app->path)) {
    snprintf(note,cap,"Could not remove the application. Check SD access."); return 0;
  }
  if (erase_data && !erase_tree(app->data,0)) {
    snprintf(note,cap,"App removed. Some data could not be deleted."); return 1;
  }
  snprintf(note,cap,erase_data ? "Application and its private data removed." :
                               "Application removed. Saves and data kept.");
  return 1;
}
