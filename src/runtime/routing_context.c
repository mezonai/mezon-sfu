#include "runtime/routing_context.h"
#include <stdlib.h>
#include <string.h>
#include "peer/session.h"
#include "util/log.h"

static uint32_t routing_probe(sfu_routing_table_t *table, const char *client_ufrag, uint32_t hash, bool for_insert) {
  uint32_t mask = SFU_ROUTING_UFRAG_HASH_SLOTS - 1;
  uint32_t first_deleted = SFU_HASH_EMPTY;
  for (uint32_t probe = 0; probe < SFU_ROUTING_UFRAG_HASH_SLOTS; probe++) {
    uint32_t slot_index = (hash + probe) & mask;
    sfu_hash_slot_t *slot = &table->ufrag_index[slot_index];
    if (slot->index == SFU_HASH_EMPTY) {
      return for_insert && first_deleted != SFU_HASH_EMPTY ? first_deleted : slot_index;
    }
    if (slot->index == SFU_HASH_DELETED) {
      if (for_insert && first_deleted == SFU_HASH_EMPTY) {
        first_deleted = slot_index;
      }
      continue;
    }
    if (slot->hash == hash && slot->index < table->count && strcmp(table->entries[slot->index].ufrag, client_ufrag) == 0) {
      return slot_index;
    }
  }
  return for_insert ? first_deleted : SFU_HASH_EMPTY;
}

static bool index_entry_locked(sfu_routing_table_t *table, uint32_t entry_index) {
  sfu_routing_entry_t *entry = &table->entries[entry_index];
  uint32_t hash = fnv1a(entry->ufrag, strlen(entry->ufrag));
  uint32_t slot_index = routing_probe(table, entry->ufrag, hash, true);
  if (slot_index == SFU_HASH_EMPTY) {
    return false;
  }
  if (table->ufrag_index[slot_index].index == SFU_HASH_DELETED && table->deleted_slots > 0) {
    table->deleted_slots--;
  }
  table->ufrag_index[slot_index].hash = hash;
  table->ufrag_index[slot_index].index = entry_index;
  return true;
}

static bool rebuild_index_locked(sfu_routing_table_t *table) {
  for (uint32_t i = 0; i < SFU_ROUTING_UFRAG_HASH_SLOTS; i++) {
    table->ufrag_index[i].index = SFU_HASH_EMPTY;
  }
  table->deleted_slots = 0;
  for (uint32_t i = 0; i < table->count; i++) {
    if (!index_entry_locked(table, i)) {
      return false;
    }
  }
  return true;
}

int sfu_routing_table_init(sfu_routing_table_t *table) {
  if (!table) {
    return -1;
  }

  memset(table->entries, 0, sizeof(table->entries));
  for (uint32_t i = 0; i < SFU_ROUTING_UFRAG_HASH_SLOTS; i++) {
    table->ufrag_index[i].index = SFU_HASH_EMPTY;
  }
  table->count = 0;
  table->deleted_slots = 0;

  return pthread_mutex_init(&table->mutex, NULL) == 0 ? 0 : -1;
}

void sfu_routing_table_destroy(sfu_routing_table_t *table) {
  if (table) {
    pthread_mutex_destroy(&table->mutex);
  }
}

static sfu_routing_entry_t *find_entry_locked(sfu_routing_table_t *table, const char *client_ufrag) {
  uint32_t hash = fnv1a(client_ufrag, strlen(client_ufrag));
  uint32_t slot_index = routing_probe(table, client_ufrag, hash, false);
  if (slot_index == SFU_HASH_EMPTY) {
    return NULL;
  }
  uint32_t entry_index = table->ufrag_index[slot_index].index;
  return entry_index < table->count ? &table->entries[entry_index] : NULL;
}

static void remove_index_locked(sfu_routing_table_t *table, const char *client_ufrag, uint32_t expected_index) {
  uint32_t hash = fnv1a(client_ufrag, strlen(client_ufrag));
  uint32_t slot_index = routing_probe(table, client_ufrag, hash, false);
  if (slot_index != SFU_HASH_EMPTY && table->ufrag_index[slot_index].index == expected_index) {
    table->ufrag_index[slot_index].index = SFU_HASH_DELETED;
    table->deleted_slots++;
  }
}

static bool update_index_locked(sfu_routing_table_t *table, const char *client_ufrag, uint32_t old_index, uint32_t new_index) {
  uint32_t hash = fnv1a(client_ufrag, strlen(client_ufrag));
  uint32_t slot_index = routing_probe(table, client_ufrag, hash, false);
  if (slot_index == SFU_HASH_EMPTY || table->ufrag_index[slot_index].index != old_index) {
    return false;
  }
  table->ufrag_index[slot_index].index = new_index;
  return true;
}

static void remove_entry_at_locked(sfu_routing_table_t *table, uint32_t removed_index) {
  uint32_t last_index = table->count - 1;
  remove_index_locked(table, table->entries[removed_index].ufrag, removed_index);
  if (removed_index != last_index) {
    table->entries[removed_index] = table->entries[last_index];
    if (!update_index_locked(table, table->entries[removed_index].ufrag, last_index, removed_index)) {
      SFU_LOG_ERROR("routing ufrag index lost moved entry %s", table->entries[removed_index].ufrag);
      abort();
    }
  }
  table->count--;
  memset(&table->entries[table->count], 0, sizeof(table->entries[0]));
}

sfu_routing_register_result_t sfu_routing_table_prepare_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd,
                                                               const sfu_pending_answer_t *answer, sfu_routing_answer_reservation_t *reservation) {
  size_t ufrag_len;
  if (!table || !client_ufrag || !room || fd < 0 || !answer || !reservation ||
      (ufrag_len = strnlen(client_ufrag, sizeof(table->entries[0].ufrag))) == 0 || ufrag_len >= sizeof(table->entries[0].ufrag)) {
    return SFU_ROUTING_REGISTER_INVALID_ARGUMENT;
  }

  memset(reservation, 0, sizeof(*reservation));
  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, client_ufrag);
  if (entry && (entry->room != room || entry->fd != fd)) {
    pthread_mutex_unlock(&table->mutex);
    return SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT;
  }
  bool new_entry = entry == NULL;
  if (new_entry) {
    if (table->count >= SFU_MAX_UFRAG_MAPPINGS) {
      pthread_mutex_unlock(&table->mutex);
      return SFU_ROUTING_REGISTER_TABLE_FULL;
    }
    if (table->deleted_slots > table->count && !rebuild_index_locked(table)) {
      pthread_mutex_unlock(&table->mutex);
      return SFU_ROUTING_REGISTER_TABLE_FULL;
    }
    uint32_t entry_index = table->count;
    entry = &table->entries[entry_index];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->ufrag, client_ufrag, ufrag_len + 1);
    entry->room = room;
    entry->fd = fd;
    if (!index_entry_locked(table, entry_index)) {
      if (!rebuild_index_locked(table) || !index_entry_locked(table, entry_index)) {
        memset(entry, 0, sizeof(*entry));
        pthread_mutex_unlock(&table->mutex);
        return SFU_ROUTING_REGISTER_TABLE_FULL;
      }
    }
    table->count++;
  }

  uint32_t generation = entry->pending_answer.generation + 1;
  if (generation == 0) generation = 1;
  reservation->table = table;
  reservation->entry = entry;
  reservation->answer = *answer;
  reservation->generation = generation;
  reservation->entry_index = (uint32_t)(entry - table->entries);
  reservation->ufrag_len = ufrag_len;
  memcpy(reservation->ufrag, client_ufrag, ufrag_len + 1);
  reservation->room = room;
  reservation->fd = fd;
  reservation->new_entry = new_entry;
  reservation->active = true;
  pthread_mutex_unlock(&table->mutex);
  return SFU_ROUTING_REGISTER_OK;
}

bool sfu_routing_table_commit_answer(sfu_routing_answer_reservation_t *reservation, uint32_t *out_generation) {
  if (!reservation || !reservation->active || !reservation->table) return false;
  sfu_routing_table_t *table = reservation->table;
  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, reservation->ufrag);
  if (!entry || entry->room != reservation->room || entry->fd != reservation->fd ||
      (uint32_t)(entry - table->entries) != reservation->entry_index) {
    pthread_mutex_unlock(&table->mutex);
    reservation->active = false;
    return false;
  }
  sfu_pending_answer_t published = reservation->answer;
  if (entry->pending_answer.peer_id != 0) published.peer_id = entry->pending_answer.peer_id;
  published.generation = reservation->generation;
  published.valid = true;
  entry->room = reservation->room;
  entry->fd = reservation->fd;
  entry->pending_answer = published;
  if (out_generation) *out_generation = reservation->generation;
  reservation->active = false;
  pthread_mutex_unlock(&table->mutex);
  return true;
}

void sfu_routing_table_cancel_answer(sfu_routing_answer_reservation_t *reservation) {
  if (!reservation || !reservation->active || !reservation->table) return;
  sfu_routing_table_t *table = reservation->table;
  pthread_mutex_lock(&table->mutex);
  if (reservation->new_entry) {
    sfu_routing_entry_t *entry = find_entry_locked(table, reservation->ufrag);
    if (entry && (uint32_t)(entry - table->entries) == reservation->entry_index && !entry->pending_answer.valid &&
        entry->room == reservation->room && entry->fd == reservation->fd) {
      remove_entry_at_locked(table, reservation->entry_index);
      if (table->deleted_slots > table->count && !rebuild_index_locked(table)) {
        SFU_LOG_ERROR("routing ufrag index rebuild failed after reservation cancel");
      }
    }
  }
  pthread_mutex_unlock(&table->mutex);
  reservation->active = false;
}

sfu_routing_register_result_t sfu_routing_table_register_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd,
                                                                const sfu_pending_answer_t *answer, uint32_t *out_generation) {
  sfu_routing_answer_reservation_t reservation;
  sfu_routing_register_result_t result = sfu_routing_table_prepare_answer(table, client_ufrag, room, fd, answer, &reservation);
  if (result != SFU_ROUTING_REGISTER_OK) return result;
  if (!sfu_routing_table_commit_answer(&reservation, out_generation)) {
    sfu_routing_table_cancel_answer(&reservation);
    return SFU_ROUTING_REGISTER_TABLE_FULL;
  }
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

bool sfu_routing_table_peek_route(sfu_routing_table_t *table, const char *client_ufrag, sfu_routing_snapshot_t *out) {
  if (!table || !client_ufrag || !out) {
    return false;
  }
  pthread_mutex_lock(&table->mutex);
  sfu_routing_entry_t *entry = find_entry_locked(table, client_ufrag);
  if (!entry) {
    pthread_mutex_unlock(&table->mutex);
    return false;
  }
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
  for (uint32_t i = 0; i < table->count;) {
    if (table->entries[i].fd == fd) {
      remove_entry_at_locked(table, i);
    } else {
      i++;
    }
  }
  if (table->deleted_slots > table->count && !rebuild_index_locked(table)) {
    SFU_LOG_ERROR("routing ufrag index rebuild failed after fd removal");
    abort();
  }
  pthread_mutex_unlock(&table->mutex);
}
