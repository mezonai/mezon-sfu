#ifndef SFU_ROOM_ROOM_H
#define SFU_ROOM_ROOM_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "sfu/config.h"

/*
 * Minimal peer registry standing in for real room/publish/subscribe
 * signaling, which doesn't exist yet (protocol/signaling/, peer/auth.c).
 * Every peer seen on the shared UDP port is implicitly "in the room" and
 * receives every other peer's packets -- i.e. broadcast-to-room, the
 * same shape as mezon-proto-server's channel broadcast, just over UDP/
 * SRTP instead of the WS/TLS channel fan-out.
 *
 * KNOWN LIMITATION: this uses a mutex on the packet hot path (every
 * recv touches the peer table). That's acceptable as a placeholder to
 * prove out the cross-thread fan-out mechanics, but it is exactly the
 * kind of hot-path lock that should not survive contact with real
 * traffic -- replace with a sharded-by-room or RCU/seqlock snapshot
 * structure once real join/publish signaling exists and packets carry
 * enough identity (SSRC) to look up room membership without a global
 * table scan.
 */
typedef struct sfu_peer_entry {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  uint32_t worker_id; /* which worker core owns this peer's send path */
  bool active;
} sfu_peer_entry_t;

typedef struct sfu_room {
  sfu_peer_entry_t peers[SFU_ROOM_MAX_PEERS];
  uint32_t peer_count;
  pthread_mutex_t lock;
} sfu_room_t;

int sfu_room_init(sfu_room_t *room);
void sfu_room_destroy(sfu_room_t *room);

/* Registers a peer's address + owning worker on first sight, or
 * refreshes its worker_id if it's already known (e.g. reconnected on a
 * different flow). Cheap enough to call once per received packet for
 * now; real signaling should call this from join/publish handling
 * instead of implicit discovery. */
void sfu_room_touch_peer(sfu_room_t *room, const struct sockaddr_storage *addr,
                         socklen_t addr_len, uint32_t worker_id);

/* Copies up to max_out peer entries, excluding the peer matching
 * `exclude` (the packet's sender, which shouldn't receive its own
 * packet echoed back). Returns the number copied. Snapshot semantics:
 * copies under the lock, then the caller iterates the copy lock-free. */
uint32_t sfu_room_list_subscribers_excluding(
    sfu_room_t *room, const struct sockaddr_storage *exclude,
    socklen_t exclude_len, sfu_peer_entry_t *out, uint32_t max_out);

#endif /* SFU_ROOM_ROOM_H */
