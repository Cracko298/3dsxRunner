#include <aurora_app.h>
#include <string.h>
#include <stdio.h>
#include "httpbridge.h"
#define HTTP_FAIL ((int32_t)0xD8A0A046u)
#define HTTP_HEADER_MISSING ((int32_t)0xD840A028u)
static HttpResponse *responses[HTTP_SLOTS];
void httpbridge_drop(uint32_t mask) {
  for (unsigned i=0;i<HTTP_SLOTS;i++) if ((mask>>i)&1u) {
    if (responses[i]) http_free(responses[i]);
    responses[i]=NULL;
  }
}
void httpbridge_do(JobHttp *h) {
  static const char *methods[]={NULL,"GET","POST","HEAD","PUT","DELETE","POST","PUT"};
  h->result=HTTP_FAIL;
  if (h->slot>=HTTP_SLOTS || h->size>HTTP_CHUNK) return;
  HttpResponse *r=responses[h->slot];
  switch (h->op) {
  case HTTPOP_NET_STATUS:
    h->status=net_available() && net_connected();
    break;
  case HTTPOP_NET_CONNECT:
    h->status=net_available() && net_connect();
    if (!h->status) return;
    break;
  case HTTPOP_REQUEST:
    httpbridge_drop(1u<<h->slot);
    h->status=h->length=0;
    if (!h->method || h->method>7 || h->body_size>sizeof h->body ||
        !memchr(h->url,0,sizeof h->url) || !memchr(h->headers,0,sizeof h->headers)) return;
    h->error[0]=0;
    if (!net_available() || !net_connect()) {
      snprintf(h->error,sizeof h->error,"%s",net_error()); return;
    }
    r=http_request(methods[h->method],h->url,h->headers,h->body,h->body_size);
    if (!r) { snprintf(h->error,sizeof h->error,"%s",net_error()); return; }
    responses[h->slot]=r;
    h->status=(uint32_t)r->status; h->length=(uint32_t)r->length;
    break;
  case HTTPOP_READ:
    if (!r || h->offset>r->length || h->size>r->length-h->offset) return;
    memcpy(h->data,r->body+h->offset,h->size);
    break;
  case HTTPOP_HEADER:
    if (!r || !h->size || !memchr(h->headers,0,sizeof h->headers)) return;
    if (!http_header(r,h->headers,(char *)h->data,h->size)) {
      h->result=HTTP_HEADER_MISSING; return;
    }
    h->length=(uint32_t)strlen((char *)h->data)+1;
    break;
  default: return;
  }
  h->result=0;
}
