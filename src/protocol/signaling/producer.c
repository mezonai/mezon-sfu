#include "producer.h"
#include <arpa/inet.h>
#include <mimalloc.h>
#include <nats/adapters/libuv.h>
#include <nats/nats.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "mezon_api_client.h"
#include "mq_def.h"

extern natsConnection *global_nats_nc;

extern volatile sig_atomic_t g_shutdown;

static void onDisconnect(natsConnection *, void *) { log_info("[Producer] Disconnected from NATS server!\n"); }

static void onReconnect(natsConnection *nc, void *) {
  char url_buffer[256];
  natsStatus st = natsConnection_GetConnectedUrl(nc, url_buffer, sizeof(url_buffer));
  if (st == NATS_OK) {
    log_info("[Producer] Reconnected to: %s\n", url_buffer);
  } else {
    log_error("[Producer] Reconnected to NATS server (Could not fetch URL)\n");
  }
}

static void on_nats_error(__attribute__((unused)) natsConnection *nc, __attribute__((unused)) natsSubscription *sub, natsStatus stat,
                          __attribute__((unused)) void *closure) {
  log_error("NATS Error: %s", natsStatus_GetText(stat));
  if (stat == NATS_SLOW_CONSUMER) {
    log_warn("Worker is too slow! NATS is dropping messages.");
  }
}

static void onClosed(natsConnection *nc, void *closure) {
  (void)nc;
  uv_loop_t *loop = (uv_loop_t *)closure;

  if (g_shutdown) {
    log_info("[Producer] NATS connection closed as part of shutdown drain.\n");
    uv_stop(loop);
  } else {
    log_error("[Producer] NATS connection closed unexpectedly (not during shutdown).\n");
  }
}

natsConnection *init_nats_connection(uv_loop_t *loop, const char *url, const char *client_name) {
  natsConnection *nc = NULL;
  natsOptions *opts = NULL;
  natsStatus st;

  natsLibuv_SetThreadLocalLoop(loop);
  st = natsOptions_Create(&opts);
  if (st != NATS_OK) {
    log_error("Failed to create NATS options");
    return NULL;
  }

  natsOptions_SetURL(opts, url);
  natsOptions_SetName(opts, client_name);
  natsOptions_SetMaxReconnect(opts, -1);
  natsOptions_SetDisconnectedCB(opts, onDisconnect, NULL);
  natsOptions_SetReconnectedCB(opts, onReconnect, NULL);
  natsOptions_SetErrorHandler(opts, on_nats_error, NULL);
  natsOptions_SetClosedCB(opts, onClosed, (void *)loop);

  st = natsOptions_SetEventLoop(opts, (void *)loop, natsLibuv_Attach, natsLibuv_Read, natsLibuv_Write, natsLibuv_Detach);
  if (st != NATS_OK) {
    log_error("Failed to set NATS event loop");
    natsOptions_Destroy(opts);
    return NULL;
  }

  st = natsConnection_Connect(&nc, opts);
  natsOptions_Destroy(opts);

  if (st != NATS_OK) {
    log_error("[NATS] Failed to connect: %s\n", natsStatus_GetText(st));
    return NULL;
  }

  log_info("[NATS] Connected successfully on libuv loop.\n");
  return nc;
}

void cleanup_nats_connection(natsConnection *nc) {
  if (nc != NULL) {
    natsConnection_Close(nc);
    natsConnection_Destroy(nc);
    log_info("[Producer] NATS connection closed.\n");
  }
}

bool dispatch_reaction_event(Mezon__Api__MessageReaction *msg, const char *key, const char *topic) {
  if (!msg || !key || !topic) {
    return false;
  }

  size_t pb_size = mezon__api__message_reaction__get_packed_size(msg);
  if (pb_size == 0) {
    return true;
  }

  uint16_t key_len = (uint16_t)strlen(key);
  size_t total_size = 2 + key_len + pb_size;

  uint8_t *total_buf = mi_malloc(total_size);
  if (!total_buf) {
    return false;
  }

  total_buf[0] = (uint8_t)(key_len & 0xFF);
  total_buf[1] = (uint8_t)((key_len >> 8) & 0xFF);

  memcpy(total_buf + 2, key, key_len);

  mezon__api__message_reaction__pack(msg, total_buf + 2 + key_len);

  natsStatus s = natsConnection_Publish(global_nats_nc, topic, (const void *)total_buf, (int)total_size);

  mi_free(total_buf);

  if (s != NATS_OK) {
    log_error("NATS Publish failed: %s", natsStatus_GetText(s));
  }

  return s == NATS_OK;
}

bool dispatch_message_service(Mezon__Realtime__FcmDataPayload *msg, const char *key, const char *topic) {
  if (!msg || !key || !topic) {
    return false;
  }

  size_t pb_size = mezon__realtime__fcm_data_payload__get_packed_size(msg);
  if (pb_size == 0) {
    return true;
  }

  uint16_t key_len = (uint16_t)strlen(key);
  size_t total_size = 2 + key_len + pb_size;

  uint8_t *total_buf = mi_malloc(total_size);
  if (!total_buf) {
    log_error("dispatch_message_service: memory allocation failed");
    return false;
  }

  total_buf[0] = (uint8_t)(key_len & 0xFF);
  total_buf[1] = (uint8_t)((key_len >> 8) & 0xFF);

  memcpy(total_buf + 2, key, key_len);

  mezon__realtime__fcm_data_payload__pack(msg, total_buf + 2 + key_len);

  natsStatus s = natsConnection_Publish(global_nats_nc, topic, (const void *)total_buf, (int)total_size);

  mi_free(total_buf);

  if (s != NATS_OK) {
    log_error("NATS Publish failed for topic %s: %s", topic, natsStatus_GetText(s));
  }

  return s == NATS_OK;
}

uint64_t htobe64_custom(uint64_t value) {
  uint32_t high_part = htonl((uint32_t)(value >> 32));
  uint32_t low_part = htonl((uint32_t)(value & 0xFFFFFFFFLL));
  return ((uint64_t)low_part << 32) | high_part;
}

bool dispatch_api_request(uint16_t cid, int32_t sid, int64_t user_id, const char *username, int32_t api_index, const uint8_t *extra, size_t extra_len,
                          const uint8_t *payload, size_t payload_len) {
  log_debug("dispatch_api_request cid=%d, sid=%d api_index=%d", cid, sid, api_index);

  size_t user_len = username ? strlen(username) : 0;

  if (user_len > 255 || extra_len > 255) {
    log_error("Protocol violation: string too long (user=%zu, extra=%zu)", user_len, extra_len);
    return false;
  }

  if (api_index > API_INDEX_INVALID) {
    log_error("string too long: API=%zu, user=%zu, extra=%zu (max 255)", api_index, user_len, extra_len);
    return false;
  }

  // Base header: 2 (cid) + 4 (sid) + 8 (uid) + 1 (uLen) + 1 (eLen) = 16 bytes
  size_t header_size = 16;
  size_t total_size = header_size + user_len + 4 + extra_len + payload_len;

  uint8_t *buf = mi_malloc(total_size);
  if (!buf) {
    log_error("dispatch_api_request: memory allocation failed");
    return false;
  }

  uint16_t b_cid = htons(cid);
  uint32_t b_sid = htonl((uint32_t)sid);
  uint64_t b_uid = htobe64_custom((uint64_t)user_id);

  memcpy(buf + 0, &b_cid, 2);
  memcpy(buf + 2, &b_sid, 4);
  memcpy(buf + 6, &b_uid, 8);

  buf[14] = (uint8_t)user_len;
  buf[15] = (uint8_t)extra_len;

  size_t curr = header_size;

  if (user_len > 0) {
    memcpy(buf + curr, username, user_len);
    curr += user_len;
  }

  uint32_t b_idx = htonl((uint32_t)api_index);
  memcpy(buf + curr, &b_idx, 4);
  curr += 4;

  if (extra_len > 0) {
    memcpy(buf + curr, extra, extra_len);
    curr += extra_len;
  }

  if (payload_len > 0) {
    memcpy(buf + curr, payload, payload_len);
  }

  natsStatus s = natsConnection_Publish(global_nats_nc, API_REQUEST_EVENT, (const void *)buf, (int)total_size);

  mi_free(buf);

  if (s != NATS_OK) {
    log_error("NATS Publish failed for API request: %s", natsStatus_GetText(s));
    return false;
  }

  return s == NATS_OK;
}
