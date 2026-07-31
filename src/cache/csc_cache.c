#define _POSIX_C_SOURCE 200809L

#include "csc_cache.h"
#include <assert.h>
#include <errno.h>
#include <mimalloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "uthash.h"

typedef struct cmd_entry {
  char *cmd_entry_name;
  csc_cache_entry_t *entry;
  UT_hash_handle hh;
} cmd_entry_t;

typedef struct tracked_entry {
  char *tracked_entry_name;
  UT_hash_handle hh;
} tracked_entry_t;

typedef struct key_flights {
  char *kf_name;
  cmd_entry_t *cmds;
  UT_hash_handle hh;
} key_flights_t;

typedef struct key_tracked {
  char *key_tracked_name;
  tracked_entry_t *cmds;
  UT_hash_handle hh;
} key_tracked_t;

typedef struct default_store_entry {
  char *key;
  void *data;
  csc_free_fn free_data;
  struct default_store_entry *retired_next;
  UT_hash_handle hh;
} default_store_entry_t;

typedef struct default_store {
  default_store_entry_t *active;
  default_store_entry_t *retired;
} default_store_t;

struct csc_cache {
  pthread_rwlock_t mu;
  key_flights_t *flights;
  key_tracked_t *tracked;
  csc_cache_backend_t backend;
};

static void *default_get(void *privdata, const char *ckey) {
  default_store_t *store = privdata;
  default_store_entry_t *head = store ? store->active : NULL;
  default_store_entry_t *e = NULL;
  HASH_FIND_STR(head, ckey, e);
  return e ? e->data : NULL;
}

static void default_release_entry_data(default_store_entry_t *e) {
  if (e && e->data && e->free_data) {
    e->free_data(e->data);
  }
}

static void default_retire_entry(default_store_t *store, default_store_entry_t *e) {
  if (!store || !e) {
    return;
  }
  e->retired_next = store->retired;
  store->retired = e;
}

static void default_free_entry(default_store_entry_t *e) {
  if (!e) {
    return;
  }
  default_release_entry_data(e);
  mi_free(e->key);
  mi_free(e);
}

static int default_set(void *privdata, const char *ckey, void *data, csc_free_fn free_data) {
  default_store_t *store = privdata;
  default_store_entry_t *e = NULL;
  HASH_FIND_STR(store->active, ckey, e);
  if (e) {
    if (e->data && e->data != data && !e->free_data) {
      log_warn("[csc] replacing unowned cache data for key=%s", ckey);
    }
    if (e->data != data) {
      HASH_DEL(store->active, e);
      default_retire_entry(store, e);
      e = NULL;
    } else {
      e->free_data = free_data;
      return 0;
    }
  }
  e = mi_zalloc(sizeof *e);
  if (!e) {
    return ENOMEM;
  }
  e->key = mi_strdup(ckey);
  if (!e->key) {
    mi_free(e);
    return ENOMEM;
  }
  e->data = data;
  e->free_data = free_data;
  HASH_ADD_STR(store->active, key, e);
  return 0;
}

static void default_del(void *privdata, const char *ckey) {
  default_store_t *store = privdata;
  default_store_entry_t *e = NULL;
  HASH_FIND_STR(store->active, ckey, e);
  if (e) {
    HASH_DEL(store->active, e);
    default_retire_entry(store, e);
  }
}

static void default_clear(void *privdata) {
  default_store_t *store = privdata;

  default_store_entry_t *entries = store->active;
  store->active = NULL;

  int count = 0;
  default_store_entry_t *e, *tmp;
  HASH_ITER(hh, entries, e, tmp) {
    HASH_DEL(entries, e);
    default_retire_entry(store, e);
    count++;
  }
}

static void default_destroy(void *privdata) {
  default_store_t *store = privdata;

  default_store_entry_t *active = store->active;
  store->active = NULL;

  int active_count = 0;
  default_store_entry_t *e, *tmp;
  HASH_ITER(hh, active, e, tmp) {
    HASH_DEL(active, e);
    default_free_entry(e);
    active_count++;
  }

  int retired_count = 0;
  default_store_entry_t *retired = store->retired;
  store->retired = NULL;
  while (retired) {
    default_store_entry_t *next = retired->retired_next;
    default_free_entry(retired);
    retired = next;
    retired_count++;
  }

  mi_free(store);
}

static csc_cache_entry_t *entry_alloc(int64_t pxat) {
  csc_cache_entry_t *e = mi_zalloc(sizeof *e);
  if (!e) {
    return NULL;
  }
  e->pxat = pxat;
  pthread_mutex_init(&e->mu, NULL);
  pthread_cond_init(&e->cv, NULL);
  e->err = 0;
  atomic_init(&e->refcount, 1);  // in-flight registry reference
  return e;
}

static void entry_resolve_locked(csc_cache_entry_t *e, void *data, int err) {
  e->data = data;
  e->err = err;
  e->done = 1;
  pthread_cond_broadcast(&e->cv);
}

static void entry_free(csc_cache_entry_t *e) {
  if (!e) {
    return;
  }
  pthread_mutex_destroy(&e->mu);
  pthread_cond_destroy(&e->cv);
  mi_free(e);
}

static void entry_unref(csc_cache_entry_t *e) {
  if (!e) {
    return;
  }
  if (atomic_fetch_sub_explicit(&e->refcount, 1, memory_order_acq_rel) == 1) {
    entry_free(e);
  }
}

int csc_cache_entry_wait(csc_cache_entry_t *entry, const struct timespec *deadline, void **out) {
  int rc = 0;
  struct timespec fallback_deadline;
  const struct timespec *eff_deadline = deadline;

  /* Defence-in-depth: if caller didn't supply a deadline, install a
   * sensible default so a stalled/dead leader cannot hang us forever. */
  if (!eff_deadline) {
    if (clock_gettime(CLOCK_REALTIME, &fallback_deadline) == 0) {
      fallback_deadline.tv_sec += CSC_CACHE_DEFAULT_WAIT_SECONDS;
      eff_deadline = &fallback_deadline;
    }
  }

  pthread_mutex_lock(&entry->mu);
  while (!entry->done) {
    if (eff_deadline) {
      int r = pthread_cond_timedwait(&entry->cv, &entry->mu, eff_deadline);
      if (r == ETIMEDOUT) {
        rc = ETIMEDOUT;
        break;
      }
    } else {
      pthread_cond_wait(&entry->cv, &entry->mu);
    }
  }
  if (rc == 0) {
    rc = entry->err;
    *out = entry->data;
  } else {
    *out = NULL;
  }

  pthread_mutex_unlock(&entry->mu);

  entry_unref(entry);

  return rc;
}

static int composite_key(const char *key, const char *cmd, char *buf, size_t bufsz, char **allocated) {
  size_t klen = strlen(key);
  size_t clen = strlen(cmd);
  size_t need = klen + clen + 1;
  *allocated = NULL;
  if (need > bufsz) {
    *allocated = mi_malloc(need);
    if (!*allocated) {
      return -1;
    }
    buf = *allocated;
  }
  memcpy(buf, key, klen);
  memcpy(buf + klen, cmd, clen + 1);
  return (int)(need - 1);
}

static const char *const k_invalidate_suffixes[] = {
    "", "GET", "MGET", "SMEMBERS", "SCARD", "HGETALL", "EXISTS", "LRANGE0-1",
};
static const size_t k_invalidate_suffix_count = sizeof k_invalidate_suffixes / sizeof k_invalidate_suffixes[0];

static int should_track(const char *cmd) {
  size_t len = strlen(cmd);
  if (len < 4) {
    return 0;
  }
  if (memcmp(cmd, "SISM", 4) == 0) {
    return 1;
  }
  if (memcmp(cmd, "HGET", 4) == 0 && (len == 4 || cmd[4] != 'A')) {
    return 1;
  }
  return 0;
}

static void fail_cmd_entries(cmd_entry_t *cmds, int err) {
  cmd_entry_t *ce, *tmp;
  HASH_ITER(hh, cmds, ce, tmp) {
    if (ce->entry) {
      pthread_mutex_lock(&ce->entry->mu);
      entry_resolve_locked(ce->entry, NULL, err);
      pthread_mutex_unlock(&ce->entry->mu);
    }
  }
}

static void free_cmd_entries(cmd_entry_t *cmds) {
  cmd_entry_t *ce = cmds;
  if (cmds) {
    HASH_CLEAR(hh, cmds);  // free uthash bucket table; nodes walked via hh.next below
  }
  while (ce) {
    cmd_entry_t *tmp = (cmd_entry_t *)ce->hh.next;
    entry_unref(ce->entry);
    mi_free(ce->cmd_entry_name);
    mi_free(ce);
    ce = tmp;
  }
}

static void free_tracked_entries(tracked_entry_t *cmds) {
  tracked_entry_t *te = cmds;
  if (cmds) {
    HASH_CLEAR(hh, cmds);  // free uthash bucket table; nodes walked via hh.next below
  }
  while (te) {
    tracked_entry_t *tmp = (tracked_entry_t *)te->hh.next;
    mi_free(te->tracked_entry_name);
    mi_free(te);
    te = tmp;
  }
}

csc_cache_t *csc_cache_create(const csc_cache_backend_t *backend) {
  csc_cache_t *c = mi_zalloc(sizeof *c);
  if (!c) {
    return NULL;
  }
  if (pthread_rwlock_init(&c->mu, NULL) != 0) {
    mi_free(c);
    return NULL;
  }
  c->flights = NULL;
  c->tracked = NULL;
  c->backend = *backend;
  return c;
}

csc_cache_t *csc_cache_create_default(void) {
  default_store_t *store = mi_zalloc(sizeof *store);
  if (!store) {
    return NULL;
  }

  csc_cache_backend_t backend = {
      .get = default_get,
      .set = default_set,
      .del = default_del,
      .clear = default_clear,
      .destroy = default_destroy,
      .privdata = store,
  };

  csc_cache_t *c = csc_cache_create(&backend);
  if (!c) {
    mi_free(store);
  }
  return c;
}

void csc_cache_destroy(csc_cache_t *cache, int err) {
  csc_cache_close(cache, err);

  if (cache->backend.destroy) {
    cache->backend.destroy(cache->backend.privdata);
  }

  pthread_rwlock_destroy(&cache->mu);
  mi_free(cache);
}

#define CACHE_FIXED_KEY_MAX 256
int csc_cache_flight(csc_cache_t *cache, const char *key, const char *cmd, void **data_out, csc_cache_entry_t **entry_out) {
  assert(cache && key && cmd && data_out && entry_out);
  *data_out = NULL;
  *entry_out = NULL;

  char fixed[CACHE_FIXED_KEY_MAX];
  char *alloc = NULL;
  if (composite_key(key, cmd, fixed, CACHE_FIXED_KEY_MAX, &alloc) < 0) {
    return VALKEY_ERR;
  }
  const char *ckey = alloc ? alloc : fixed;

  pthread_rwlock_wrlock(&cache->mu);

  void *hit = cache->backend.get(cache->backend.privdata, ckey);
  if (hit) {
    mi_free(alloc);
    *data_out = hit;
    pthread_rwlock_unlock(&cache->mu);
    return CSC_CACHE_HIT;
  }

  key_flights_t *kf = NULL;
  HASH_FIND_STR(cache->flights, key, kf);

  if (kf) {
    cmd_entry_t *ce = NULL;
    HASH_FIND_STR(kf->cmds, cmd, ce);
    if (ce) {
      mi_free(alloc);
      atomic_fetch_add_explicit(&ce->entry->refcount, 1, memory_order_relaxed);
      *entry_out = ce->entry;
      pthread_rwlock_unlock(&cache->mu);
      return CSC_CACHE_WAITER;
    }
  } else {
    kf = mi_zalloc(sizeof *kf);
    if (!kf) {
      goto oom;
    }
    kf->kf_name = mi_strdup(key);
    if (!kf->kf_name) {
      mi_free(kf);
      goto oom;
    }
    kf->cmds = NULL;
    HASH_ADD_KEYPTR(hh, cache->flights, kf->kf_name, strlen(kf->kf_name), kf);
  }

  csc_cache_entry_t *entry = entry_alloc(0);
  if (!entry) {
    goto oom;
  }

  cmd_entry_t *ce = mi_zalloc(sizeof *ce);
  if (!ce) {
    entry_free(entry);
    goto oom;
  }
  ce->cmd_entry_name = mi_strdup(cmd);
  if (!ce->cmd_entry_name) {
    mi_free(ce);
    entry_free(entry);
    goto oom;
  }
  ce->entry = entry;
  HASH_ADD_KEYPTR(hh, kf->cmds, ce->cmd_entry_name, strlen(ce->cmd_entry_name), ce);

  mi_free(alloc);
  *entry_out = entry;
  pthread_rwlock_unlock(&cache->mu);
  return CSC_CACHE_LEADER;

oom:
  mi_free(alloc);
  pthread_rwlock_unlock(&cache->mu);
  return VALKEY_ERR;
}

int64_t csc_cache_update(csc_cache_t *cache, const char *key, const char *cmd, void *data) { return csc_cache_update_owned(cache, key, cmd, data, NULL); }

int64_t csc_cache_update_owned(csc_cache_t *cache, const char *key, const char *cmd, void *data, csc_free_fn free_data) {
  char fixed[CACHE_FIXED_KEY_MAX];
  char *alloc = NULL;
  // We need the ckey to save to the backend!
  if (composite_key(key, cmd, fixed, CACHE_FIXED_KEY_MAX, &alloc) < 0) {
    return -1;
  }
  const char *ckey = alloc ? alloc : fixed;
  csc_free_fn owner = free_data ? free_data : cache->backend.free_data;

  pthread_rwlock_wrlock(&cache->mu);

  key_flights_t *kf = NULL;
  HASH_FIND_STR(cache->flights, key, kf);
  if (kf) {
    cmd_entry_t *ce = NULL;
    HASH_FIND_STR(kf->cmds, cmd, ce);
    if (ce) {
      int set_rc = cache->backend.set(cache->backend.privdata, ckey, data, owner);

      pthread_mutex_lock(&ce->entry->mu);
      entry_resolve_locked(ce->entry, set_rc == 0 ? data : NULL, set_rc);
      pthread_mutex_unlock(&ce->entry->mu);

      if (set_rc != 0) {
        if (data && owner) {
          owner(data);
        }
        HASH_DEL(kf->cmds, ce);
        entry_unref(ce->entry);
        mi_free(ce->cmd_entry_name);
        mi_free(ce);
        if (kf->cmds == NULL) {
          HASH_DEL(cache->flights, kf);
          mi_free(kf->kf_name);
          mi_free(kf);
        }
        pthread_rwlock_unlock(&cache->mu);
        mi_free(alloc);
        return -set_rc;
      }

      if (should_track(cmd)) {
        key_tracked_t *kt = NULL;
        HASH_FIND_STR(cache->tracked, key, kt);
        if (!kt) {
          kt = mi_zalloc(sizeof(*kt));
          if (kt) {
            kt->key_tracked_name = mi_strdup(key);
            if (kt->key_tracked_name) {
              HASH_ADD_KEYPTR(hh, cache->tracked, kt->key_tracked_name, strlen(kt->key_tracked_name), kt);
            } else {
              log_warn("[csc] OOM tracking key=%s", key);
              mi_free(kt);
              kt = NULL;
            }
          }
        }
        if (kt) {
          tracked_entry_t *te = NULL;
          HASH_FIND_STR(kt->cmds, cmd, te);
          if (!te) {
            te = mi_zalloc(sizeof(*te));
            if (te) {
              te->tracked_entry_name = mi_strdup(cmd);
              if (te->tracked_entry_name) {
                HASH_ADD_KEYPTR(hh, kt->cmds, te->tracked_entry_name, strlen(te->tracked_entry_name), te);
              } else {
                log_warn("[csc] OOM tracking cmd=%s for key=%s", cmd, key);
                mi_free(te);
              }
            }
          }
        }
      }

      HASH_DEL(kf->cmds, ce);
      entry_unref(ce->entry);
      mi_free(ce->cmd_entry_name);
      mi_free(ce);
    }

    if (kf->cmds == NULL) {
      HASH_DEL(cache->flights, kf);
      mi_free(kf->kf_name);
      mi_free(kf);
    }
  }

  pthread_rwlock_unlock(&cache->mu);
  mi_free(alloc);
  return 0;
}

void csc_cache_cancel(csc_cache_t *cache, const char *key, const char *cmd, int err) {
  assert(cache && key && cmd);

  pthread_rwlock_wrlock(&cache->mu);

  key_flights_t *kf = NULL;
  HASH_FIND_STR(cache->flights, key, kf);
  if (kf) {
    cmd_entry_t *ce = NULL;
    HASH_FIND_STR(kf->cmds, cmd, ce);
    if (ce) {
      pthread_mutex_lock(&ce->entry->mu);
      entry_resolve_locked(ce->entry, NULL, err);
      pthread_mutex_unlock(&ce->entry->mu);
      HASH_DEL(kf->cmds, ce);
      entry_unref(ce->entry);
      mi_free(ce->cmd_entry_name);
      mi_free(ce);
    }

    if (kf->cmds == NULL) {
      HASH_DEL(cache->flights, kf);
      mi_free(kf->kf_name);
      mi_free(kf);
    }
  }

  pthread_rwlock_unlock(&cache->mu);
}

void csc_cache_delete(csc_cache_t *cache, struct valkeyReply **keys, size_t nkeys) {
  assert(cache);

  pthread_rwlock_wrlock(&cache->mu);

  // NULL keys or nkeys==0 means full flush (server tracking table overflow)
  if (!keys || nkeys == 0) {
    key_flights_t *flights = cache->flights;
    if (cache->flights) {
      HASH_CLEAR(hh, cache->flights);
    }
    key_flights_t *kf = flights;
    while (kf) {
      key_flights_t *kftmp = (key_flights_t *)kf->hh.next;
      fail_cmd_entries(kf->cmds, ECANCELED);
      free_cmd_entries(kf->cmds);
      mi_free(kf->kf_name);
      mi_free(kf);
      kf = kftmp;
    }

    key_tracked_t *tracked = cache->tracked;
    if (cache->tracked) {
      HASH_CLEAR(hh, cache->tracked);
    }
    key_tracked_t *kt = tracked;
    while (kt) {
      key_tracked_t *kttmp = (key_tracked_t *)kt->hh.next;
      free_tracked_entries(kt->cmds);
      mi_free(kt->key_tracked_name);
      mi_free(kt);
      kt = kttmp;
    }

    cache->backend.clear(cache->backend.privdata);
    pthread_rwlock_unlock(&cache->mu);
    return;
  }

  for (size_t ki = 0; ki < nkeys; ki++) {
    struct valkeyReply *kr = keys[ki];
    if (!kr || kr->type != VALKEY_REPLY_STRING || !kr->str || kr->len == 0) {
      continue;
    }

    const char *k = kr->str;
    size_t klen = (size_t)kr->len;

    // delete all static suffix combos from backend
    for (size_t si = 0; si < k_invalidate_suffix_count; si++) {
      const char *suf = k_invalidate_suffixes[si];
      size_t slen = strlen(suf);
      size_t need = klen + slen + 1;

      char fixed[CACHE_FIXED_KEY_MAX];
      char *a = NULL;
      char *ckey;

      if (need <= sizeof fixed) {
        ckey = fixed;
      } else {
        a = mi_malloc(need);
        if (!a) {
          log_warn("[csc] OOM building ckey for key=%.*s suf=%s", (int)klen, k, suf);
          continue;
        }
        ckey = a;
      }
      memcpy(ckey, k, klen);
      memcpy(ckey + klen, suf, slen + 1);
      cache->backend.del(cache->backend.privdata, ckey);
      mi_free(a);
    }

    // delete tracked dynamic cmd entries (HGET field, SISMEMBER member)
    key_tracked_t *kt = NULL;
    HASH_FIND_STR(cache->tracked, k, kt);
    if (kt) {
      tracked_entry_t *te, *tetmp;
      HASH_ITER(hh, kt->cmds, te, tetmp) {
        const char *cmd = te->tracked_entry_name;
        size_t clen = strlen(cmd);
        size_t need = klen + clen + 1;

        char fixed[CACHE_FIXED_KEY_MAX];
        char *a = NULL;
        char *ckey;

        if (need <= sizeof fixed) {
          ckey = fixed;
        } else {
          a = mi_malloc(need);
          if (!a) {
            log_warn("[csc] OOM building ckey for tracked key=%.*s cmd=%s", (int)klen, k, cmd);
            /* still remove the tracked entry even if we can't del from backend */
            HASH_DEL(kt->cmds, te);
            mi_free(te->tracked_entry_name);
            mi_free(te);
            continue;
          }
          ckey = a;
        }
        memcpy(ckey, k, klen);
        memcpy(ckey + klen, cmd, clen + 1);
        cache->backend.del(cache->backend.privdata, ckey);
        mi_free(a);

        HASH_DEL(kt->cmds, te);
        mi_free(te->tracked_entry_name);
        mi_free(te);
      }
      HASH_DEL(cache->tracked, kt);
      mi_free(kt->key_tracked_name);
      mi_free(kt);
    }

    // cancel any in-flight requests for this key
    key_flights_t *kf = NULL;
    HASH_FIND_STR(cache->flights, k, kf);
    if (kf) {
      cmd_entry_t *ce, *cetmp;
      HASH_ITER(hh, kf->cmds, ce, cetmp) {
        const char *cmd = ce->cmd_entry_name;
        size_t clen = strlen(cmd);
        size_t need = klen + clen + 1;

        char fixed[CACHE_FIXED_KEY_MAX];
        char *a = NULL;
        char *ckey;

        if (need <= sizeof fixed) {
          ckey = fixed;
        } else {
          a = mi_malloc(need);
          if (!a) {
            log_warn("[csc] OOM building ckey for flight key=%.*s cmd=%s", (int)klen, k, cmd);
            // still cancel the waiter even if backend del fails
            goto cancel;
          }
          ckey = a;
        }
        memcpy(ckey, k, klen);
        memcpy(ckey + klen, cmd, clen + 1);
        cache->backend.del(cache->backend.privdata, ckey);
        mi_free(a);

      cancel:
        if (ce->entry) {
          pthread_mutex_lock(&ce->entry->mu);
          entry_resolve_locked(ce->entry, NULL, ECANCELED);
          pthread_mutex_unlock(&ce->entry->mu);
        }
        HASH_DEL(kf->cmds, ce);
        entry_unref(ce->entry);
        mi_free(ce->cmd_entry_name);
        mi_free(ce);
      }
      HASH_DEL(cache->flights, kf);
      mi_free(kf->kf_name);
      mi_free(kf);
    }

    log_debug("[csc] invalidated key=%.*s", (int)klen, k);
  }

  pthread_rwlock_unlock(&cache->mu);
}

void csc_cache_close(csc_cache_t *cache, int err) {
  assert(cache);

  pthread_rwlock_wrlock(&cache->mu);

  if (cache->flights == NULL && cache->tracked == NULL) {
    log_warn("[csc] csc_cache_close: nothing to close (flights and tracked already NULL, cache was flushed or never populated) cache=%p", (void *)cache);
    pthread_rwlock_unlock(&cache->mu);
    return;
  }

  key_flights_t *flights = cache->flights;
  if (cache->flights) {
    HASH_CLEAR(hh, cache->flights);
  }
  key_flights_t *kf = flights;
  while (kf) {
    key_flights_t *kftmp = (key_flights_t *)kf->hh.next;
    fail_cmd_entries(kf->cmds, err);
    free_cmd_entries(kf->cmds);
    mi_free(kf->kf_name);
    mi_free(kf);
    kf = kftmp;
  }

  key_tracked_t *tracked = cache->tracked;
  if (cache->tracked) {
    HASH_CLEAR(hh, cache->tracked);
  }
  key_tracked_t *kt = tracked;
  while (kt) {
    key_tracked_t *kttmp = (key_tracked_t *)kt->hh.next;
    free_tracked_entries(kt->cmds);
    mi_free(kt->key_tracked_name);
    mi_free(kt);
    kt = kttmp;
  }

  cache->backend.clear(cache->backend.privdata);

  pthread_rwlock_unlock(&cache->mu);
}
