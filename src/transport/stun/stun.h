#ifndef SFU_TRANSPORT_STUN_H
#define SFU_TRANSPORT_STUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct sfu_ice_credentials {
  char ufrag[32];
  char pwd[64];
} sfu_ice_credentials_t;

void sfu_ice_credentials_generate(sfu_ice_credentials_t *out);
bool sfu_stun_is_stun_packet(const uint8_t *data, size_t len);
size_t sfu_stun_handle_binding_request(const uint8_t *data, size_t len, const sfu_ice_credentials_t *local, const struct sockaddr_storage *src_addr,
                                       socklen_t src_addr_len, uint8_t *out_buf, size_t out_buf_cap);
bool sfu_stun_extract_client_ufrag(const uint8_t *data, size_t len, const char *local_ufrag, char *out_client_ufrag, size_t max_len);

#endif /* SFU_TRANSPORT_STUN_H */
