#ifndef SFU_PROTOCOL_SDP_H
#define SFU_PROTOCOL_SDP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Builds an SDP answer by cloning the client's offer and replacing only
 * the attributes that must reflect mezon-sfu's side: ICE ufrag/pwd, the
 * DTLS certificate fingerprint, a=setup:passive (we are always the
 * DTLS *server* -- see transport/dtls/dtls.c's SSL_set_accept_state),
 * and a single static ICE-lite host candidate. Every codec/rtpmap/
 * fmtp/mid/direction line is passed through unchanged, since the
 * answer just needs to accept a compatible subset of what was offered.
 *
 * This is the server-side counterpart of what
 * tools/webrtc_test_client.html used to do by hand in JS before
 * signaling existed -- same transform, same reasoning, now the
 * server's job instead of the client's.
 *
 * Returns the answer length on success, or -1 if out_cap is too small
 * or the offer has no m= line.
 */
int sfu_sdp_build_answer(const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                         char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_SDP_H */
