#include "cheatfile.h"

#include <stdlib.h>
#include <string.h>

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool code_line(const char *s, uint32_t *a, uint32_t *b) {
  uint32_t w[2] = {0, 0};
  for (int k = 0; k < 2; k++) {
    while (*s == ' ' || *s == '\t') s++;
    for (int i = 0; i < 8; i++, s++) {
      int v = hexval(*s);
      if (v < 0) return false;
      w[k] = w[k] << 4 | (uint32_t)v;
    }
    if (hexval(*s) >= 0) return false;
  }
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  if (*s) return false;
  *a = w[0], *b = w[1];
  return true;
}

static Cheat *add(CheatList *l, const char *name, size_t len) {
  if(l->n>=512) return NULL;
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 32;
    Cheat *grown = realloc(l->c, (size_t)cap * sizeof *grown);
    if (!grown) return NULL;
    l->c = grown, l->cap = cap;
  }
  Cheat *c = &l->c[l->n++];
  memset(c, 0, sizeof *c);
  if (len >= sizeof c->name) len = sizeof c->name - 1;
  memcpy(c->name, name, len);
  return c;
}

static bool push_code(Cheat *c, uint32_t a, uint32_t b) {
  if(c->lines>=4095) return false;
  if ((c->lines & 63u) == 0) {
    uint32_t *grown = realloc(c->codes, (c->lines + 64u) * 2u * sizeof *grown);
    if (!grown) return false;
    c->codes = grown;
  }
  c->codes[c->lines * 2] = a;
  c->codes[c->lines * 2 + 1] = b;
  c->lines++;
  return true;
}

void cheatfile_parse(const char *text, CheatList *out) {
  Cheat *cur = NULL;
  bool in_note = false;
  char line[512];
  while (*text) {
    size_t n = strcspn(text, "\n");
    size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
    memcpy(line, text, k);
    line[k] = 0;
    text += n + (text[n] == '\n');
    char *s = line;
    while (*s == ' ' || *s == '\t' || (unsigned char)*s == 0xEF || (unsigned char)*s == 0xBB || (unsigned char)*s == 0xBF)
      s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
    if (in_note) {
      char *end = strchr(s, '}');
      if (end) *end = 0, in_note = false;
      if (cur && strlen(cur->note) + strlen(s) + 2 < sizeof cur->note) {
        if (cur->note[0]) strcat(cur->note, " ");
        strcat(cur->note, s);
      }
      continue;
    }
    if (s[0] == '[') {
      char *end = strrchr(s, ']');
      cur = add(out, s + 1, end ? (size_t)(end - s - 1) : len - 1);
      continue;
    }
    if (s[0] == '{') {
      char *end = strchr(s, '}');
      if (end) *end = 0;
      else in_note = true;
      if (cur) {
        strncpy(cur->note, s + 1, sizeof cur->note - 1);
        cur->note[sizeof cur->note - 1] = 0;
      }
      continue;
    }
    uint32_t a, b;
    if (cur && code_line(s, &a, &b) && !push_code(cur,a,b)) {
      free(cur->codes); cur->codes=NULL; cur->lines=0; cur=NULL;
    }
  }
  int kept = 0;
  for (int i = 0; i < out->n; i++) {
    if (out->c[i].lines)
      out->c[kept++] = out->c[i];
    else
      free(out->c[i].codes);
  }
  out->n = kept;
}

void cheatfile_free(CheatList *l) {
  for (int i = 0; i < l->n; i++) free(l->c[i].codes);
  free(l->c);
  l->c = NULL;
  l->n = l->cap = 0;
}

uint32_t *cheatfile_program(const CheatList *l, const uint32_t *extra, uint32_t extra_lines, uint32_t *bytes,
                            int *index_of, int max_index) {
  uint32_t words = 1, n = 0;
  for (int i = 0; i < l->n; i++)
    if (l->c[i].enabled) words += 1 + l->c[i].lines * 2, n++;
  if (extra_lines) words += 1 + extra_lines * 2, n++;
  if (!n) {
    *bytes = 0;
    return NULL;
  }
  if(words>8192 || n>32 || max_index<(int)n) { *bytes=UINT32_MAX; return NULL; }
  uint32_t *p = malloc(words * 4u), at = 1;
  if (!p) {
    *bytes = UINT32_MAX;
    return NULL;
  }
  p[0] = n;
  int k = 0;
  for (int i = 0; i < l->n; i++) {
    if (!l->c[i].enabled) continue;
    if (k < max_index) index_of[k] = i;
    k++;
    p[at++] = l->c[i].lines;
    memcpy(&p[at], l->c[i].codes, l->c[i].lines * 8u);
    at += l->c[i].lines * 2;
  }
  if (extra_lines) {
    if (k < max_index) index_of[k] = -1;
    p[at++] = extra_lines;
    memcpy(&p[at], extra, extra_lines * 8u);
    at += extra_lines * 2;
  }
  *bytes = at * 4u;
  return p;
}
