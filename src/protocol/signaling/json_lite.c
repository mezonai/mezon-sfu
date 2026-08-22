#include "protocol/signaling/json_lite.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

int sfu_json_extract_string(const char *json, size_t json_len, const char *field, char *out, size_t out_cap) {
  if (out == NULL || out_cap == 0) {
    return -1;
  }
  char needle[128];
  int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
  if (needle_len < 0 || (size_t)needle_len >= sizeof(needle) || (size_t)needle_len > json_len) {
    return -1;
  }

  const char *end = json + json_len;
  const char *p = NULL;
  for (const char *candidate = json; candidate + needle_len <= end; candidate++) {
    if (memcmp(candidate, needle, (size_t)needle_len) == 0) {
      p = candidate + needle_len;
      break;
    }
  }
  if (!p) {
    return -1;
  }

  while (p < end && (*p == ' ' || *p == ':')) {
    p++;
  }
  if (p == end || *p++ != '"') {
    return -1;
  }

  size_t out_len = 0;
  while (p < end && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      if (p == end) {
        return -1;
      }
      switch (*p++) {
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        default: c = p[-1]; break;
      }
    }
    if (out_len + 1 >= out_cap) {
      return -1;
    }
    out[out_len++] = c;
  }
  if (p == end || *p != '"') {
    return -1;
  }

  out[out_len] = '\0';
  return (int)out_len;
}

int sfu_json_extract_bool(const char *json, size_t json_len, const char *field, bool *out) {
  if (!json || !field || !out || field[0] == '\0') {
    return -1;
  }

  char needle[128];
  int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
  if (needle_len < 0 || (size_t)needle_len >= sizeof(needle) || (size_t)needle_len > json_len) {
    return -1;
  }

  const char *end = json + json_len;
  const char *p = NULL;
  for (const char *candidate = json; candidate + needle_len <= end; candidate++) {
    if (memcmp(candidate, needle, (size_t)needle_len) == 0) {
      p = candidate + needle_len;
      break;
    }
  }
  if (!p) {
    return -1;
  }

  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) {
    p++;
  }
  if (p >= end) {
    return -1;
  }

  if (*p == '"') {
    p++;
    char token[8] = {0};
    size_t n = 0;
    while (p < end && *p != '"' && n + 1 < sizeof(token)) {
      token[n++] = *p++;
    }
    if (p >= end || *p != '"') {
      return -1;
    }
    token[n] = '\0';
    if (strcmp(token, "true") == 0 || strcmp(token, "1") == 0) {
      *out = true;
      return 0;
    }
    if (strcmp(token, "false") == 0 || strcmp(token, "0") == 0) {
      *out = false;
      return 0;
    }
    return -1;
  }

  if ((size_t)(end - p) >= 4 && memcmp(p, "true", 4) == 0) {
    *out = true;
    return 0;
  }
  if ((size_t)(end - p) >= 5 && memcmp(p, "false", 5) == 0) {
    *out = false;
    return 0;
  }
  return -1;
}

int sfu_json_extract_uint64(const char *json, size_t json_len, const char *field, uint64_t *out) {
  if (!json || !field || !out || !*field) return -1;
  char needle[128];
  int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
  if (needle_len <= 0 || (size_t)needle_len >= sizeof(needle)) return -1;
  const char *end = json + json_len;
  const char *p = NULL;
  for (const char *candidate = json; candidate + needle_len <= end; candidate++) {
    if (memcmp(candidate, needle, (size_t)needle_len) == 0) { p = candidate + needle_len; break; }
  }
  if (!p) return -1;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if (p == end || *p++ != ':') return -1;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if (p == end || *p < '0' || *p > '9') return -1;
  uint64_t value = 0;
  do {
    uint32_t digit = (uint32_t)(*p++ - '0');
    if (value > (UINT64_MAX - digit) / 10) return -1;
    value = value * 10 + digit;
  } while (p < end && *p >= '0' && *p <= '9');
  if (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return -1;
  *out = value;
  return 0;
}

int sfu_json_escape(const char *value, size_t value_len, char *out, size_t out_cap) {
  if (out == NULL || out_cap == 0) {
    return -1;
  }
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
