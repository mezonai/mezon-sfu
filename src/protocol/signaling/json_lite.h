#ifndef SFU_PROTOCOL_JSON_LITE_H
#define SFU_PROTOCOL_JSON_LITE_H

#include <stddef.h>

/*
 * Not a general JSON parser -- signaling messages here have a fixed,
 * known shape ({"type":"...","sdp":"..."}), so a full parser would be
 * a lot of code for no real benefit. This just extracts one named
 * string field's value, unescaping \", \\, \r, \n, \t, and encodes a
 * string value the same way for building responses.
 */

/* Finds "field":"value" in a flat JSON object and writes the unescaped
 * value into out (NUL-terminated). Returns the unescaped length, or -1
 * if the field isn't found or would overflow out_cap. */
int sfu_json_extract_string(const char *json, const char *field, char *out, size_t out_cap);

/* Writes a JSON-escaped copy of value (length value_len) into out,
 * NOT including surrounding quotes. Returns the escaped length, or -1
 * if it would overflow out_cap. */
int sfu_json_escape(const char *value, size_t value_len, char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_JSON_LITE_H */
