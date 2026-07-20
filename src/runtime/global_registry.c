#include "runtime/global_registry.h"
#include <pthread.h>
#include <string.h>
#include "util/log.h"

#define MAX_GLOBAL_SESSIONS 2048

typedef struct {
  char ufrag[32];
  uint32_t worker_index;
  bool active;
} global_session_entry_t;

static global_session_entry_t g_registry[MAX_GLOBAL_SESSIONS];
static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;

void sfu_global_registry_init(void) {
  pthread_rwlock_wrlock(&g_registry_lock);
  memset(g_registry, 0, sizeof(g_registry));
  pthread_rwlock_unlock(&g_registry_lock);
}

bool sfu_global_registry_lookup(const char *ufrag, uint32_t *out_worker_index) {
  bool found = false;

  // Use a shared Read Lock so multiple workers can query concurrently without blocking
  pthread_rwlock_rdlock(&g_registry_lock);
  for (int i = 0; i < MAX_GLOBAL_SESSIONS; i++) {
    if (g_registry[i].active && strcmp(g_registry[i].ufrag, ufrag) == 0) {
      *out_worker_index = g_registry[i].worker_index;
      found = true;
      break;
    }
  }
  pthread_rwlock_unlock(&g_registry_lock);
  return found;
}

void sfu_global_registry_register_owner(const char *ufrag, uint32_t worker_index) {
  // Use an Exclusive Write Lock since we are mutating the registry
  pthread_rwlock_wrlock(&g_registry_lock);

  // Check if it already exists to perform an update
  for (int i = 0; i < MAX_GLOBAL_SESSIONS; i++) {
    if (g_registry[i].active && strcmp(g_registry[i].ufrag, ufrag) == 0) {
      g_registry[i].worker_index = worker_index;
      pthread_rwlock_unlock(&g_registry_lock);
      return;
    }
  }

  // Otherwise, find the first available free slot
  for (int i = 0; i < MAX_GLOBAL_SESSIONS; i++) {
    if (!g_registry[i].active) {
      strncpy(g_registry[i].ufrag, ufrag, sizeof(g_registry[i].ufrag) - 1);
      g_registry[i].ufrag[sizeof(g_registry[i].ufrag) - 1] = '\0';
      g_registry[i].worker_index = worker_index;
      g_registry[i].active = true;
      pthread_rwlock_unlock(&g_registry_lock);
      return;
    }
  }

  pthread_rwlock_unlock(&g_registry_lock);
  SFU_LOG_ERROR("Global session registry is completely FULL! Cannot register ufrag: %s", ufrag);
}

void sfu_global_registry_unregister(const char *ufrag) {
  pthread_rwlock_wrlock(&g_registry_lock);
  for (int i = 0; i < MAX_GLOBAL_SESSIONS; i++) {
    if (g_registry[i].active && strcmp(g_registry[i].ufrag, ufrag) == 0) {
      g_registry[i].active = false;
      memset(g_registry[i].ufrag, 0, sizeof(g_registry[i].ufrag));
      break;
    }
  }
  pthread_rwlock_unlock(&g_registry_lock);
}
