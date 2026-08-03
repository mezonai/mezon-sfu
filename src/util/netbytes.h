#ifndef SFU_UTIL_NETBYTES_H
#define SFU_UTIL_NETBYTES_H

#include <stdint.h>

static inline uint16_t sfu_read_be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]); }

static inline uint32_t sfu_read_be24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2]; }

static inline uint32_t sfu_read_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }

static inline void sfu_write_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v);
}

static inline void sfu_write_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v);
}

#endif /* SFU_UTIL_NETBYTES_H */
