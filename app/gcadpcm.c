#include "gcadpcm.h"
#include <string.h>

static uint32_t u32le(const uint8_t *p) {
  return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int32_t s16le(const uint8_t *p) {
  uint32_t n=p[0]|(uint32_t)p[1]<<8;
  return n>=32768 ? (int32_t)n-65536 : (int32_t)n;
}

bool gcadpcm_info(const uint8_t *data,size_t bytes,GcAdpcmInfo *info) {
  if(!data || !info || bytes<48 || memcmp(data,"GCA1",4)) return false;
  uint32_t rate=u32le(data+4),samples=u32le(data+8);
  if(rate<8000 || rate>32000 || !samples || samples>5u*1024*1024) return false;
  size_t frames=((size_t)samples+13)/14;
  if(bytes!=48+frames*8) return false;
  for(unsigned i=0;i<16;i++) {
    int32_t c=s16le(data+12+i*2);
    if(c < -4075 || c > 4075) return false;
  }
  for(size_t i=0;i<frames;i++)
    if((data[48+i*8]>>4)>7 || (data[48+i*8]&15)>12) return false;
  *info=(GcAdpcmInfo){rate,samples};
  return true;
}
bool gcadpcm_decode(const uint8_t *data,size_t bytes,int16_t *output,size_t capacity) {
  GcAdpcmInfo info;
  if(!output || !gcadpcm_info(data,bytes,&info) || capacity<info.samples) return false;
  int32_t coef[16];
  for(unsigned i=0;i<16;i++) coef[i]=s16le(data+12+i*2);
  int32_t h1=s16le(data+44),h2=s16le(data+46);
  size_t done=0;
  for(size_t at=48;at<bytes;at+=8) {
    unsigned pair=data[at]>>4,shift=data[at]&15;
    int32_t step=(int32_t)(2048u<<shift);
    for(unsigned j=0;j<14 && done<info.samples;j++) {
      unsigned b=data[at+1+j/2];
      int32_t q=j&1 ? b&15 : b>>4;
      if(q>=8) q-=16;
      int32_t v=(q*step+1024+coef[pair*2]*h1+coef[pair*2+1]*h2)>>11;
      if(v>32767) v=32767;
      if(v < -32768) v=-32768;
      output[done++]=(int16_t)v;
      h2=h1;h1=v;
    }
  }
  return true;
}
