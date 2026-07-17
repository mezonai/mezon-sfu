#ifndef SFU_TRANSPORT_STUN_H
#define SFU_TRANSPORT_STUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/*
 * Minimal RFC 5389 STUN server: just enough to answer ICE-lite Binding
 * Requests from a WebRTC client's connectivity checks. We are not a full
 * ICE agent -- no candidate gathering, no outgoing checks, no
 * controlling/controlled role logic. We only ever *respond*.
 *
 * Short-term credential mechanism (RFC 5389 15.4): the client's request
 * carries USERNAME = "{our-ufrag}:{their-ufrag}" and is authenticated
 * with MESSAGE-INTEGRITY computed using *our* ICE password as the HMAC
 * key -- that's the recipient's password, per spec, not the sender's.
 *
 * KNOWN LIMITATION: local ICE credentials are a single fixed ufrag/pwd
 * pair for the whole process (see runtime/main.c), generated once at
 * startup and logged. There is no SDP/signaling exchange yet to
 * negotiate per-peer credentials -- see protocol/signaling/, not yet
 * implemented. A test client must be told the logged ufrag/pwd out of
 * band. This is exactly the gap protocol/signaling/'s SDP answer is
 * meant to close: it's where these credentials would actually get
 * handed to the browser.
 */
typedef struct sfu_ice_credentials {
  char ufrag[32];
  char pwd[64];
} sfu_ice_credentials_t;

/* Generates a random ufrag/pwd pair (base64-alphabet, ICE-legal chars). */
void sfu_ice_credentials_generate(sfu_ice_credentials_t *out);

/* RFC 7983 demux helper: does this datagram's first byte identify it as
 * STUN? (top two bits 00, matches STUN's message-type encoding). Used
 * by the packet demux before DTLS/SRTP checks. */
bool sfu_stun_is_stun_packet(const uint8_t *data, size_t len);

/*
 * Validates and answers one Binding Request. On success, writes a
 * Binding Success Response (XOR-MAPPED-ADDRESS + MESSAGE-INTEGRITY +
 * FINGERPRINT) into out_buf and returns its length. Returns 0 if the
 * packet isn't a valid, correctly-authenticated Binding Request for our
 * local credentials (caller should silently drop, not respond -- RFC
 * 5389 error responses are optional here and skipped to keep the
 * responder simple; a malformed/unauthenticated request gets no reply).
 */
size_t sfu_stun_handle_binding_request(const uint8_t *data, size_t len, const sfu_ice_credentials_t *local, const struct sockaddr_storage *src_addr,
                                       socklen_t src_addr_len, uint8_t *out_buf, size_t out_buf_cap);

#endif /* SFU_TRANSPORT_STUN_H */
