#ifndef SFU_PROTOCOL_SIGNALING_H
#define SFU_PROTOCOL_SIGNALING_H

#include <pthread.h>
#include <stdint.h>
#include "peer/session.h"
#include "room/room_registry.h"
#include "transport/dtls/dtls.h"
#include "transport/stun/stun.h"

/*
 * WebSocket signaling server (RFC 6455 transport, a hand-rolled JSON
 * message shape -- not a standards body's WebRTC signaling protocol,
 * there isn't a universal one). One TCP listener, one detached thread
 * per accepted connection (control-plane, not the hot media path -- see
 * protocol/websocket/ws.h's header comment for why that's the right
 * amount of complexity here).
 *
 * Message shape, client -> server:
 *   {"type":"offer","sdp":"<full SDP offer text>"}
 * Message shape, server -> client:
 *   {"type":"answer","sdp":"<full SDP answer text>"}
 *
 * The server holds no per-connection state beyond the single exchange:
 * it builds one answer from one offer using the process-wide ICE
 * credentials and DTLS fingerprint (see the KNOWN LIMITATION notes in
 * transport/stun/stun.h and peer/session.h -- credentials are still
 * global to the process, not negotiated per-peer). The actual STUN/
 * DTLS/SRTP handshake that follows happens over the media UDP socket,
 * fully independent of this WebSocket connection.
 */
typedef struct sfu_signaling_server {
  int listen_fd;
  pthread_t thread;
  volatile int running;

  char media_host[64];
  uint16_t media_port;
  const sfu_ice_credentials_t *ice_creds; /* shared, not owned */
  const sfu_dtls_ctx_t *dtls_ctx;         /* shared, not owned */
  sfu_session_table_t *sessions;
  sfu_room_registry_t *room_registry;
} sfu_signaling_server_t;

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry);
void sfu_signaling_server_stop(sfu_signaling_server_t *s);

#endif /* SFU_PROTOCOL_SIGNALING_H */
