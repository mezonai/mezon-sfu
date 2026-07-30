#ifndef SFU_ROOM_ROOM_H
#define SFU_ROOM_ROOM_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "sfu/datadef.h"

/* Updated initialization signature to accept uint64_t for room_id */
int sfu_room_init(sfu_room_t *room, uint64_t room_id);
void sfu_room_destroy(sfu_room_t *room);

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer);

#endif /* SFU_ROOM_ROOM_H */
