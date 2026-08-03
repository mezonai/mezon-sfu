#ifndef SFU_PROTOCOL_JSON_LITE_H
#define SFU_PROTOCOL_JSON_LITE_H

#include <stddef.h>

/* Output buffer capacity must be >= 1. On success, return value is the number
 * of bytes written excluding the trailing NUL. Rejects out == NULL or
 * out_cap == 0 with -1 before any write (including the trailing NUL). */
int sfu_json_extract_string(const char *json, size_t json_len, const char *field, char *out, size_t out_cap);
int sfu_json_escape(const char *value, size_t value_len, char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_JSON_LITE_H */
