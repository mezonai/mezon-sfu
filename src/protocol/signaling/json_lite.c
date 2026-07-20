#include "protocol/signaling/json_lite.h"

#include <stdio.h>
#include <string.h>

int sfu_json_extract_string(const char *json, const char *field, char *out, size_t out_cap) {
  char needle[128];
  int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
  const char *p = strstr(json, needle);
  if (!p) {
    return -1;
  }
  p += needle_len;

  while (*p == ' ' || *p == ':') {
    p++;
  }
  if (*p != '"') {
    return -1;
  }
  p++;

  size_t out_len = 0;
  while (*p && *p != '"') {
    char c = *p;
    if (c == '\\') {
      p++;
      switch (*p) {
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case '"':
          c = '"';
          break;
        case '\\':
          c = '\\';
          break;
        case '/':
          c = '/';
          break;
        default:
          c = *p;
          break; /* unknown escape: pass through literally */
      }
    }
    if (out_len + 1 >= out_cap) {
      return -1;
    }
    out[out_len++] = c;
    p++;
  }
  if (*p != '"') {
    return -1; /* unterminated string */
  }

  out[out_len] = '\0';
  return (int)out_len;
}

int sfu_json_escape(const char *value, size_t value_len, char *out, size_t out_cap) {
  size_t out_len = 0;
  for (size_t i = 0; i < value_len; i++) {
    char c = value[i];
    const char *esc = NULL;
    switch (c) {
      case '"':
        esc = "\\\"";
        break;
      case '\\':
        esc = "\\\\";
        break;
      case '\r':
        esc = "\\r";
        break;
      case '\n':
        esc = "\\n";
        break;
      case '\t':
        esc = "\\t";
        break;
      default:
        break;
    }
    if (esc) {
      size_t elen = strlen(esc);
      if (out_len + elen >= out_cap) {
        return -1;
      }
      memcpy(out + out_len, esc, elen);
      out_len += elen;
    } else {
      if (out_len + 1 >= out_cap) {
        return -1;
      }
      out[out_len++] = c;
    }
  }
  out[out_len] = '\0';
  return (int)out_len;
}
