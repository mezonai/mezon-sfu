#include "valkey_client.h"
#include <mimalloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <valkey/adapters/libuv.h>
#include "config.h"
#include "log.h"

#define INVAL_RING_SIZE 256 /* must be power of 2 */
#define INVAL_RING_MASK (INVAL_RING_SIZE - 1)

typedef struct {
  struct valkeyReply **keys;
  size_t nkeys;
} inval_msg_t;

static inval_msg_t g_inval_ring[INVAL_RING_SIZE];
static _Atomic uint32_t g_inval_head = 0; /* drained by UV loop */
static _Atomic uint32_t g_inval_tail = 0; /* advanced by producers */

static pthread_key_t sync_context_key;
static pthread_once_t sync_context_key_once = PTHREAD_ONCE_INIT;
static int sync_context_key_status = -1;
static uv_async_t *g_inval_handle = NULL;

static void sync_context_destroy(void *ptr) {
  if (ptr) {
    valkeyFree(ptr);
  }
}

static void sync_context_key_init(void) { sync_context_key_status = pthread_key_create(&sync_context_key, sync_context_destroy); }

static valkeyContext *sync_context_get(void) {
  pthread_once(&sync_context_key_once, sync_context_key_init);
  if (sync_context_key_status != 0) {
    return NULL;
  }
  return pthread_getspecific(sync_context_key);
}

static bool sync_context_set(valkeyContext *ctx) {
  pthread_once(&sync_context_key_once, sync_context_key_init);
  if (sync_context_key_status != 0) {
    return false;
  }
  return pthread_setspecific(sync_context_key, ctx) == 0;
}

static bool sync_context_clear_and_free(valkeyContext *ctx) {
  if (!ctx) {
    return true;
  }
  if (ctx && sync_context_set(NULL)) {
    valkeyFree(ctx);
    return true;
  }
  return false;
}

static void __attribute__((unused)) on_tracking_reply(valkeyAsyncContext *ac, void *reply, void *privdata) {
  (void)ac;
  (void)privdata;
  valkeyReply *r = reply;
  if (!r) {
    log_error("[valkey] TRACKING reply is NULL");
    return;
  }

  log_debug("[valkey] TRACKING reply type=%d str=%s", r->type, r->type == VALKEY_REPLY_STATUS ? r->str : "?");
}

static void on_hello(valkeyAsyncContext *ac, void *reply, void *privdata) {
  log_debug("[valkey] HELLO reply received %p %p", reply, privdata);
  int ret = valkeyAsyncCommand(ac, on_tracking_reply, NULL, "CLIENT TRACKING ON BCAST");
  if (ret != VALKEY_OK) {
    log_error("[valkey] CLIENT TRACKING ON BCAST failed: %s", ac->errstr);
  } else {
    log_info("[valkey] CLIENT TRACKING ON BCAST enqueued ret=%d", ret);
  }
}

static void on_inval_handle(uv_async_t *handle) {
  csc_cache_t *cache = handle->data;
  uint32_t tail = atomic_load_explicit(&g_inval_tail, memory_order_acquire);
  uint32_t head = atomic_load_explicit(&g_inval_head, memory_order_relaxed);
  while (head != tail) {
    inval_msg_t *m = &g_inval_ring[head & INVAL_RING_MASK];
    if (cache) {
      // csc_cache_delete is only ever called here — UV loop thread
      csc_cache_delete(cache, m->keys, m->nkeys);
    }
    mi_free(m->keys);  // keys array itself; entries are valkeyReply* owned by caller
    head++;
    atomic_store_explicit(&g_inval_head, head, memory_order_release);
    tail = atomic_load_explicit(&g_inval_tail, memory_order_acquire);
  }
}

static void on_invalidation_msg(valkeyAsyncContext *ac, void *reply) {
  valkey_client_t *client = ac->data;
  if (!client || !client->cache) {
    return;
  }
  csc_cache_t *cache = client->cache;
  valkeyReply *r = reply;
  if (!r) {
    log_debug("on_invalidation_msg: reply is NULL");
    return;
  }
  if ((r->type != VALKEY_REPLY_ARRAY && r->type != VALKEY_REPLY_PUSH) || r->elements < 2) {
    log_debug("on_invalidation_msg: unexpected type=%d elements=%zu", r->type, r->elements);
    return;
  }
  valkeyReply *keys = r->element[1];
  if (!keys) {
    log_debug("on_invalidation_msg: keys element is NULL");
    return;
  }
  if (keys->type == VALKEY_REPLY_NIL) {
    log_debug("on_invalidation_msg: [ALL KEYS CLEARED / FLUSHED]");
    return;
  }
  if (keys->type != VALKEY_REPLY_ARRAY) {
    log_debug("on_invalidation_msg: unexpected keys type=%d", keys->type);
    return;
  }

#if defined(DEBUG)
  log_info("on_invalidation_msg: received %zu key(s) to invalidate", keys->elements);
  for (size_t i = 0; i < keys->elements; i++) {
    valkeyReply *key_element = keys->element[i];
    if (!key_element) {
      log_info("  -> key[%zu]: NULL", i);
    } else if (key_element->type == VALKEY_REPLY_STRING) {
      log_info("  -> key[%zu]: %s", i, key_element->str);
    } else if (key_element->type == VALKEY_REPLY_NIL) {
      log_info("  -> key[%zu]: [NIL]", i);
    } else {
      log_info("  -> key[%zu]: unexpected type=%d", i, key_element->type);
    }
  }
#endif

  csc_cache_delete(cache, keys->element, keys->elements);
}

static void on_connect(valkeyAsyncContext *ac, int status) {
  log_info("[valkey] on_connect fired status=%d err=%s", status, ac->errstr);

  if (status != VALKEY_OK) {
    log_error("[valkey] async connect error: %s", ac->errstr);
    return;
  }

  valkeyAsyncCommand((valkeyAsyncContext *)ac, on_hello, NULL, "HELLO 3");
}

static void on_disconnect(const valkeyAsyncContext *ac, int status) {
  (void)status;
  valkey_client_t *client = (valkey_client_t *)ac->data;

  if (client && client->cache) {
    // Null the inval handle data first so on_inval_handle (UV loop thread,
    // same thread as on_disconnect) won't touch the cache after we free it.
    client->inval_handle.data = NULL;
    csc_cache_t *cache = client->cache;
    client->cache = NULL;
    csc_cache_destroy(cache, ECONNRESET);
  }

  client->async = NULL;
}

static pthread_mutex_t g_inval_mutex = PTHREAD_MUTEX_INITIALIZER;

void valkey_post_invalidation(struct valkeyReply **keys, size_t nkeys) {
  pthread_mutex_lock(&g_inval_mutex);

  uv_async_t *handle = g_inval_handle;
  if (!handle) {
    pthread_mutex_unlock(&g_inval_mutex);
    mi_free(keys);
    return;
  }

  uint32_t tail = atomic_load_explicit(&g_inval_tail, memory_order_relaxed);
  inval_msg_t *m = &g_inval_ring[tail & INVAL_RING_MASK];
  m->keys = keys;
  m->nkeys = nkeys;

  // Advance tail ONLY after data is safely written
  atomic_store_explicit(&g_inval_tail, tail + 1, memory_order_release);

  pthread_mutex_unlock(&g_inval_mutex);
  uv_async_send(handle);
}

int valkey_plain_connect(const char *host, int port) {
  struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
  valkeyContext *ctx = valkeyConnectWithTimeout(host, port, tv);
  if (!ctx || ctx->err) {
    log_error("valkey_plain_connect failed: %s", ctx ? ctx->errstr : "alloc failure");
    if (ctx) {
      valkeyFree(ctx);
    }
    return -1;
  }

  valkey_plain_disconnect();
  if (!sync_context_set(ctx)) {
    valkeyFree(ctx);
    return -1;
  }
  return 0;
}

void valkey_plain_disconnect(void) {
  valkeyContext *ctx = sync_context_get();
  sync_context_clear_and_free(ctx);
}

valkeyContext *valkey_sync(void) {
  valkeyContext *ctx = sync_context_get();
  if (ctx && ctx->err) {
    if (!sync_context_clear_and_free(ctx)) {
      return NULL;
    }
    ctx = NULL;
  }
  if (!ctx) {
    // attempt reconnect — or just return NULL and let caller handle
    struct timeval tv = {2, 0};
    ctx = valkeyConnectWithTimeout(cfg->redis.host, 6379, tv);
    if (ctx && ctx->err) {
      valkeyFree(ctx);
      ctx = NULL;
    }
    if (ctx && !sync_context_set(ctx)) {
      valkeyFree(ctx);
      ctx = NULL;
    }
  }
  return ctx;
}

valkey_client_t *valkey_client_create(uv_loop_t *loop, const char *host, int port) {
  valkey_client_t *c = mi_zalloc(sizeof *c);
  if (!c) {
    return NULL;
  }

  c->async = valkeyAsyncConnect(host, port);
  if (!c->async || c->async->err) {
    log_error("valkeyAsyncConnect: %s", c->async ? c->async->errstr : "alloc");
    goto fail;
  }

  c->async->data = c;

  // subscribe to CLIENT TRACKING push messages
  valkeyAsyncSetPushCallback(c->async, on_invalidation_msg);

  valkeyLibuvAttach(c->async, loop);

  valkeyAsyncSetConnectCallback(c->async, on_connect);
  valkeyAsyncSetDisconnectCallback(c->async, on_disconnect);

  /* shared in-process cache */
  c->cache = csc_cache_create_default();
  if (!c->cache) {
    goto fail;
  }

  // async handle so workers can safely trigger cache invalidation
  uv_async_init(loop, &c->inval_handle, on_inval_handle);
  c->inval_handle.data = c->cache;
  g_inval_handle = &c->inval_handle;

  return c;

fail:
  if (c->async) {
    valkeyAsyncFree(c->async);
  }
  if (c->cache) {
    csc_cache_destroy(c->cache, ECANCELED);
  }
  mi_free(c);
  return NULL;
}

void valkey_client_destroy(valkey_client_t *client) {
  if (!client) {
    return;
  }

  pthread_mutex_lock(&g_inval_mutex);
  g_inval_handle = NULL;
  pthread_mutex_unlock(&g_inval_mutex);
  client->inval_handle.data = NULL;
  csc_cache_t *cache = client->cache;
  client->cache = NULL;

  if (client->async) {
    valkeyAsyncFree(client->async);
    client->async = NULL;
  }

  if (cache) {
    csc_cache_destroy(cache, 0);
  }
  mi_free(client);
}
