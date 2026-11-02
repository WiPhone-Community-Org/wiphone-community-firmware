/* json_read.cpp - see json_read.h. The reader is gemini.cpp's (0.9.81), moved here byte for byte;
 * jsNumber is new (the weather). */
#include "json_read.h"

#include <stdlib.h>
#include <string.h>



void jsWs(Js* j) {
  while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
    j->p++;
  }
}

static int jsHex4(const char* p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    const char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

/* Append code point `cp` as UTF-8 at out[*o] if the whole sequence fits (cap counts the NUL). */
static bool jsPutCp(uint32_t cp, char* out, size_t cap, size_t* o) {
  char b[4];
  int n;
  if (cp < 0x80) { b[0] = (char)cp; n = 1; }
  else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
  else if (cp < 0x10000) {
    b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
  } else {
    b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
  }
  if (!out || *o + (size_t)n >= cap) {
    return false;
  }
  memcpy(out + *o, b, (size_t)n);
  *o += (size_t)n;
  return true;
}

/* At '"': decode the string, appending at out[*o] (NUL kept after it) while it fits; *trunc
 * when something did not. out may be NULL (skip). False on a malformed string. */
bool jsString(Js* j, char* out, size_t cap, size_t* o, bool* trunc) {
  if (j->p >= j->e || *j->p != '"') {
    return false;
  }
  j->p++;
  size_t dummy = 0;
  if (!o) o = &dummy;
  while (j->p < j->e) {
    const char c = *j->p++;
    if (c == '"') {
      if (out && cap) out[*o < cap ? *o : cap - 1] = '\0';
      return true;
    }
    if ((unsigned char)c < 0x20) {
      return false;                              // a raw control character: not JSON
    }
    if (c != '\\') {
      if (out && *o + 1 < cap) {
        out[(*o)++] = c;
      } else if (out && trunc) {
        *trunc = true;
      }
      continue;
    }
    if (j->p >= j->e) {
      return false;
    }
    const char x = *j->p++;
    uint32_t cp;
    switch (x) {
    case '"': cp = '"'; break;
    case '\\': cp = '\\'; break;
    case '/': cp = '/'; break;
    case 'b': cp = 8; break;
    case 'f': cp = 12; break;
    case 'n': cp = '\n'; break;
    case 'r': cp = '\r'; break;
    case 't': cp = '\t'; break;
    case 'u': {
      if (j->e - j->p < 4) return false;
      const int h = jsHex4(j->p);
      if (h < 0) return false;
      j->p += 4;
      cp = (uint32_t)h;
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        int lo = -1;
        if (j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
          lo = jsHex4(j->p + 2);
        }
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          j->p += 6;
          cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
        } else {
          cp = 0xFFFD;                           // a lone high surrogate
        }
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = 0xFFFD;                             // a lone low surrogate
      }
      break;
    }
    default:
      return false;
    }
    if (cp == 0) {
      continue;                                  // \u0000 would end the C string: dropped
    }
    if (out && !jsPutCp(cp, out, cap, o) && trunc) {
      *trunc = true;
    }
  }
  return false;
}

/* Skip one value of any kind: a string, a primitive, or a whole object/array by counting
 * brackets (strings inside skipped properly). */
bool jsSkip(Js* j) {
  jsWs(j);
  if (j->p >= j->e) {
    return false;
  }
  const char c = *j->p;
  if (c == '"') {
    return jsString(j, NULL, 0, NULL, NULL);
  }
  if (c == '{' || c == '[') {
    int depth = 0;
    while (j->p < j->e) {
      const char d = *j->p;
      if (d == '"') {
        if (!jsString(j, NULL, 0, NULL, NULL)) return false;
        continue;
      }
      j->p++;
      if (d == '{' || d == '[') {
        depth++;
      } else if (d == '}' || d == ']') {
        if (--depth == 0) return true;
      }
    }
    return false;
  }
  const char* s = j->p;
  while (j->p < j->e && !strchr(",}] \t\r\n", *j->p)) j->p++;
  return j->p > s;
}

/* Object iteration. At '{': jsObjOpen. Then jsObjNext: 1 = a key was read into `key` and the
 * value is next; 0 = the object closed; -1 = not JSON. */
bool jsOpen(Js* j, char c) {
  jsWs(j);
  if (j->p < j->e && *j->p == c) {
    j->p++;
    return true;
  }
  return false;
}

int jsObjNext(Js* j, char* key, size_t kcap) {
  jsWs(j);
  if (j->p < j->e && *j->p == ',') {
    j->p++;
    jsWs(j);
  }
  if (j->p >= j->e) return -1;
  if (*j->p == '}') {
    j->p++;
    return 0;
  }
  size_t o = 0;
  bool t = false;
  if (!jsString(j, key, kcap, &o, &t)) return -1;
  jsWs(j);
  if (j->p >= j->e || *j->p != ':') return -1;
  j->p++;
  jsWs(j);
  return 1;
}

int jsArrNext(Js* j) {
  jsWs(j);
  if (j->p < j->e && *j->p == ',') {
    j->p++;
    jsWs(j);
  }
  if (j->p >= j->e) return -1;
  if (*j->p == ']') {
    j->p++;
    return 0;
  }
  return 1;
}

bool jsStrInto(Js* j, char* out, size_t cap) {
  jsWs(j);
  if (j->p < j->e && *j->p == '"') {
    size_t o = 0;
    bool t = false;
    return jsString(j, out, cap, &o, &t);
  }
  return jsSkip(j);                              // not a string: skipped, out left alone
}

bool jsTrue(Js* j) {
  jsWs(j);
  if (j->e - j->p >= 4 && !strncmp(j->p, "true", 4)) {
    j->p += 4;
    return true;
  }
  jsSkip(j);
  return false;
}

int jsNumber(Js* j, double* v) {
  jsWs(j);
  if (j->p >= j->e) {
    return -1;
  }
  if (j->e - j->p >= 4 && !strncmp(j->p, "null", 4)) {
    j->p += 4;
    return 0;
  }
  /* JSON's number grammar, checked by hand (strtod alone would take "0x1A", "inf", " 1"). */
  const char* s = j->p;
  const char* q = s;
  if (q < j->e && *q == '-') q++;
  if (q >= j->e || *q < '0' || *q > '9') {
    jsSkip(j);
    return -1;
  }
  if (*q == '0') {
    q++;
  } else {
    while (q < j->e && *q >= '0' && *q <= '9') q++;
  }
  if (q < j->e && *q == '.') {
    q++;
    const char* d = q;
    while (q < j->e && *q >= '0' && *q <= '9') q++;
    if (q == d) {
      jsSkip(j);
      return -1;
    }
  }
  if (q < j->e && (*q == 'e' || *q == 'E')) {
    q++;
    if (q < j->e && (*q == '+' || *q == '-')) q++;
    const char* d = q;
    while (q < j->e && *q >= '0' && *q <= '9') q++;
    if (q == d) {
      jsSkip(j);
      return -1;
    }
  }
  /* What follows must end the value: a body cut mid-number ("17.") is not a number. */
  if (q >= j->e || !strchr(",}] \t\r\n", *q)) {
    jsSkip(j);
    return -1;
  }
  char buf[41];
  const size_t n = (size_t)(q - s);
  if (n >= sizeof(buf)) {
    j->p = q;
    return -1;
  }
  memcpy(buf, s, n);
  buf[n] = '\0';
  *v = strtod(buf, NULL);
  j->p = q;
  return 1;
}
