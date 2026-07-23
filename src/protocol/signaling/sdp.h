#ifndef SFU_PROTOCOL_SDP_H
#define SFU_PROTOCOL_SDP_H

#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

int sfu_sdp_build_answer(const sfu_peer_session_t *session, const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag,
                         const char *pwd, const char *fingerprint, char *out, size_t out_cap);

int sfu_sdp_build_offer(const sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                        char *out, size_t out_cap);

int sfu_sdp_build_initial_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_SDP_H */
