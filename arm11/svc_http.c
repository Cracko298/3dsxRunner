#include "sup.h"
#define HTTP_PENDING 0xD840A02Bu
#define HTTP_BAD_ARG 0xD9001830u
#define HTTP_STATE   0xD8A0A04Cu

typedef struct {
  u32 id, owner, method, begun, ready, status, length, pos, body_size;
  s32 error;
  char url[1024], headers[1024];
  u8 body[1024];
} HttpContext;
static HttpContext contexts[HTTP_SLOTS];
static u32 next_id, next_owner;
static JobHttp bridge __attribute__((aligned(64)));
static struct { HttpContext *c; u32 id, va, left; } pending;

static void reply(u32 *cmd,u32 id,s32 result) {
  cmd[0]=ipc_header(id,1,0); cmd[1]=(u32)result;
}
static HttpContext *find(u32 id) {
  for (u32 i=0;i<HTTP_SLOTS;i++) if (id && contexts[i].id==id) return &contexts[i];
  return NULL;
}
static void drop(HttpContext *c) {
  P->close_http|=1u<<(c-contexts); memset(c,0,sizeof *c);
}
static void closed(Object *o) {
  if (o->u.se.data[1]) for (u32 i=0;i<HTTP_SLOTS;i++)
    if (contexts[i].owner==o->u.se.data[1]) drop(&contexts[i]);
}

static const char *string(u32 va,u32 size,u32 cap) {
  if (!size || size>cap || !mmu_user_ok(va,size)) return NULL;
  const char *s=(const char *)(uintptr_t)va;
  if (s[size-1] || strnlen(s,size)!=size-1) return NULL;
  for (u32 i=0;i+1<size;i++) if ((u8)s[i]<32 || (u8)s[i]==127) return NULL;
  return s;
}
static bool token(const char *s) {
  if (!*s) return false;
  for (;*s;s++) {
    char ch=*s;
    if ((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')) continue;
    const char *p="!#$%&'*+-.^_`|~"; while (*p && *p!=ch) p++;
    if (!*p) return false;
  }
  return true;
}
static void done(Thread *t);
static void submit(Thread *t,u32 op) {
  bridge.op=op; bridge.slot=(u32)(pending.c-contexts); bridge.result=0;
  P->http_pa=(u32)(uintptr_t)&bridge;
  arm9_request(t,done); P->yield=YIELD_HTTP;
}
static void read_next(Thread *t) {
  bridge.offset=pending.c->pos;
  bridge.size=pending.left>HTTP_CHUNK?HTTP_CHUNK:pending.left;
  submit(t,HTTPOP_READ);
}
static void done(Thread *t) {
  u32 *cmd=(u32 *)(uintptr_t)(t->ctx.tls+0x80);
  HttpContext *c=pending.c;
  if (bridge.result) {
    if (bridge.op!=HTTPOP_HEADER) c->error=bridge.result;
    if (bridge.op==HTTPOP_REQUEST) log_printf("http:C request failed: %s\n",bridge.error);
    reply(cmd,pending.id,bridge.result); return;
  }
  if (bridge.op==HTTPOP_REQUEST) {
    c->ready=1; c->status=bridge.status; c->length=bridge.length;
    reply(cmd,pending.id,0);
  } else if (bridge.op==HTTPOP_READ) {
    memcpy((void *)(uintptr_t)pending.va,bridge.data,bridge.size);
    c->pos+=bridge.size; pending.va+=bridge.size; pending.left-=bridge.size;
    if (pending.left) { read_next(t); return; }
    reply(cmd,pending.id,c->pos<c->length?HTTP_PENDING:0);
  } else {
    memcpy((void *)(uintptr_t)pending.va,bridge.data,bridge.length);
    reply(cmd,pending.id,0);
  }
}
static bool append_header(HttpContext *c,const char *name,const char *value) {
  u32 n=(u32)strlen(name),v=(u32)strlen(value),len=(u32)strlen(c->headers);
  if (n+v+4>=sizeof c->headers-len) return false;
  memcpy(c->headers+len,name,n); len+=n;
  memcpy(c->headers+len,": ",2); len+=2;
  memcpy(c->headers+len,value,v); len+=v;
  memcpy(c->headers+len,"\r\n",3); return true;
}
static u32 encode(u8 *dst,const char *src) {
  static const char hex[]="0123456789ABCDEF";
  u32 n=0;
  for (;*src;src++) {
    u8 c=(u8)*src;
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') dst[n++]=c;
    else if (c==' ') dst[n++]='+';
    else { dst[n++]='%'; dst[n++]=hex[c>>4]; dst[n++]=hex[c&15]; }
  }
  return n;
}
static void request(Thread *t,Object *o,u32 *cmd) {
  u32 id=cmd[0]>>16,handle=cmd[1];
  if (id==1) {

    if (!o->u.se.data[1]) o->u.se.data[1]=++next_owner;
    reply(cmd,id,0); return;
  }
  if (id==0x39) { closed(o); o->u.se.data[1]=0; reply(cmd,id,0); return; }
  if (id==2) {
    const char *url=string(cmd[4],cmd[1],sizeof contexts[0].url);
    if (!o->u.se.data[1] || !url || cmd[1]<8 ||
        (memcmp(url,"http://",7) && (cmd[1]<9 || memcmp(url,"https://",8))) ||
        cmd[2]<1 || cmd[2]>7) { reply(cmd,id,HTTP_BAD_ARG); return; }
    for (u32 i=0;i<HTTP_SLOTS;i++) if (!contexts[i].id) {
      HttpContext *c=&contexts[i]; memset(c,0,sizeof *c);
      c->id=++next_id; if (!c->id) c->id=++next_id;
      c->owner=o->u.se.data[1]; c->method=cmd[2]; memcpy(c->url,url,cmd[1]);
      cmd[0]=ipc_header(id,2,0); cmd[1]=0; cmd[2]=c->id; return;
    }
    reply(cmd,id,RES_OUT_OF_MEMORY); return;
  }
  HttpContext *c=find(handle);
  if (!c || (o->u.se.data[0]!=handle && o->u.se.data[1]!=c->owner && id!=8)) {
    reply(cmd,id,RES_INVALID_HANDLE); return;
  }
  if (id==8) { o->u.se.data[0]=handle; reply(cmd,id,0); return; }
  switch (id) {
  case 3: drop(c); reply(cmd,id,0); return;
  case 4: P->close_http|=1u<<(c-contexts); c->ready=0; c->error=HTTP_STATE; reply(cmd,id,0); return;
  case 5: cmd[0]=ipc_header(id,2,0); cmd[1]=0; cmd[2]=c->ready?7:5; return;
  case 6: cmd[0]=ipc_header(id,3,0); cmd[1]=0; cmd[2]=c->pos; cmd[3]=c->length; return;
  case 0xE:
    reply(cmd,id,0); return;
  case 0x37: reply(cmd,id,cmd[2]<=1?0:HTTP_BAD_ARG); return;
  case 0x2B: case 0x2C:
    reply(cmd,id,cmd[2]?RES_UNIMPL:0); return;
  case 0x11: case 0x12: {
    const char *name=string(cmd[5],cmd[2],1024),*value=string(cmd[7],cmd[3],1024);
    if (c->begun || !name || !value || (id==0x11 && !token(name))) { reply(cmd,id,HTTP_BAD_ARG); return; }
    if (id==0x11) { reply(cmd,id,append_header(c,name,value)?0:HTTP_BAD_ARG); return; }
    u32 need=3u*((u32)strlen(name)+(u32)strlen(value))+2;
    if (need>sizeof c->body-c->body_size) { reply(cmd,id,HTTP_BAD_ARG); return; }
    if (!c->body_size && !append_header(c,"Content-Type","application/x-www-form-urlencoded")) { reply(cmd,id,HTTP_BAD_ARG); return; }
    if (c->body_size) c->body[c->body_size++]='&';
    c->body_size+=encode(c->body+c->body_size,name); c->body[c->body_size++]='=';
    c->body_size+=encode(c->body+c->body_size,value); reply(cmd,id,0); return;
  }
  case 0x14:
    if (c->begun || cmd[2]>sizeof c->body || (cmd[2] && !mmu_user_ok(cmd[4],cmd[2]))) { reply(cmd,id,HTTP_BAD_ARG); return; }
    c->body_size=cmd[2]; if (c->body_size) memcpy(c->body,(void *)(uintptr_t)cmd[4],c->body_size);
    reply(cmd,id,0); return;
  case 9:
    if (c->begun) { reply(cmd,id,HTTP_STATE); return; }
    c->begun=1; pending.c=c; pending.id=id;
    memset(&bridge,0,sizeof bridge); bridge.method=c->method; bridge.body_size=c->body_size;
    memcpy(bridge.url,c->url,sizeof c->url); memcpy(bridge.headers,c->headers,sizeof c->headers);
    memcpy(bridge.body,c->body,c->body_size); submit(t,HTTPOP_REQUEST); return;
  case 0x22: case 0x23:
    cmd[0]=ipc_header(id,2,0); cmd[1]=c->ready?0:(c->error?(u32)c->error:HTTP_STATE); cmd[2]=c->status; return;
  case 0xB: case 0xC: {
    u32 size=cmd[2],va=cmd[id==0xB?4:6];
    if (!c->ready || c->error) { reply(cmd,id,c->error?(u32)c->error:HTTP_STATE); return; }
    if (size && !mmu_user_write_ok(va,size)) { reply(cmd,id,HTTP_BAD_ARG); return; }
    pending.c=c; pending.id=id; pending.va=va;
    pending.left=size<c->length-c->pos?size:c->length-c->pos;
    if (!pending.left) { reply(cmd,id,c->pos<c->length?HTTP_PENDING:0); return; }
    read_next(t); return;
  }
  case 0x1E: {
    const char *name=string(cmd[5],cmd[2],sizeof bridge.headers);
    u32 size=cmd[3],va=cmd[7];
    if (!c->ready || !name || !token(name) || !size || size>HTTP_CHUNK || !mmu_user_write_ok(va,size)) {
      reply(cmd,id,HTTP_BAD_ARG); return;
    }
    pending.c=c; pending.id=id; pending.va=va;
    memcpy(bridge.headers,name,cmd[2]); bridge.size=size; submit(t,HTTPOP_HEADER); return;
  }
  default: reply(cmd,id,RES_UNIMPL); return;
  }
}
const Service http_service={"http:C",request,closed};

void http_network_request(Thread *t,u32 op,void (*callback)(Thread *)) {
  memset(&bridge,0,sizeof bridge); bridge.op=op;
  P->http_pa=(u32)(uintptr_t)&bridge;
  arm9_request(t,callback); P->yield=YIELD_HTTP;
}
JobHttp *http_network_result(void) { return &bridge; }
