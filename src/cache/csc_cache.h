#ifndef MEZON_CSC_CACHE_H
#define MEZON_CSC_CACHE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <valkey/valkey.h>

#define CSC_CACHE_HIT 0
#define CSC_CACHE_LEADER 1
#define CSC_CACHE_WAITER 2

#define CSC_CACHE_DEFAULT_WAIT_SECONDS 5

typedef void (*csc_free_fn)(void *data);

typedef struct csc_cache_backend {
  void *(*get)(void *privdata, const char *ckey);
  int (*set)(void *privdata, const char *ckey, void *data, csc_free_fn free_data);
  void (*del)(void *privdata, const char *ckey);
  void (*clear)(void *privdata);
  void (*destroy)(void *privdata);
  csc_free_fn free_data;
  void *privdata;
} csc_cache_backend_t;

typedef struct csc_cache_entry {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  void *data;
  int err;
  int done;
  int64_t pxat;
  _Atomic int refcount;
} csc_cache_entry_t;

int csc_cache_entry_wait(csc_cache_entry_t *entry, const struct timespec *deadline, void **out);

typedef struct csc_cache csc_cache_t;

csc_cache_t *csc_cache_create(const csc_cache_backend_t *backend);
csc_cache_t *csc_cache_create_default(void);
void csc_cache_destroy(csc_cache_t *cache, int err);
int csc_cache_flight(csc_cache_t *cache, const char *key, const char *cmd, void **data_out, csc_cache_entry_t **entry_out);
int64_t csc_cache_update(csc_cache_t *cache, const char *key, const char *cmd, void *data);
int64_t csc_cache_update_owned(csc_cache_t *cache, const char *key, const char *cmd, void *data, csc_free_fn free_data);
void csc_cache_cancel(csc_cache_t *cache, const char *key, const char *cmd, int err);
void csc_cache_delete(csc_cache_t *cache, struct valkeyReply **keys, size_t nkeys);
void csc_cache_close(csc_cache_t *cache, int err);

#endif
