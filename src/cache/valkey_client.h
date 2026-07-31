#ifndef VALKEY_CLIENT_H
#define VALKEY_CLIENT_H

#include <stdint.h>

#include <uv.h>
#include <valkey/async.h>
#include <valkey/valkey.h>
#include "csc_cache.h"

typedef struct valkey_client {
  valkeyAsyncContext *async;
  csc_cache_t *cache;
  uv_async_t inval_handle;
} valkey_client_t;

valkey_client_t *valkey_client_create(uv_loop_t *loop, const char *host, int port);
void valkey_client_destroy(valkey_client_t *client);


/* Each cqe_worker_thread calls valkey_plain_connect() once at startup and
   valkey_plain_disconnect() at exit (via pthread_cleanup_push).
   valkey_sync() returns the calling thread's own valkeyContext* — never
   shared, never locked. */
int valkey_plain_connect(const char *host, int port);
void valkey_plain_disconnect(void);
valkeyContext *valkey_sync(void);

/* If a worker needs to evict keys from the shared cache (e.g. after a
   write), call this. It enqueues the keys and calls uv_async_send() so
   the UV loop drains the queue on its own thread, where csc_cache_delete
   is safe to call. keys[] must be heap-allocated; ownership transfers. */
void valkey_post_invalidation(struct valkeyReply **keys, size_t nkeys);

#endif
