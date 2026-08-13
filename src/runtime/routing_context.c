#include "runtime/routing_context.h"
#include <stdio.h>
#include <string.h>
#include "peer/session.h"
#include "util/log.h"

int sfu_routing_table_init(sfu_routing_table_t *table) {
  if (!table) {
    return -1;
  }

  memset(table->entries, 0, sizeof(table->entries));
  table->count = 0;

  return pthread_mutex_init(&table->mutex, NULL) == 0 ? 0 : -1;
}

void sfu_routing_table_destroy(sfu_routing_table_t *table) {
  if (table) {
    pthread_mutex_destroy(&table->mutex);
  }
}

static sfu_routing_entry_t *find_entry_locked(sfu_routing_table_t *table, const char *client_ufrag) {
  for (int i = 0; i < table->count; i++) {
    if (strcmp(table->entries[i].ufrag, client_ufrag) == 0) {
      return &table->entries[i];
    }
  }
  return NULL;
}

sfu_routing_register_result_t sfu_routing_table_register_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd,
                                                                 const sfu_pending_answer_t *answer, uint32_t *out_generation) {
  if (!table || !client_ufrag || client_ufrag[0] == '\0' || !room || fd < 0 || !answer) {
    return SFU_ROUTING_REGISTER_INVALID_ARGUMENT;
  }

  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, client_ufrag);
  if (entry && (entry->room != room || entry->fd != fd)) {
    pthread_mutex_unlock(&table->mutex);
    SFU_LOG_WARN("rejecting route ownership change for ufrag=%s (fd %d -> %d)", client_ufrag, entry->fd, fd);
    return SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT;
  }
  if (!entry) {
    if (table->count >= SFU_MAX_UFRAG_MAPPINGS) {
      pthread_mutex_unlock(&table->mutex);
      SFU_LOG_ERROR("ufrag->room table FULL. Cannot register ufrag=%s", client_ufrag);
      return SFU_ROUTING_REGISTER_TABLE_FULL;
    }
    entry = &table->entries[table->count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->ufrag, sizeof(entry->ufrag), "%s", client_ufrag);
    entry->fd = -1;
  }

  uint32_t generation = entry->pending_answer.generation + 1;
  if (generation == 0) {
    generation = 1;
  }

  sfu_pending_answer_t published = *answer;
  if (entry->pending_answer.peer_id != 0) {
    published.peer_id = entry->pending_answer.peer_id;
  }
  published.generation = generation;
  published.valid = true;

  entry->room = room;
  entry->fd = fd;
  entry->pending_answer = published;

  if (out_generation) {
    *out_generation = generation;
  }
  pthread_mutex_unlock(&table->mutex);
  return SFU_ROUTING_REGISTER_OK;
}

bool sfu_routing_table_lookup_route(sfu_routing_table_t *table, const char *client_ufrag, uint32_t worker_index, sfu_routing_snapshot_t *out) {
  if (!table || !client_ufrag || !out) {
    return false;
  }

  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, client_ufrag);
  if (!entry) {
    pthread_mutex_unlock(&table->mutex);
    return false;
  }

  entry->worker_index = worker_index;
  entry->has_owner = true;
  out->room = entry->room;
  out->worker_index = entry->worker_index;
  out->fd = entry->fd;
  out->has_owner = entry->has_owner;
  out->pending_generation = entry->pending_answer.valid ? entry->pending_answer.generation : 0;
  pthread_mutex_unlock(&table->mutex);
  return true;
}

bool sfu_routing_table_reconcile_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd, uint32_t generation,
                                        sfu_peer_session_t *session, bool *role_changed, bool *media_changed) {
  if (!table || !client_ufrag || !room || fd < 0 || generation == 0 || !session) {
    return false;
  }

  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, client_ufrag);
  if (!entry || entry->room != room || entry->fd != fd || !entry->pending_answer.valid || entry->pending_answer.generation != generation) {
    pthread_mutex_unlock(&table->mutex);
    return false;
  }

  sfu_pending_answer_t pending = entry->pending_answer;
  pthread_mutex_unlock(&table->mutex);

  if (session->room && session->room != room) {
    SFU_LOG_WARN("answer reconciliation rejected for ufrag=%s generation=%u: session belongs to another room", client_ufrag, generation);
    return false;
  }

  bool applied = sfu_session_apply_pending_answer(session, &pending, fd, role_changed, media_changed);
  if (!applied) {
    SFU_LOG_DEBUG("answer reconciliation skipped for ufrag=%s generation=%u: session already applied this or a newer answer", client_ufrag, generation);
    return false;
  }

  pthread_mutex_lock(&table->mutex);
  entry = find_entry_locked(table, client_ufrag);
  if (entry && entry->room == room && entry->fd == fd && entry->pending_answer.valid && entry->pending_answer.generation == generation) {
    entry->pending_answer.valid = false;
  }
  pthread_mutex_unlock(&table->mutex);
  return true;
}

bool sfu_routing_table_invalidate_pending(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd, uint32_t *out_generation) {
  if (!table || !client_ufrag || !room || fd < 0) {
    return false;
  }
  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, client_ufrag);
  if (!entry || entry->room != room || entry->fd != fd) {
    pthread_mutex_unlock(&table->mutex);
    return false;
  }
  uint32_t generation = entry->pending_answer.generation + 1;
  if (generation == 0) {
    generation = 1;
  }
  entry->pending_answer.generation = generation;
  entry->pending_answer.valid = false;
  if (out_generation) {
    *out_generation = generation;
  }
  pthread_mutex_unlock(&table->mutex);
  return true;
}

void sfu_routing_table_unregister_fd(sfu_routing_table_t *table, int fd) {
  if (!table) {
    return;
  }
  pthread_mutex_lock(&table->mutex);
  for (int i = 0; i < table->count;) {
    if (table->entries[i].fd == fd) {
      table->entries[i] = table->entries[--table->count];
      memset(&table->entries[table->count], 0, sizeof(table->entries[0]));
    } else {
      i++;
    }
  }
  pthread_mutex_unlock(&table->mutex);
}
