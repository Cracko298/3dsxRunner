#include <aurora_app.h>
#include "audio.h"
#include "gcadpcm.h"
#include "ui_audio.h"
#include <stdio.h>

extern void app_core_post(uint32_t cmd,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3);
extern void os_cache_sync(void);
extern void ui_music_gain11(uint32_t gain);
#define ASSET(name) extern const uint8_t ui_##name[],ui_##name##_end[]
ASSET(menu);ASSET(move);ASSET(select);ASSET(back);ASSET(success);ASSET(error);
#undef ASSET
static const struct { const uint8_t *start,*end; } compressed[]={
  {ui_menu,ui_menu_end},{ui_move,ui_move_end},{ui_select,ui_select_end},
  {ui_back,ui_back_end},{ui_success,ui_success_end},{ui_error,ui_error_end}
};
static struct { uint32_t address,samples,rate; } decoded[UI_SOUND_COUNT+1];
static bool enabled=true,loaded;
enum { MUSIC_VOLUME=60, DUCK_VOLUME=18, RELEASE_MS=220 };
static uint32_t duck_until;
static unsigned music_volume;
static bool ducked;

static void music_gain(unsigned volume) {
  if(volume==music_volume) return;
  music_volume=volume;
  uint32_t gain=volume*0x8000u/100u;

  app_core_post(AUDIO_CMD_RUN11,(uint32_t)(uintptr_t)ui_music_gain11,
                gain|(gain<<16),0,0);
}
void ui_audio_tick(void) {
  if(!loaded || !ducked) return;
  int32_t elapsed=(int32_t)(sys_millis()-duck_until);
  if(elapsed<=0) return;
  if(elapsed>=RELEASE_MS) {
    ducked=false; music_gain(MUSIC_VOLUME);
  } else music_gain(DUCK_VOLUME+(MUSIC_VOLUME-DUCK_VOLUME)*(unsigned)elapsed/RELEASE_MS);
}

static void play(unsigned index,bool loop,int volume) {
  if(!loaded || index>=UI_SOUND_COUNT+1) return;

  uint32_t voice=loop?AUDIO_VOICE_LOOP:(index==UI_MOVE+1?1u:2u);
  uint32_t flags=voice|AUDIO_VOICE_PCM16|AUDIO_VOICE_VOL((uint32_t)volume*0x8000u/100u);
  app_core_post(AUDIO_CMD_VOICE,flags,decoded[index].address,
                decoded[index].samples*2u,decoded[index].rate);
}
void ui_audio_suspend(void) {
  if(!loaded) return;
  loaded=false;
  ducked=false;

  app_core_post(AUDIO_CMD_VOICE_STOP,AUDIO_VOICE_ALL,0,0,0);
}
void ui_audio_resume(void) {
  if(loaded || !enabled || !snd_available()) return;
  size_t used=0;
  for(unsigned i=0;i<UI_SOUND_COUNT+1;i++) {
    const uint8_t *data=compressed[i].start;
    size_t bytes=(size_t)((uintptr_t)compressed[i].end-(uintptr_t)data);
    GcAdpcmInfo info;
    if(!gcadpcm_info(data,bytes,&info)) return;
    used=(used+3u)&~3u;
    if(info.samples>(AUDIO_PCM_MAX-used)/2u) return;
    int16_t *pcm=(int16_t *)(uintptr_t)(AUDIO_PCM_ADDR+used);
    if(!gcadpcm_decode(data,bytes,pcm,info.samples)) return;
    for(size_t k=0;k<info.samples;k++) {
      int32_t v=pcm[k]*3/2;
      pcm[k]=(int16_t)(v>32767?32767:v<-32768?-32768:v);
    }
    decoded[i].address=AUDIO_PCM_ADDR+(uint32_t)used;
    decoded[i].samples=info.samples; decoded[i].rate=info.rate;
    used+=(size_t)info.samples*2u;
  }
  os_cache_sync();
  loaded=true;
  ducked=false; music_volume=MUSIC_VOLUME;
  play(0,true,MUSIC_VOLUME);
}
void ui_audio_init(void) {
  char path[320]; snprintf(path,sizeof path,"%s/sound.cfg",app_dir());
  FILE *f=fopen(path,"rb");
  if(f) { int c=fgetc(f); if(c=='0'||c=='1') enabled=c=='1'; fclose(f); }
  ui_audio_resume();
}
void ui_audio_toggle(void) {
  enabled=!enabled;
  if(enabled) ui_audio_resume(); else ui_audio_suspend();
  char path[320]; snprintf(path,sizeof path,"%s/sound.cfg",app_dir());
  FILE *f=fopen(path,"wb");
  if(f) { fputc(enabled?'1':'0',f); fclose(f); }
}
bool ui_audio_enabled(void) { return enabled; }
void ui_sound(int effect) {
  if(enabled && loaded && effect>=0 && effect<UI_SOUND_COUNT) {
    unsigned index=(unsigned)effect+1;
    uint32_t now=sys_millis();
    uint32_t end=now+(decoded[index].samples*1000u+decoded[index].rate-1u)/decoded[index].rate+40u;
    if(!ducked || (int32_t)(end-duck_until)>0) duck_until=end;
    ducked=true;
    music_gain(DUCK_VOLUME);
    play(index,false,effect==UI_MOVE?80:100);
  }
}
