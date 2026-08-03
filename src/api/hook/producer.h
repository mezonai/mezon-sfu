#ifndef MEZON_PRODUCER_H
#define MEZON_PRODUCER_H

#include <nats/nats.h>
#include <uv.h>
#include "api.pb-c.h"
#include "realtime.pb-c.h"

natsConnection *init_nats_connection(uv_loop_t *loop, const char *nats_url, const char *client_name);
void cleanup_nats_connection(natsConnection *nc);
bool dispatch_reaction_event(Mezon__Api__MessageReaction *msg, const char *key, const char *topic);
bool dispatch_message_service(Mezon__Realtime__FcmDataPayload *data, const char *event_key, const char *topic);
bool dispatch_api_request(uint16_t cid, int32_t session_id, int64_t user_id, const char *username, int32_t api_index, const uint8_t *extra, size_t extra_len,
                          const uint8_t *payload, size_t payload_len);

#endif