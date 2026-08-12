#include "producer.h"
#include <arpa/inet.h>
#include <mimalloc.h>
#include <nats/nats.h>
#include <stdint.h>
#include <string.h>
#include "util/log.h"

static natsConnection *global_nats_nc;

static void onDisconnect(natsConnection *, void *) { SFU_LOG_INFO("[Producer] Disconnected from NATS server!\n"); }

static void onReconnect(natsConnection *nc, void *) {
  char url_buffer[256];
  natsStatus st = natsConnection_GetConnectedUrl(nc, url_buffer, sizeof(url_buffer));
  if (st == NATS_OK) {
    SFU_LOG_INFO("[Producer] Reconnected to: %s\n", url_buffer);
  } else {
    SFU_LOG_ERROR("[Producer] Reconnected to NATS server (Could not fetch URL)\n");
  }
}

static void on_nats_error(__attribute__((unused)) natsConnection *nc, __attribute__((unused)) natsSubscription *sub, natsStatus stat,
                          __attribute__((unused)) void *closure) {
  SFU_LOG_ERROR("NATS Error: %s", natsStatus_GetText(stat));
  if (stat == NATS_SLOW_CONSUMER) {
    SFU_LOG_WARN("Worker is too slow! NATS is dropping messages.");
  }
}

natsStatus init_nats_connection(const char *url, const char *client_name) {
  natsOptions *opts = NULL;
  natsStatus st;

  st = natsOptions_Create(&opts);
  if (st != NATS_OK) {
    SFU_LOG_ERROR("Failed to create NATS options");
    return st;
  }

  natsOptions_SetURL(opts, url);
  natsOptions_SetName(opts, client_name);
  natsOptions_SetMaxReconnect(opts, -1);
  natsOptions_SetDisconnectedCB(opts, onDisconnect, NULL);
  natsOptions_SetReconnectedCB(opts, onReconnect, NULL);
  natsOptions_SetErrorHandler(opts, on_nats_error, NULL);

  st = natsConnection_Connect(&global_nats_nc, opts);
  natsOptions_Destroy(opts);

  if (st != NATS_OK) {
    SFU_LOG_ERROR("[NATS] Failed to connect: %s\n", natsStatus_GetText(st));
    return st;
  }

  SFU_LOG_ERROR("[NATS] Connected successfully.\n");

  return NATS_OK;
}

void cleanup_nats_connection() {
  if (global_nats_nc != NULL) {
    natsConnection_Close(global_nats_nc);
    natsConnection_Destroy(global_nats_nc);
    SFU_LOG_INFO("[Producer] NATS connection closed.\n");
  }
}

bool dispatch_hook_event(const void *msg, int len) {
  if (!msg || len <= 0) {
    return false;
  }
  if (global_nats_nc == NULL) {
    SFU_LOG_DEBUG("NATS Publish skipped: connection not ready");
    return false;
  }

  const char *topic = "SFU_HOOK_EVENT";
  natsStatus s = natsConnection_Publish(global_nats_nc, topic, (const void *)msg, len);

  if (s != NATS_OK) {
    SFU_LOG_ERROR("NATS Publish failed: %s", natsStatus_GetText(s));
  }

  return s == NATS_OK;
}
