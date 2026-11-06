#include "menu.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

typedef struct {
  char magic[16];
  char target[200];
  uint32_t checksum;
} LegacyLink;
_Static_assert(sizeof(LegacyLink) == 220, "launcher trailer format");
static const char legacy_magic[16] = "3DSXR-LINK-v1";
static const char link_magic[16] = "3DSXR-LINK-v2";
typedef struct { char magic[16]; MenuInfo info; uint32_t checksum; } MenuLink;
_Static_assert(sizeof(MenuLink)==352,"v2 launcher trailer format");

static uint32_t hash_bytes(const void *data, size_t size) {
  const unsigned char *bytes = data;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

static uint32_t read32(const unsigned char *p) {
  return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}

static bool image_size(FILE *file, long *size) {
  unsigned char header[36];
  if (fseek(file, 0, SEEK_SET) || fread(header, 1, sizeof header, file) != sizeof header)
    return false;
  uint32_t payload = read32(header + 8);
  if (memcmp(header, "AURC", 4) || read32(header + 4) != 36 || payload < 140 || payload > 12u*1024*1024)
    return false;
  *size = 36 + payload;
  return true;
}

int menu_read(const char *path, MenuInfo *info) {
  memset(info,0,sizeof *info);
  FILE *file=fopen(path,"rb");
  if (!file) return -1;
  long base;
  int result=-1;
  if (image_size(file,&base) && !fseek(file,0,SEEK_END)) {
    long size=ftell(file);
    if (size==base) result=0;
    else if (size==base+(long)sizeof(LegacyLink) && !fseek(file,base,SEEK_SET)) {
      LegacyLink link;
      if (fread(&link,1,sizeof link,file)==sizeof link &&
          !memcmp(link.magic,legacy_magic,16) && memchr(link.target,0,200) &&
          link.target[0]=='/' && link.checksum==hash_bytes(link.target,200)) {
        memcpy(info->target,link.target,200);
        result=1;
      }
    } else if (size==base+(long)sizeof(MenuLink) && !fseek(file,base,SEEK_SET)) {
      MenuLink link;
      if (fread(&link,1,sizeof link,file)==sizeof link &&
          !memcmp(link.magic,link_magic,16) && memchr(link.info.target,0,200) &&
          link.info.target[0]=='/' && memchr(link.info.title,0,64) &&
          memchr(link.info.author,0,64) && !(link.info.flags&~7u) &&
          link.checksum==hash_bytes(&link.info,sizeof link.info)) {
        *info=link.info;
        result=1;
      }
    }
  }
  fclose(file);
  return result;
}

int menu_target(const char *path, char target[200]) {
  MenuInfo info;
  int result=menu_read(path,&info);
  if (result==1) memcpy(target,info.target,200);
  return result;
}

void menu_name(const MenuInfo *info, char name[59]) {
  char full[140];
  snprintf(full,sizeof full,"%s%s%s",info->title,
           (info->flags&MENU_CREDIT_IN_TITLE) && info->author[0] ? " - by " : "",
           (info->flags&MENU_CREDIT_IN_TITLE) ? info->author : "");
  unsigned n=0;
  for (const unsigned char *p=(const unsigned char *)full; *p && n<58; p++)
    if (*p>=32 && !strchr("<>:\"/\\|?*",*p)) name[n++]=*p;
  unsigned end=n;
  while (end && ((unsigned char)name[end-1]&0xC0)==0x80) end--;
  if (end && (unsigned char)name[end-1]>=0xC0) {
    unsigned char lead=(unsigned char)name[end-1];
    unsigned needed=lead<0xE0 ? 2 : lead<0xF0 ? 3 : 4;
    if (n-(end-1)<needed) n=end-1;
  }
  while (n && (name[n-1]==' ' || name[n-1]=='.')) n--;
  name[n]=0;
  if (!n) strcpy(name,"Homebrew");
}

static unsigned luminance(const uint16_t *source, int x, int y) {
  int m = (x&1) | (y&1)<<1 | (x&2)<<1 | (y&2)<<2 | (x&4)<<2 | (y&4)<<3;
  uint16_t c = source[((y/8)*6+x/8)*64+m];
  return ((c>>11)*255/31*299 + ((c>>5)&63)*255/63*587 + (c&31)*255/31*114)/1000;
}

static void convert_icon(const uint16_t *source, unsigned char output[128]) {
  int errors[2][34] = {{0}};
  memset(output, 0, 128);
  for (int y=0; y<32; y++) {
    int *row=errors[y&1], *next=errors[(y+1)&1];
    memset(next,0,sizeof errors[0]);
    int dir = y&1 ? -1 : 1;
    for (int step=0; step<32; step++) {
      int x = dir>0 ? step : 31-step;
      unsigned sum=0;
      for (int yy=0; yy<3; yy++) for (int xx=0; xx<3; xx++)
        sum += luminance(source,(x*3+xx)/2,(y*3+yy)/2);
      int value=(int)((sum+4)/9)+row[x+1]/16;
      if (value<0) value=0;
      if (value>255) value=255;
      bool on=value>=128;
      if (on) output[y*4+x/8] |= 0x80u>>(x%8);
      int error=value-(on?255:0);
      row[x+1+dir] += error*7;
      next[x+1-dir] += error*3;
      next[x+1] += error*5;
      next[x+1+dir] += error;
    }
  }
}

int menu_install_ex(const char *source, const char *directory, const MenuInfo *info,
                    const uint16_t *icon, const char *previous,
                    char *note, size_t capacity, MenuProgress progress) {
  if (!memchr(info->target,0,200) || !memchr(info->title,0,64) ||
      !memchr(info->author,0,64) || (info->flags&~7u)) {
    snprintf(note,capacity,"Invalid launcher details."); return 0;
  }
  const char *target=info->target, *title=info->title;
  char name[59], old_name[31], destination[320], temporary[340], backup[340], legacy[320], existing[200];
  menu_name(info,name);
  unsigned count = 0;
  for (const unsigned char *p=(const unsigned char *)title; *p && count<sizeof old_name-1; p++)
    if (isalnum(*p) || *p==' ' || *p=='-' || *p=='_') old_name[count++]=*p;
  while (count && old_name[count-1]==' ') count--;
  old_name[count]=0;
  if (!count) strcpy(old_name,"Homebrew");
  if (target[0]!='/' || strlen(target)>=sizeof existing) {
    snprintf(note,capacity,"The software's path is too long or invalid.");
    return 0;
  }
  bool replacing=false, found=false;
  for (unsigned n=1; n<=99; n++) {
    if (progress && !progress(0,0)) goto cancelled;
    int length = n==1 ? snprintf(destination,sizeof destination,"%s/%s.bin",directory,name)
                     : snprintf(destination,sizeof destination,"%s/%s (%u).bin",directory,name,n);
    if (length<0 || (size_t)length>=sizeof destination) break;
    FILE *check=fopen(destination,"rb");
    if (check) {
      fclose(check);
      if (menu_target(destination,existing)==1 && !strcmp(existing,target)) {
        replacing=found=true;
        break;
      }
    } else {
      if (errno!=ENOENT) break;
      found=true;
      break;
    }
  }
  if (!found) {
    snprintf(note,capacity,"Cannot find an available menu name on the SD card.");
    return 0;
  }
  snprintf(legacy,sizeof legacy,"%s/%s-%08lX.bin",directory,old_name,
           (unsigned long)hash_bytes(target,strlen(target)));
  bool old_link=menu_target(legacy,existing)==1 && !strcmp(existing,target);

  bool retire_previous=false;
  if (previous && previous[0] && strcasecmp(previous,destination)) {
    size_t d=strlen(directory);
    retire_previous=!strncmp(previous,directory,d) && previous[d]=='/' &&
      !strchr(previous+d+1,'/') && !strchr(previous+d+1,'\\') &&
      menu_target(previous,existing)==1 && !strcmp(existing,target);
    if (!retire_previous) {
      snprintf(note,capacity,"The existing launcher changed. Refresh and try again.");
      return 0;
    }
  }
  snprintf(temporary,sizeof temporary,"%s.part",destination);
  snprintf(backup,sizeof backup,"%s.backup",destination);
  if (replacing) {
    FILE *check=fopen(backup,"rb");
    if (check) fclose(check);
    if (check || errno!=ENOENT) {
      snprintf(note,capacity,"A previous backup exists. Keep it safe before updating this tile.");
      return 0;
    }
  }
  FILE *input=fopen(source,"rb"), *output=NULL;
  bool success=false, cancelled=false;
  long remaining;
  size_t done=0, total=0;
  if (!input || !image_size(input,&remaining) || fseek(input,0,SEEK_SET)) goto finish;
  total=(size_t)remaining+sizeof(MenuLink);
  output=fopen(temporary,"wb");
  if (!output) goto finish;
  unsigned char buffer[4096];
  while (remaining) {
    if (progress && !progress(done,total)) { cancelled=true; goto finish; }
    size_t amount=remaining<(long)sizeof buffer ? (size_t)remaining : sizeof buffer;
    if (fread(buffer,1,amount,input)!=amount || fwrite(buffer,1,amount,output)!=amount) goto finish;
    remaining-=amount;
    done+=amount;
  }
  MenuLink link={0};
  memcpy(link.magic,link_magic,sizeof link.magic);
  strcpy(link.info.target,info->target);
  strcpy(link.info.title,info->title);
  strcpy(link.info.author,info->author);
  link.info.flags=info->flags;
  link.checksum=hash_bytes(&link.info,sizeof link.info);
  if (fwrite(&link,1,sizeof link,output)!=sizeof link) goto finish;
  if (icon || previous) {
    unsigned char pixels[128];
    if (icon) convert_icon(icon,pixels);
    else {

      if (menu_target(previous,existing)!=1 || strcmp(existing,target)) goto finish;
      FILE *art=fopen(previous,"rb");
      if (!art) goto finish;
      bool read_ok=!fseek(art,36+12,SEEK_SET) && fread(pixels,1,sizeof pixels,art)==sizeof pixels;
      fclose(art);
      if (!read_ok) goto finish;
    }
    if (fseek(output,36+12,SEEK_SET) || fwrite(pixels,1,sizeof pixels,output)!=sizeof pixels) goto finish;
  }
  if (progress && !progress(done,total)) { cancelled=true; goto finish; }
  success=true;
finish:
  if (input) fclose(input);
  if (output && fclose(output)) success=false;
  bool backed_up=false;
  if (success && replacing) {
    if (rename(destination,backup)) success=false;
    else backed_up=true;
  }
  if (success && rename(temporary,destination)) success=false;
  if (!success) {
    remove(temporary);
    if (backed_up && rename(backup,destination)) {
      snprintf(note,capacity,"Update failed. Your previous tile is preserved in its .backup file.");
      return 0;
    }
  } else {
    if (backed_up) remove(backup);
    if (retire_previous && remove(previous)) {
      snprintf(note,capacity,"Saved the new tile. Remove the previous tile from Aurora.");
      return 1;
    }
    if (old_link && (!retire_previous || strcasecmp(previous,legacy)) && remove(legacy)) {
      snprintf(note,capacity,"Saved the clean tile. Remove the old hashed tile from Aurora.");
      return 1;
    }
    if (progress) progress(total,total);
  }
  if (cancelled) goto cancelled;
  snprintf(note,capacity,success ? (replacing ? "Updated on Aurora menu. HOME to see it."
                                             : "Added to Aurora menu. HOME to see it.")
                                 : "Could not save the menu entry. Check SD space/access.");
  return success;
cancelled:
  snprintf(note,capacity,"Adding to Home Menu cancelled.");
  return 0;
}

int menu_install(const char *source, const char *directory, const char *target,
                 const char *title, const uint16_t *icon, char *note, size_t capacity,
                 MenuProgress progress) {
  MenuInfo info={0};
  if (strlen(target)>=sizeof info.target || strlen(title)>=sizeof info.title) {
    snprintf(note,capacity,"The path or title is too long."); return 0;
  }
  strcpy(info.target,target); strcpy(info.title,title);
  return menu_install_ex(source,directory,&info,icon,NULL,note,capacity,progress);
}
