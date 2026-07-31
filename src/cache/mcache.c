#include "mcache.h"
#include <errno.h>
#include <mimalloc.h>
#include <stdlib.h>
#include <string.h>
#include "api.pb-c.h"
#include "csc_cache.h"
#include "log.h"
#include "realtime.pb-c.h"

extern valkey_client_t *global_valkey;

static void free_cached_channel_description(void *data) { mezon__api__channel_description__free_unpacked((Mezon__Api__ChannelDescription *)data, NULL); }

static void free_cached_channel_message_header(void *data) {
  mezon__api__channel_message_header__free_unpacked((Mezon__Api__ChannelMessageHeader *)data, NULL);
}

static void free_cached_clan_desc(void *data) { mezon__api__clan_desc__free_unpacked((Mezon__Api__ClanDesc *)data, NULL); }

static void free_cached_clan_desc_list(void *data) { mezon__api__clan_desc_list__free_unpacked((Mezon__Api__ClanDescList *)data, NULL); }

static void free_cached_clan_profile(void *data) { mezon__api__clan_profile__free_unpacked((Mezon__Api__ClanProfile *)data, NULL); }

static void free_cached_user_profile(void *data) { mezon__realtime__user_profile_redis__free_unpacked((Mezon__Realtime__UserProfileRedis *)data, NULL); }

static void free_cached_channel_desc_list(void *data) { mezon__api__channel_desc_list__free_unpacked((Mezon__Api__ChannelDescList *)data, NULL); }

static void free_cached_friend_list(void *data) { mezon__api__friend_list__free_unpacked((Mezon__Api__FriendList *)data, NULL); }

static void free_cached_clan_user_list(void *data) { mezon__api__clan_user_list__free_unpacked((Mezon__Api__ClanUserList *)data, NULL); }

static void free_cached_valkey_reply(void *data) { freeReplyObject(data); }

static void free_cached_channel_id_list(void *data) {
  channel_id_list_t *list = data;
  if (!list) {
    return;
  }
  for (size_t i = 0; i < list->n_channels; i++) {
    mi_free((void *)list->channels[i]);
  }
  mi_free((void *)list->channels);
  mi_free(list);
}

static void free_cached_clan_badge_count(void *data) { free_clan_badge_count((clan_badge_count_t *)data); }

Mezon__Api__ChannelDescription *channel_desc_from_reply(valkeyReply *reply) {
  if (!reply || reply->type != VALKEY_REPLY_STRING) {
    return NULL;
  }

  Mezon__Api__ChannelDescription *ch = mezon__api__channel_description__unpack(NULL, reply->len, (const uint8_t *)reply->str);

  if (!ch) {
    log_error("Failed to unpack ChannelDescription Protobuf");
    return NULL;
  }

  return ch;
}

Mezon__Api__ChannelMessageHeader *msg_header_from_reply(valkeyReply *reply) {
  if (!reply || reply->type != VALKEY_REPLY_STRING) {
    return NULL;
  }

  Mezon__Api__ChannelMessageHeader *header = mezon__api__channel_message_header__unpack(NULL, reply->len, (const uint8_t *)reply->str);

  if (!header) {
    log_error("Failed to unpack ChannelMessageHeader Protobuf");
    return NULL;
  }

  return header;
}

Mezon__Api__ClanDesc *clan_desc_from_reply(valkeyReply *reply) {
  if (!reply || reply->type != VALKEY_REPLY_STRING) {
    return NULL;
  }

  Mezon__Api__ClanDesc *clan = mezon__api__clan_desc__unpack(NULL, reply->len, (const uint8_t *)reply->str);

  if (!clan) {
    log_error("Failed to unpack ClanDesc Protobuf");
    return NULL;
  }

  return clan;
}

Mezon__Api__ClanProfile *clan_profile_from_reply(valkeyReply *r) {
  if (r->type != VALKEY_REPLY_STRING) {
    return NULL;
  }
  return mezon__api__clan_profile__unpack(NULL, r->len, (const uint8_t *)r->str);
}

Mezon__Realtime__UserProfileRedis *user_profile_from_reply(valkeyReply *reply) {
  if (!reply || reply->type != VALKEY_REPLY_STRING) {
    return NULL;
  }

  Mezon__Realtime__UserProfileRedis *profile = mezon__realtime__user_profile_redis__unpack(NULL, reply->len, (const uint8_t *)reply->str);

  if (!profile) {
    log_error("Failed to unpack UserProfileRedis Protobuf");
    return NULL;
  }

  return profile;
}

Mezon__Api__ChannelDescription *get_channel_from_id(int64_t channel_id) {
  char cache_key[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "chls:%lld", (long long)channel_id);
  static const char cmd[] = "GET";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (Mezon__Api__ChannelDescription *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (Mezon__Api__ChannelDescription *)data;

    case CSC_CACHE_LEADER: {
      valkeyReply *v_reply = valkeyCommand(valkey_sync(), "GET %s", cache_key);
      if (!v_reply || v_reply->type == VALKEY_REPLY_NIL) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        if (v_reply) {
          freeReplyObject(v_reply);
        }
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Api__ChannelDescription *ch = channel_desc_from_reply(v_reply);
      freeReplyObject(v_reply);

      if (!ch) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, ch, free_cached_channel_description) != 0) {
        return NULL;
      }

      return ch;
    }

    default:
      return NULL;
  }
}

bool check_user_in_channel_vdb(int64_t user_id, int64_t channel_id) {
  char cache_key[64] = {0}, cmd[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "chlu:%lld", (long long)channel_id);
  snprintf(cmd, sizeof(cmd), "SISMEMBER %lld", (long long)user_id);

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (uintptr_t)data == 1;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (uintptr_t)data == 1;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return false;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "SISMEMBER %s %lld", cache_key, (long long)user_id);
      if (!v_reply) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, errno);
        return false;
      }

      bool exists = v_reply->integer == 1;
      csc_cache_update(global_valkey->cache, cache_key, cmd, exists ? (void *)1 : (void *)0);

      if (v_reply) {
        freeReplyObject(v_reply);
      }
      return exists;
    }

    default:
      return false;
  }
}

bool check_user_in_channel(int64_t user_id, int64_t channel_id, Mezon__Api__ChannelDescription *channel) {
  /* Bounded depth — guards against corrupted parent_id chains (e.g.
   * parent_id == channel_id, or a long cycle) which would otherwise blow
   * the stack and look like a deadlock when debugging. */
  enum { MAX_PARENT_DEPTH = 8 };

  for (int depth = 0; depth < MAX_PARENT_DEPTH; depth++) {
    if (check_user_in_channel_vdb(user_id, channel_id)) {
      return true;
    }

    if (channel == NULL) {
      channel = get_channel_from_id(channel_id);
      if (channel == NULL) {
        log_error("could not get channel %lld", (long long)channel_id);
        return false;
      }
    }

    /* Detect direct self-cycle. */
    if (channel->parent_id == channel_id) {
      log_warn("channel %lld parent_id == channel_id (corrupt data)", (long long)channel_id);
      break;
    }

    if (channel->parent_id != 0 && channel->channel_private == 0) {
      channel_id = channel->parent_id;
      channel = NULL;  // re-fetch parent on next iteration
      continue;
    }

    if (channel->channel_private == 0) {
      return check_user_in_clan(user_id, channel->clan_id);
    }

    return false;
  }

  log_warn("check_user_in_channel: max parent depth reached for channel %lld user %lld", (long long)channel_id, (long long)user_id);
  return false;
}

bool check_user_in_clan(int64_t user_id, int64_t clan_id) {
  char cache_key[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "clan:%lld:%lld", (long long)clan_id, (long long)user_id);
  static const char cmd[] = "EXISTS";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (uintptr_t)data != 0;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (uintptr_t)data != 0;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return false;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "EXISTS %s", cache_key);

      bool exists = (v_reply && v_reply->type == VALKEY_REPLY_INTEGER && v_reply->integer == 1);
      void *to_store = exists ? (void *)1 : (void *)0;

      csc_cache_update(global_valkey->cache, cache_key, cmd, to_store);

      if (v_reply) {
        freeReplyObject(v_reply);
      }
      return exists;
    }

    default:
      return false;
  }
}

Mezon__Realtime__UserProfileRedis *get_user_profile(int64_t user_id) {
  char cache_key[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "usrs:%lld", (long long)user_id);
  static const char cmd[] = "GET";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (Mezon__Realtime__UserProfileRedis *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (Mezon__Realtime__UserProfileRedis *)data;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "GET %s", cache_key);
      if (!v_reply) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Realtime__UserProfileRedis *profile = user_profile_from_reply(v_reply);
      freeReplyObject(v_reply);

      if (!profile) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, profile, free_cached_user_profile) != 0) {
        return NULL;
      }

      return profile;
    }

    default:
      return NULL;
  }
}

Mezon__Api__ClanProfile *get_clan_profile(int64_t clan_id, int64_t user_id) {
  char cache_key[128];
  snprintf(cache_key, sizeof(cache_key), "clan:%lld:%lld", (long long)clan_id, (long long)user_id);

  static const char cmd[] = "GET";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (Mezon__Api__ClanProfile *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (Mezon__Api__ClanProfile *)data;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "GET %s", cache_key);
      if (!v_reply) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        if (v_reply) {
          freeReplyObject(v_reply);
        }

        log_warn("cache miss for key: %s", cache_key);

        return NULL;
      }

      Mezon__Api__ClanProfile *profile = clan_profile_from_reply(v_reply);
      freeReplyObject(v_reply);

      if (!profile) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, profile, free_cached_clan_profile) != 0) {
        return NULL;
      }

      return profile;
    }

    default:
      return NULL;
  }
}

Mezon__Api__ClanDesc *get_clan_by_clan_id(int64_t clan_id) {
  char cache_key[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "clandesc:%lld", (long long)clan_id);
  static const char cmd[] = "GET";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (Mezon__Api__ClanDesc *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (Mezon__Api__ClanDesc *)data;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "GET %s", cache_key);
      if (!v_reply || v_reply->type == VALKEY_REPLY_NIL) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        if (v_reply) {
          freeReplyObject(v_reply);
        }

        log_warn("cache miss for key: %s", cache_key);

        return NULL;
      }

      Mezon__Api__ClanDesc *clan = clan_desc_from_reply(v_reply);
      freeReplyObject(v_reply);

      if (!clan) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, clan, free_cached_clan_desc) != 0) {
        return NULL;
      }

      return clan;
    }

    default:
      return NULL;
  }
}

void get_users_from_ids(const int64_t *ids, size_t nids, Mezon__Realtime__UserProfileRedis **out_profiles) {
  if (nids == 0) {
    return;
  }

  char keys[nids][64];
  size_t leaders_idx[nids];
  size_t nleaders = 0;
  /* dup_of[i] == i  means i is the first occurrence (unique within this batch).
   * dup_of[i] <  i  means i is a duplicate of slot dup_of[i]; we MUST NOT call
   * csc_cache_flight for it again on the same thread, otherwise we register as
   * WAITER for an in-flight we are also LEADER of and self-deadlock. */
  size_t dup_of[nids];
  static const char cmd[] = "GET";

  for (size_t i = 0; i < nids; i++) {
    snprintf(keys[i], sizeof(keys[i]), "usrs:%lld", (long long)ids[i]);
    out_profiles[i] = NULL;
    dup_of[i] = i;
    for (size_t j = 0; j < i; j++) {
      if (ids[j] == ids[i]) {
        dup_of[i] = j;
        break;
      }
    }
    if (dup_of[i] != i) {
      continue;  // resolve via the first occurrence after the batch fetch
    }

    void *data = NULL;
    csc_cache_entry_t *entry = NULL;

    int state = csc_cache_flight(global_valkey->cache, keys[i], cmd, &data, &entry);

    switch (state) {
      case CSC_CACHE_HIT:
        out_profiles[i] = (Mezon__Realtime__UserProfileRedis *)data;
        break;

      case CSC_CACHE_WAITER:
        csc_cache_entry_wait(entry, NULL, &data);
        out_profiles[i] = (Mezon__Realtime__UserProfileRedis *)data;
        break;

      case CSC_CACHE_LEADER:
        leaders_idx[nleaders++] = i;
        break;

      default:
        log_error("get_users_from_ids: Unexpected cache return code %d", state);
        break;
    }
  }

  if (nleaders == 0) {
    /* Propagate to duplicates from earlier occurrences (HIT/WAITER paths). */
    for (size_t i = 0; i < nids; i++) {
      if (dup_of[i] != i) {
        out_profiles[i] = out_profiles[dup_of[i]];
      }
    }
    return;
  }

  char mget_cmd[nleaders * 72 + 8];
  int off = snprintf(mget_cmd, sizeof(mget_cmd), "MGET");
  for (size_t m = 0; m < nleaders; m++) {
    off += snprintf(mget_cmd + off, sizeof(mget_cmd) - off, " %s", keys[leaders_idx[m]]);
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    for (size_t m = 0; m < nleaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, EIO);
    }
    return;
  }
  valkeyReply *v_reply = valkeyCommand(vc, mget_cmd);

  if (!v_reply || v_reply->type != VALKEY_REPLY_ARRAY) {
    for (size_t m = 0; m < nleaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, EIO);
    }
    if (v_reply) {
      freeReplyObject(v_reply);
    }
    return;
  }

  for (size_t m = 0; m < nleaders && m < v_reply->elements; m++) {
    size_t i = leaders_idx[m];
    valkeyReply *elem = v_reply->element[m];

    if (!elem || elem->type == VALKEY_REPLY_NIL) {
      csc_cache_cancel(global_valkey->cache, keys[i], cmd, ENOENT);
      continue;
    }

    out_profiles[i] = user_profile_from_reply(elem);
    if (!out_profiles[i]) {
      csc_cache_cancel(global_valkey->cache, keys[i], cmd, ENOMEM);
      continue;
    }

    if (csc_cache_update_owned(global_valkey->cache, keys[i], cmd, out_profiles[i], free_cached_user_profile) != 0) {
      out_profiles[i] = NULL;
    }
  }

  freeReplyObject(v_reply);

  /* Replicate results to duplicate slots; the cache owns the underlying object. */
  for (size_t i = 0; i < nids; i++) {
    if (dup_of[i] != i) {
      out_profiles[i] = out_profiles[dup_of[i]];
    }
  }
}

void get_msg_header_from_keys(const char **keys, size_t nkeys, Mezon__Api__ChannelMessageHeader **out_headers) {
  if (nkeys == 0) {
    return;
  }

  size_t leaders_idx[nkeys];
  size_t nleaders = 0;
  /* dup_of[i] == i  → first occurrence; otherwise i mirrors dup_of[i]. Used
   * to avoid the same thread becoming both LEADER and WAITER for one key. */
  size_t dup_of[nkeys];
  static const char cmd[] = "GET";

  for (size_t i = 0; i < nkeys; i++) {
    out_headers[i] = NULL;
    dup_of[i] = i;
    for (size_t j = 0; j < i; j++) {
      if (strcmp(keys[j], keys[i]) == 0) {
        dup_of[i] = j;
        break;
      }
    }
    if (dup_of[i] != i) {
      continue;
    }

    void *data = NULL;
    csc_cache_entry_t *entry = NULL;

    int state = csc_cache_flight(global_valkey->cache, keys[i], cmd, &data, &entry);

    switch (state) {
      case CSC_CACHE_HIT:
        out_headers[i] = (Mezon__Api__ChannelMessageHeader *)data;
        break;

      case CSC_CACHE_WAITER:
        csc_cache_entry_wait(entry, NULL, &data);
        out_headers[i] = (Mezon__Api__ChannelMessageHeader *)data;
        break;

      case CSC_CACHE_LEADER:
        leaders_idx[nleaders++] = i;
        break;

      default:
        log_error("get_msg_header_from_keys: Unexpected cache return code %d", state);
        break;
    }
  }

  if (nleaders == 0) {
    for (size_t i = 0; i < nkeys; i++) {
      if (dup_of[i] != i) {
        out_headers[i] = out_headers[dup_of[i]];
      }
    }
    return;
  }

  size_t cmd_sz = 8;
  for (size_t m = 0; m < nleaders; m++) {
    cmd_sz += strlen(keys[leaders_idx[m]]) + 2;
  }

  char *mget_cmd = mi_malloc(cmd_sz);
  if (!mget_cmd) {
    for (size_t m = 0; m < nleaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, ENOMEM);
    }
    return;
  }

  int off = snprintf(mget_cmd, cmd_sz, "MGET");
  for (size_t m = 0; m < nleaders; m++) {
    off += snprintf(mget_cmd + off, cmd_sz - off, " %s", keys[leaders_idx[m]]);
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    for (size_t m = 0; m < nleaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, EIO);
    }
    mi_free(mget_cmd);
    return;
  }
  valkeyReply *v_reply = valkeyCommand(vc, mget_cmd);
  mi_free(mget_cmd);

  if (!v_reply || v_reply->type != VALKEY_REPLY_ARRAY) {
    for (size_t m = 0; m < nleaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, EIO);
    }
    if (v_reply) {
      freeReplyObject(v_reply);
    }
    return;
  }

  for (size_t m = 0; m < nleaders && m < v_reply->elements; m++) {
    size_t i = leaders_idx[m];
    valkeyReply *elem = v_reply->element[m];

    if (!elem || elem->type == VALKEY_REPLY_NIL) {
      csc_cache_cancel(global_valkey->cache, keys[i], cmd, ENOENT);
      continue;
    }

    out_headers[i] = msg_header_from_reply(elem);
    if (!out_headers[i]) {
      csc_cache_cancel(global_valkey->cache, keys[i], cmd, ENOMEM);
      continue;
    }

    if (csc_cache_update_owned(global_valkey->cache, keys[i], cmd, out_headers[i], free_cached_channel_message_header) != 0) {
      out_headers[i] = NULL;
    }
  }

  freeReplyObject(v_reply);

  for (size_t i = 0; i < nkeys; i++) {
    if (dup_of[i] != i) {
      out_headers[i] = out_headers[dup_of[i]];
    }
  }
}

Mezon__Api__ChannelDescription *get_dm_channel_cache(const char *key) {
  static const char cmd[] = "GET";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (Mezon__Api__ChannelDescription *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (Mezon__Api__ChannelDescription *)data;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "GET %s", key);
      if (!v_reply || v_reply->type == VALKEY_REPLY_NIL) {
        csc_cache_cancel(global_valkey->cache, key, cmd, ENOENT);
        if (v_reply) {
          freeReplyObject(v_reply);
        }
        return NULL;
      }

      Mezon__Api__ChannelDescription *ch = channel_desc_from_reply(v_reply);
      freeReplyObject(v_reply);

      if (!ch) {
        csc_cache_cancel(global_valkey->cache, key, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, key, cmd, ch, free_cached_channel_description) != 0) {
        return NULL;
      }

      return ch;
    }

    default:
      return NULL;
  }
}

int save_lsent_lseen(int64_t channel_id, int64_t user_id, const uint8_t *msg_header_bytes, size_t msg_header_len) {
  char sent_key[64], seen_key[128];
  snprintf(sent_key, sizeof sent_key, "lsent:%lld", (long long)channel_id);
  snprintf(seen_key, sizeof seen_key, "lseen:%lld:%lld", (long long)channel_id, (long long)user_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }

  // MSET key1 val1 key2 val2 — single round trip, both keys same value.
  const char *argv[5];
  size_t argv_len[5];

  argv[0] = "MSET";
  argv_len[0] = 4;
  argv[1] = sent_key;
  argv_len[1] = strlen(sent_key);
  argv[2] = (const char *)msg_header_bytes;
  argv_len[2] = msg_header_len;
  argv[3] = seen_key;
  argv_len[3] = strlen(seen_key);
  argv[4] = (const char *)msg_header_bytes;
  argv_len[4] = msg_header_len;

  valkeyReply *v_reply = valkeyCommandArgv(vc, 5, argv, argv_len);
  if (!v_reply) {
    return -EIO;
  }
  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey Error (save_lsent_lseen): %s", v_reply->str);
  }
  int ok = (v_reply->type != VALKEY_REPLY_ERROR) ? 0 : -EIO;
  freeReplyObject(v_reply);
  return ok;
}

int save_lseen(int64_t channel_id, int64_t user_id, const uint8_t *msg_header_bytes, size_t msg_header_len) {
  char seen_key[128];
  snprintf(seen_key, sizeof seen_key, "lseen:%lld:%lld", (long long)channel_id, (long long)user_id);
  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "SET %s %b", seen_key, msg_header_bytes, msg_header_len);
  if (!v_reply) {
    return -EIO;
  }
  int ok = (v_reply->type != VALKEY_REPLY_ERROR) ? 0 : -EIO;
  if (ok != 0) {
    log_error("Valkey Error (save_lseen): %s", v_reply->str);
  }
  freeReplyObject(v_reply);
  return ok;
}

int remove_chtks(int64_t channel_id, const char **tokens, size_t ntokens) {
  if (ntokens == 0) {
    return 0;
  }

  char key[64] = {0};
  snprintf(key, sizeof(key), "chtk:%lld", (long long)channel_id);

  valkey_post_invalidation(NULL, 0); /* invalidate whole cache for key */

  size_t cmd_sz = strlen(key) + 8;
  for (size_t i = 0; i < ntokens; i++) {
    cmd_sz += strlen(tokens[i]) + 2;
  }

  char *cmd = mi_malloc(cmd_sz);
  if (!cmd) {
    return -ENOMEM;
  }

  int off = snprintf(cmd, cmd_sz, "SREM %s", key);
  for (size_t i = 0; i < ntokens; i++) {
    off += snprintf(cmd + off, cmd_sz - off, " %s", tokens[i]);
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, cmd);
  mi_free(cmd);

  if (!v_reply) {
    return -EIO;
  }
  int ok = (v_reply->type != VALKEY_REPLY_ERROR) ? 0 : -EIO;
  freeReplyObject(v_reply);
  return ok;
}

int remove_chtkds(int64_t channel_id, const char **tokens, size_t ntokens) {
  if (ntokens == 0) {
    return 0;
  }

  char key[64] = {0};
  snprintf(key, sizeof(key), "chtkd:%lld", (long long)channel_id);

  valkey_post_invalidation(NULL, 0);

  size_t cmd_sz = strlen(key) + 8;
  for (size_t i = 0; i < ntokens; i++) {
    cmd_sz += strlen(tokens[i]) + 2;
  }

  char *cmd = mi_malloc(cmd_sz);
  if (!cmd) {
    return -ENOMEM;
  }

  int off = snprintf(cmd, cmd_sz, "SREM %s", key);
  for (size_t i = 0; i < ntokens; i++) {
    off += snprintf(cmd + off, cmd_sz - off, " %s", tokens[i]);
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, cmd);
  mi_free(cmd);

  if (!v_reply) {
    return -EIO;
  }
  int ok = (v_reply->type != VALKEY_REPLY_ERROR) ? 0 : -EIO;
  freeReplyObject(v_reply);
  return ok;
}

static const char k_reset_badge_script[] =
    "local c = redis.call('HGET', KEYS[1], ARGV[1])\n"
    "if c then\n"
    "  redis.call('INCRBY', KEYS[2], -tonumber(c))\n"
    "  redis.call('HDEL',   KEYS[1], ARGV[1])\n"
    "end\n";

int reset_channel_badge_count(int64_t clan_id, int64_t channel_id, int64_t user_id) {
  char key[96], key_total[96], channel_id_str[32];
  snprintf(key, sizeof key, "badge:%lld:%lld", (long long)clan_id, (long long)user_id);
  snprintf(key_total, sizeof key_total, "cbadge:%lld:%lld", (long long)clan_id, (long long)user_id);
  snprintf(channel_id_str, sizeof channel_id_str, "%lld", (long long)channel_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "EVAL %s 2 %s %s %s", k_reset_badge_script, key, key_total, channel_id_str);

  if (!v_reply) {
    return -EIO;
  }
  int ok = (v_reply->type != VALKEY_REPLY_ERROR) ? 0 : -EIO;
  freeReplyObject(v_reply);
  return ok;
}

int get_seed_gid(int64_t channel_id, int64_t *out) {
  char field[32];
  snprintf(field, sizeof(field), "%lld", (long long)channel_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "HGET seedid %s", field);
  if (!v_reply || v_reply->type == VALKEY_REPLY_NIL) {
    if (v_reply) {
      freeReplyObject(v_reply);
    }
    return -ENOENT;
  }
  if (v_reply->type != VALKEY_REPLY_INTEGER && v_reply->type != VALKEY_REPLY_STRING) {
    freeReplyObject(v_reply);
    return -EIO;
  }

  *out = (v_reply->type == VALKEY_REPLY_INTEGER) ? v_reply->integer : (int64_t)strtoll(v_reply->str, NULL, 10);

  freeReplyObject(v_reply);
  return 0;
}

int add_closed_direct_message(int64_t channel_id) {
  char key[64] = {0};
  snprintf(key, sizeof(key), "dmafk:%lld", (long long)channel_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "SET %s 1", key);
  if (!v_reply) {
    return -EIO;
  }
  int ok = (v_reply->type != VALKEY_REPLY_ERROR) ? 0 : -EIO;
  freeReplyObject(v_reply);
  return ok;
}

int save_custom_status(int64_t user_id, const char *status) {
  Mezon__Realtime__UserProfileRedis *profile = get_user_profile(user_id);
  if (!profile) {
    return -ENOENT;
  }

  Mezon__Realtime__UserProfileRedis updated = *profile;
  updated.status = status ? (char *)status : (char *)protobuf_c_empty_string;
  return save_usrs(&updated);
}

void user_profile_set_status(Mezon__Realtime__UserProfileRedis *profile, const char *status) {
  if (!profile) {
    return;
  }

  if (profile->status && profile->status != (char *)protobuf_c_empty_string) {
    free(profile->status);
    profile->status = NULL;
  }

  if (status) {
    profile->status = strdup(status);
  } else {
    profile->status = (char *)protobuf_c_empty_string;
  }
}

int save_usrs(Mezon__Realtime__UserProfileRedis *profile) {
  if (!profile) {
    return -EINVAL;
  }

  char key[64] = {0};
  snprintf(key, sizeof(key), "usrs:%lld", (long long)profile->user_id);

  size_t size = mezon__realtime__user_profile_redis__get_packed_size(profile);

  uint8_t *buf = (uint8_t *)mi_malloc(size);
  if (!buf) {
    log_error("Could not allocate buffer for user profile packing");
    return -ENOMEM;
  }

  size_t n = mezon__realtime__user_profile_redis__pack(profile, buf);
  if (n != size) {
    log_error("Protobuf packing size mismatch: expected %zu, got %zu", size, n);
    mi_free(buf);
    return -EIO;
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "SET %s %b", key, buf, n);

  mi_free(buf);

  if (!v_reply) {
    log_error("Could not update user profile in redis for key %s", key);
    return -EIO;
  }

  int result = 0;
  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey error: %s", v_reply->str);
    result = -EIO;
  }

  freeReplyObject(v_reply);

  return result;
}

Mezon__Api__ChannelDescList *verify_list_channel_cache(int64_t clan_id, int64_t user_id, size_t *n_channels) {
  *n_channels = 0;
  char cache_key[128] = {0};
  char cache_key_obj[128] = {0};
  int n = snprintf(cache_key, sizeof(cache_key), "cache:listchanneldescs%lld%lld", (long long)clan_id, (long long)user_id);

  if (unlikely(n > (int)sizeof(cache_key_obj) - 4)) {
    n = sizeof(cache_key_obj) - 4;
  }

  memcpy(cache_key_obj, cache_key, n);

  // We use a 32-bit constant to write 'O', 'B', 'J', '\0' in a single CPU instruction
  *(uint32_t *)(cache_key_obj + n) = 0x004A424F;

  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key_obj, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      if (data) {
        Mezon__Api__ChannelDescList *list = (Mezon__Api__ChannelDescList *)data;
        *n_channels = list->n_channeldesc;
        return list;
      }
      break;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      if (data) {
        Mezon__Api__ChannelDescList *list = (Mezon__Api__ChannelDescList *)data;
        *n_channels = list->n_channeldesc;
        return list;
      }
      break;

    case CSC_CACHE_LEADER: {
      valkeyReply *target_reply = NULL;
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOTCONN);
        return NULL;
      }
      target_reply = valkeyCommand(vc, "GET %s", cache_key);

      if (!target_reply || target_reply->type == VALKEY_REPLY_NIL) {
        if (target_reply) {
          csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOENT);
          freeReplyObject(target_reply);
        }
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Api__ChannelDescList *list_obj = mezon__api__channel_desc_list__unpack(NULL, target_reply->len, (const uint8_t *)target_reply->str);
      freeReplyObject(target_reply);

      if (!list_obj) {
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key_obj, cmd, list_obj, free_cached_channel_desc_list) != 0) {
        return NULL;
      }

      *n_channels = list_obj->n_channeldesc;

      return list_obj;
    }

    default:
      return NULL;
  }

  return NULL;
}

channel_id_list_t *verify_list_channel_id_cache(int64_t clan_id, int64_t user_id, size_t *n_channels) {
  *n_channels = 0;
  char cache_key[128] = {0};
  char cache_key_id[128] = {0};
  int n = snprintf(cache_key, sizeof(cache_key), "cache:listchanneldescs%lld%lld", (long long)clan_id, (long long)user_id);

  if (unlikely(n > (int)sizeof(cache_key_id) - 3)) {
    n = sizeof(cache_key_id) - 3;
  }

  memcpy(cache_key_id, cache_key, n);

  // We use a 32-bit constant to write 'I', 'D', '\0' in a single CPU instruction
  *(uint32_t *)(cache_key_id + n) = 0x00004449;

  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key_id, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      if (data) {
        channel_id_list_t *list = (channel_id_list_t *)data;
        *n_channels = list->n_channels;
        return list;
      }
      break;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      if (data) {
        channel_id_list_t *list = (channel_id_list_t *)data;
        *n_channels = list->n_channels;
        return list;
      }
      break;

    case CSC_CACHE_LEADER: {
      valkeyReply *target_reply = NULL;
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, ENOTCONN);
        return NULL;
      }
      target_reply = valkeyCommand(vc, "LRANGE %s 0 -1", cache_key_id);

      if (!target_reply || target_reply->type == VALKEY_REPLY_NIL) {
        if (target_reply) {
          csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, ENOENT);
          freeReplyObject(target_reply);
        }
        log_warn("cache miss for key: %s", cache_key_id);
        return NULL;
      }

      size_t n = target_reply->elements;
      if (n == 0) {
        csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, ENOENT);
        freeReplyObject(target_reply);
        return NULL;
      }

      channel_id_list_t *list = mi_zalloc(sizeof(channel_id_list_t));
      if (!list) {
        csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, ENOMEM);
        freeReplyObject(target_reply);
        return NULL;
      }

      const char **channel_ids = mi_zalloc(n * sizeof(char *));
      if (!channel_ids) {
        csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, ENOMEM);
        mi_free(list);
        freeReplyObject(target_reply);
        return NULL;
      }
      for (size_t i = 0; i < n; i++) {
        valkeyReply *elem = target_reply->element[i];
        if (!elem || elem->type != VALKEY_REPLY_STRING || !elem->str) {
          csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, EIO);
          list->channels = channel_ids;
          list->n_channels = i;
          free_cached_channel_id_list(list);
          freeReplyObject(target_reply);
          return NULL;
        }
        channel_ids[i] = mi_strdup(elem->str);
        if (!channel_ids[i]) {
          csc_cache_cancel(global_valkey->cache, cache_key_id, cmd, ENOMEM);
          list->channels = channel_ids;
          list->n_channels = i;
          free_cached_channel_id_list(list);
          freeReplyObject(target_reply);
          return NULL;
        }
        list->n_channels = i + 1;
      }
      list->channels = channel_ids;
      list->n_channels = n;

      freeReplyObject(target_reply);

      if (csc_cache_update_owned(global_valkey->cache, cache_key_id, cmd, list, free_cached_channel_id_list) != 0) {
        return NULL;
      }

      *n_channels = n;

      return list;
    }

    default:
      return NULL;
  }

  return NULL;
}

Mezon__Api__FriendList *verify_list_friend_cache(int64_t user_id, size_t *n_friends) {
  *n_friends = 0;
  char cache_key[128];
  char cache_key_obj[128] = {0};
  int n = snprintf(cache_key, sizeof(cache_key), "cache:listfriend%lld", (long long)user_id);

  if (unlikely(n > (int)sizeof(cache_key_obj) - 4)) {
    n = sizeof(cache_key_obj) - 4;
  }

  memcpy(cache_key_obj, cache_key, n);

  // We use a 32-bit constant to write 'O', 'B', 'J', '\0' in a single CPU instruction
  *(uint32_t *)(cache_key_obj + n) = 0x004A424F;

  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key_obj, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      if (data) {
        Mezon__Api__FriendList *list = (Mezon__Api__FriendList *)data;
        *n_friends = list->n_friends;
        return list;
      }
      break;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      if (data) {
        Mezon__Api__FriendList *list = (Mezon__Api__FriendList *)data;
        *n_friends = list->n_friends;
        return list;
      }
      break;

    case CSC_CACHE_LEADER: {
      valkeyReply *target_reply = NULL;

      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOTCONN);
        return NULL;
      }
      target_reply = valkeyCommand(vc, "GET %s", cache_key);

      if (!target_reply || target_reply->type == VALKEY_REPLY_NIL) {
        if (target_reply) {
          csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOENT);
          freeReplyObject(target_reply);
        }
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Api__FriendList *friend_list = mezon__api__friend_list__unpack(NULL, (size_t)target_reply->len, (const uint8_t *)target_reply->str);

      if (!friend_list) {
        log_error("verify_list_friend_cache: failed to unpack FriendList (len: %zu)", (size_t)target_reply->len);
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOMEM);
        if (target_reply) {
          freeReplyObject(target_reply);
        }
        return NULL;
      }

      freeReplyObject(target_reply);

      if (csc_cache_update_owned(global_valkey->cache, cache_key_obj, cmd, friend_list, free_cached_friend_list) != 0) {
        return NULL;
      }

      *n_friends = friend_list->n_friends;
      return friend_list;
    }

    default:
      return NULL;
  }

  return NULL;
}

#define SEQUENCE_SHIFT_BITS 22
#define INSTANCE_SHIFT_BITS 10
int64_t get_next_msg_id(const char *channel_id) {
  static const char hash_key[] = "seedid";

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "HINCRBY %s %s 1", hash_key, channel_id);

  if (!v_reply) {
    return -EIO;
  }

  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey HINCRBY error: %s\n", v_reply->str);
    freeReplyObject(v_reply);
    return -EIO;
  }

  int64_t new_val = (int64_t)v_reply->integer << SEQUENCE_SHIFT_BITS | (1 << INSTANCE_SHIFT_BITS);

  freeReplyObject(v_reply);
  return new_val;
}

bool check_send_permission(int64_t user_id, int64_t channel_id) {
  char key[128];
  snprintf(key, sizeof(key), "ucns:%lld:%lld", (long long)user_id, (long long)channel_id);

  static const char cmd[] = "EXISTS";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (uintptr_t)data != 1;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (uintptr_t)data != 1;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, key, cmd, ENOTCONN);
        return false;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "EXISTS %s", key);
      if (!v_reply) {
        csc_cache_cancel(global_valkey->cache, key, cmd, errno);
        return false;
      }

      bool exists = (v_reply->type == VALKEY_REPLY_INTEGER && v_reply->integer == 1);

      csc_cache_update(global_valkey->cache, key, cmd, exists ? (void *)1 : (void *)0);

      freeReplyObject(v_reply);

      return !exists;
    }

    default:
      return false;
  }
}

Mezon__Api__ClanDescList *verify_list_clandesc_cache(int64_t user_id, size_t *n_clans) {
  *n_clans = 0;
  char cache_key[128] = {0};
  char cache_key_obj[128] = {0};

  int n = snprintf(cache_key, sizeof(cache_key), "cache:listclandescs%lld", (long long)user_id);

  if (unlikely(n > (int)sizeof(cache_key_obj) - 4)) {
    n = sizeof(cache_key_obj) - 4;
  }

  memcpy(cache_key_obj, cache_key, n);

  // We use a 32-bit constant to write 'O', 'B', 'J', '\0' in a single CPU instruction
  *(uint32_t *)(cache_key_obj + n) = 0x004A424F;

  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key_obj, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      if (data) {
        Mezon__Api__ClanDescList *list = (Mezon__Api__ClanDescList *)data;
        *n_clans = list->n_clandesc;
        return list;
      }
      break;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      if (data) {
        Mezon__Api__ClanDescList *list = (Mezon__Api__ClanDescList *)data;
        *n_clans = list->n_clandesc;
        return list;
      }
      break;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *target_reply = valkeyCommand(vc, "GET %s", cache_key);

      if (!target_reply || target_reply->type == VALKEY_REPLY_NIL) {
        if (target_reply) {
          // Notify cache system of missing entry to prevent stampede
          csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOENT);
          freeReplyObject(target_reply);
        }
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Api__ClanDescList *clandesc_list = mezon__api__clan_desc_list__unpack(NULL, (size_t)target_reply->len, (const uint8_t *)target_reply->str);

      freeReplyObject(target_reply);

      if (!clandesc_list) {
        log_error("verify_list_clandesc_cache: failed to unpack ClanDescList for user %lld", (long long)user_id);
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key_obj, cmd, clandesc_list, free_cached_clan_desc_list) != 0) {
        return NULL;
      }

      *n_clans = clandesc_list->n_clandesc;
      return clandesc_list;
    }
    default:
      return NULL;
  }

  return NULL;
}

int32_t get_badge_count_clan_total(int64_t clan_id, int64_t user_id) {
  char cache_key[128];
  snprintf(cache_key, sizeof(cache_key), "cbadge:%lld:%lld", (long long)clan_id, (long long)user_id);

  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      return (int32_t)(intptr_t)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (int32_t)(intptr_t)data;

    case CSC_CACHE_LEADER:
      // We are the leader; proceed to fetch from Valkey.
      break;

    default:
      log_error("mcache_get_badge_count: Unexpected cache status %d", state);
      return 0;
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
    return -ENOTCONN;
  }
  valkeyReply *reply = valkeyCommand(vc, "GET %s", cache_key);

  if (!reply) {
    csc_cache_cancel(global_valkey->cache, cache_key, cmd, EIO);
    return -ENOENT;
  }

  if (reply->type != VALKEY_REPLY_NIL && reply->type != VALKEY_REPLY_STRING && reply->type != VALKEY_REPLY_INTEGER) {
    log_error("mcache_get_badge_count: Unexpected Valkey reply type %d", reply->type);
    csc_cache_cancel(global_valkey->cache, cache_key, cmd, EIO);
    freeReplyObject(reply);
    return 0;
  }

  int32_t total = 0;
  if (reply->type != VALKEY_REPLY_NIL) {
    total = (int32_t)atoll(reply->str);
    freeReplyObject(reply);
  }

  csc_cache_update(global_valkey->cache, cache_key, cmd, (void *)(intptr_t)total);

  return total;
}

clan_badge_count_t *get_badge_count_clan(int64_t clan_id, int64_t user_id) {
  char cache_key[128];
  snprintf(cache_key, sizeof(cache_key), "badge:%lld:%lld", (long long)clan_id, (long long)user_id);

  static const char cmd[] = "HGETALL";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      return (clan_badge_count_t *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (clan_badge_count_t *)data;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }

      valkeyReply *reply = valkeyCommand(vc, "HGETALL %s", cache_key);
      if (!reply) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, EIO);
        return NULL;
      }

      if (reply->type == VALKEY_REPLY_NIL || (reply->type == VALKEY_REPLY_ARRAY && reply->elements == 0)) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        freeReplyObject(reply);
        return NULL;
      }

      if (reply->type != VALKEY_REPLY_ARRAY) {
        log_error("get_badge_count_clan: Unexpected Valkey reply type %d", reply->type);
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, EIO);
        freeReplyObject(reply);
        return NULL;
      }

      clan_badge_count_t *result = mi_malloc(sizeof(clan_badge_count_t));
      if (!result) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOMEM);
        freeReplyObject(reply);
        return NULL;
      }
      result->total_badge_count = 0;
      result->channel_badge_map = NULL;

      size_t total_elements = reply->elements;
      for (size_t i = 0; i < total_elements; i += 2) {
        if (i + 1 >= total_elements) {
          break;
        }

        valkeyReply *key_elem = reply->element[i];
        valkeyReply *val_elem = reply->element[i + 1];

        if (!key_elem || !val_elem || key_elem->type != VALKEY_REPLY_STRING) {
          continue;
        }

        char *endptr_id;
        int64_t channel_id = (int64_t)strtoll(key_elem->str, &endptr_id, 10);
        if (*endptr_id != '\0') {
          continue;
        }

        int32_t badge_val = (val_elem->type == VALKEY_REPLY_INTEGER) ? (int32_t)val_elem->integer : (int32_t)atoi(val_elem->str);

        result->total_badge_count += badge_val;

        if (badge_val != 0) {
          channel_badge_hash_node_t *node = mi_malloc(sizeof(channel_badge_hash_node_t));
          if (node) {
            node->channel_id = channel_id;
            node->badge_count = badge_val;

            // uthash macro injection mapping directly by 64-bit int key
            HASH_ADD_INT64(result->channel_badge_map, channel_id, node);
          }
        }
      }

      freeReplyObject(reply);
      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, result, free_cached_clan_badge_count) != 0) {
        return NULL;
      }
      return result;
    }

    default:
      return NULL;
  }
}

void free_clan_badge_count(clan_badge_count_t *badge) {
  if (!badge) {
    return;
  }

  channel_badge_hash_node_t *current_node, *tmp;
  // Safe deletion iteration loop macro from uthash.h
  HASH_ITER(hh, badge->channel_badge_map, current_node, tmp) {
    HASH_DEL(badge->channel_badge_map, current_node);
    mi_free(current_node);
  }

  mi_free(badge);
}

bool has_unread_badge(int64_t clan_id, int64_t user_id) {
  size_t n_channels = 0;
  channel_id_list_t *list = verify_list_channel_id_cache(clan_id, user_id, &n_channels);
  if (!list || n_channels == 0) {
    return false;
  }

  size_t n_keys = 2 * n_channels;
  const char **keys = mi_malloc(n_keys * sizeof(char *));
  char(*keybuf)[192] = mi_malloc(n_keys * sizeof(*keybuf));
  Mezon__Api__ChannelMessageHeader **headers = mi_zalloc(n_keys * sizeof(Mezon__Api__ChannelMessageHeader *));
  if (!keys || !keybuf || !headers) {
    mi_free(keys);
    mi_free(keybuf);
    mi_free(headers);
    return false;
  }

  for (size_t i = 0; i < n_channels; i++) {
    const char *channel_id_str = list->channels[i];
    snprintf(keybuf[2 * i], sizeof(keybuf[0]), "lsent:%s", channel_id_str);
    snprintf(keybuf[2 * i + 1], sizeof(keybuf[0]), "lseen:%s:%lld", channel_id_str, (long long)user_id);
    keys[2 * i] = keybuf[2 * i];
    keys[2 * i + 1] = keybuf[2 * i + 1];
  }

  get_msg_header_from_keys(keys, n_keys, headers);

  bool unread = false;
  for (size_t i = 0; i < n_channels; i++) {
    Mezon__Api__ChannelMessageHeader *sent = headers[2 * i];
    Mezon__Api__ChannelMessageHeader *seen = headers[2 * i + 1];

    if (!sent) {
      // No message ever sent in this channel — nothing to be unread.
      continue;
    }

    if (!seen || seen->id < sent->id) {
      unread = true;
      break;
    }
  }

  mi_free(keys);
  mi_free(keybuf);
  mi_free(headers);
  return unread;
}

int save_lpin(int64_t channel_id, int64_t user_id, int64_t message_id) {
  char key[64] = {0};
  char val_str[32] = {0};

  snprintf(key, sizeof(key), "lpin:%lld:%lld", (long long)channel_id, (long long)user_id);
  snprintf(val_str, sizeof(val_str), "%lld", (long long)message_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = (valkeyReply *)valkeyCommand(vc, "SET %s %s", key, val_str);

  if (!v_reply) {
    log_error("Valkey connection error during save_lpin");
    return -EIO;
  }

  int status = 0;
  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey SET error: %s", v_reply->str);
    status = -EIO;
  }

  freeReplyObject(v_reply);

  return status;
}

uint8_t *verify_cache_req(const char *cache_key, size_t *out_len, bool *hit) {
  *out_len = 0;
  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT: {
      valkeyReply *cached_reply = (valkeyReply *)data;
      if (!cached_reply) {
        return NULL;
      }
      if (hit != NULL) {
        *hit = true;
      }
      *out_len = cached_reply->len;
      return (uint8_t *)cached_reply->str;
    }

    case CSC_CACHE_WAITER: {
      csc_cache_entry_wait(entry, NULL, &data);
      valkeyReply *cached_reply = (valkeyReply *)data;
      if (!cached_reply) {
        return NULL;
      }
      if (hit != NULL) {
        *hit = true;
      }
      *out_len = cached_reply->len;
      return (uint8_t *)cached_reply->str;
    }

    case CSC_CACHE_LEADER: {
      log_debug("START verify cache key %s", cache_key);
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *reply = valkeyCommand(vc, "GET %s", cache_key);

      if (reply == NULL) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      /* Only string replies are usable. NIL / other types must NOT be
       * cached as a usable value — caller would otherwise see *out_len=0
       * and treat it as legitimately empty content. */
      if (reply->type != VALKEY_REPLY_STRING) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, reply->type == VALKEY_REPLY_NIL ? ENOENT : EIO);
        log_warn("cache miss for key: %s (reply type=%d)", cache_key, reply->type);
        freeReplyObject(reply);

        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, reply, free_cached_valkey_reply) != 0) {
        return NULL;
      }

      if (hit != NULL) {
        *hit = true;
      }
      *out_len = reply->len;

      log_debug("END verify cache key %s", cache_key);

      return (uint8_t *)reply->str;
    }
  }

  return NULL;
}

bool check_user_channel_mute(int64_t user_id, int64_t channel_id) {
  char key[128] = {0};
  snprintf(key, sizeof(key), "mtch:%lld:%lld", (long long)channel_id, (long long)user_id);

  static const char cmd[] = "EXISTS";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (uintptr_t)data != 0;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (uintptr_t)data != 0;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, key, cmd, ENOTCONN);
        return false;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "EXISTS %s", key);

      bool exists = false;
      if (v_reply) {
        // EXISTS returns integer 1 if key exists, 0 otherwise
        exists = (v_reply->type == VALKEY_REPLY_INTEGER && v_reply->integer == 1);
        freeReplyObject(v_reply);
      } else {
        // Connection error
        csc_cache_cancel(global_valkey->cache, key, cmd, EIO);
        return false;
      }

      void *to_store = exists ? (void *)1 : (void *)0;
      csc_cache_update(global_valkey->cache, key, cmd, to_store);

      return exists;
    }

    default:
      return false;
  }
}

uint8_t *get_list_user_permission_cache(int64_t user_id, int64_t channel_id, int64_t clan_id, size_t *out_len, bool *hit) {
  *out_len = 0;

  // key := "cache:listuserpermission" + clanId
  // field := userId + channelId
  char key[64], field[64], combined_cache_key[128];
  snprintf(key, sizeof(key), "cache:listuserpermission%lld", (long long)clan_id);
  snprintf(field, sizeof(field), "%lld%lld", (long long)user_id, (long long)channel_id);

  // For the local CSC cache, we combine key and field to form a unique local identifier
  snprintf(combined_cache_key, sizeof(combined_cache_key), "%s:%s", key, field);

  static const char cmd[] = "HGET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  // csc_cache_flight checks the local thread-local storage first
  int status = csc_cache_flight(global_valkey->cache, combined_cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      if (hit != NULL) {
        *hit = true;
      }
      if (data) {
        valkeyReply *reply = (valkeyReply *)data;
        *out_len = reply->len;
        return (uint8_t *)reply->str;
      }
      break;

    case CSC_CACHE_WAITER:
      // Wait for another thread (the leader) currently fetching this data
      csc_cache_entry_wait(entry, NULL, &data);
      if (hit != NULL) {
        *hit = true;
      }
      if (data) {
        valkeyReply *reply = (valkeyReply *)data;
        *out_len = reply->len;
        return (uint8_t *)reply->str;
      }
      break;

    case CSC_CACHE_LEADER: {
      // We are responsible for fetching from Valkey
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, combined_cache_key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "HGET %s %s", key, field);

      if (!v_reply) {
        // Cache miss: Notify waiters to stop waiting
        csc_cache_cancel(global_valkey->cache, combined_cache_key, cmd, ENOENT);
        log_warn("cache miss for key: %s field: %s", key, field);
        return NULL;
      }

      if (v_reply->type != VALKEY_REPLY_STRING) {
        csc_cache_cancel(global_valkey->cache, combined_cache_key, cmd, EIO);
        freeReplyObject(v_reply);
        return NULL;
      }

      // Store the reply object in the local cache[cite: 1]
      if (csc_cache_update_owned(global_valkey->cache, combined_cache_key, cmd, v_reply, free_cached_valkey_reply) != 0) {
        return NULL;
      }

      if (hit != NULL) {
        *hit = true;
      }
      *out_len = v_reply->len;
      return (uint8_t *)v_reply->str;
    }

    default:
      return NULL;
  }

  return NULL;
}

Mezon__Api__ClanUserList *verify_list_clan_users_cache(int64_t clan_id, size_t *n_users) {
  *n_users = 0;
  char cache_key[128] = {0};
  char cache_key_obj[128] = {0};

  // key := "cache:listclanusers" + clanId
  int n = snprintf(cache_key, sizeof(cache_key), "cache:listclanusers%lld", (long long)clan_id);

  if (unlikely(n > (int)sizeof(cache_key_obj) - 4)) {
    n = sizeof(cache_key_obj) - 4;
  }

  // cacheKeyObj := cacheKey + "OBJ"
  memcpy(cache_key_obj, cache_key, n);
  *(uint32_t *)(cache_key_obj + n) = 0x004A424F;  // Writes 'O', 'B', 'J', '\0'

  static const char cmd[] = "GET";
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int state = csc_cache_flight(global_valkey->cache, cache_key_obj, cmd, &data, &entry);

  switch (state) {
    case CSC_CACHE_HIT:
      if (data) {
        Mezon__Api__ClanUserList *list = (Mezon__Api__ClanUserList *)data;
        *n_users = list->n_clan_users;
        return list;
      }
      break;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      if (data) {
        Mezon__Api__ClanUserList *list = (Mezon__Api__ClanUserList *)data;
        *n_users = list->n_clan_users;
        return list;
      }
      break;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *target_reply = valkeyCommand(vc, "GET %s", cache_key);

      if (!target_reply || target_reply->type == VALKEY_REPLY_NIL) {
        if (target_reply) {
          csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOENT);
          freeReplyObject(target_reply);
        }
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Api__ClanUserList *user_list = mezon__api__clan_user_list__unpack(NULL, (size_t)target_reply->len, (const uint8_t *)target_reply->str);

      freeReplyObject(target_reply);

      if (!user_list) {
        log_error("verify_list_clan_users_cache: failed to unpack ClanUserList");
        csc_cache_cancel(global_valkey->cache, cache_key_obj, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key_obj, cmd, user_list, free_cached_clan_user_list) != 0) {
        return NULL;
      }
      *n_users = user_list->n_clan_users;
      return user_list;
    }

    default:
      return NULL;
  }

  return NULL;
}

typedef struct {
  int64_t user_id;
  size_t original_index;
  UT_hash_handle hh;
} deduplicate_user_t;

void users_online(const Mezon__Api__ClanUserList *users, size_t n_ids, bool *out_statuses) {
  if (n_ids == 0) {
    return;
  }

  char(*keys)[64] = mi_malloc(n_ids * sizeof(char[64]));
  size_t *leaders_idx = mi_malloc(n_ids * sizeof(size_t));
  size_t *dup_of = mi_malloc(n_ids * sizeof(size_t));
  if (!keys || !leaders_idx || !dup_of) {
    mi_free(keys);
    mi_free(leaders_idx);
    mi_free(dup_of);
    return;
  }

  size_t n_leaders = 0;
  /* dup_of[i] == i        → first valid occurrence of this user id (do flight)
   * dup_of[i] == SIZE_MAX → slot has no user id, skip
   * else                  → mirror result from dup_of[i] after the batch */
  static const char cmd[] = "GET";

  deduplicate_user_t *dup_map = NULL;

  for (size_t i = 0; i < n_ids; i++) {
    out_statuses[i] = false;

    if (users->clan_users[i] == NULL || users->clan_users[i]->user == NULL) {
      dup_of[i] = SIZE_MAX;
      continue;
    }

    int64_t uid = users->clan_users[i]->user->id;
    snprintf(keys[i], sizeof(keys[i]), "onl:%lld", (long long)uid);

    deduplicate_user_t *found = NULL;
    HASH_FIND(hh, dup_map, &uid, sizeof(int64_t), found);

    if (found) {
      dup_of[i] = found->original_index;
      continue;
    } else {
      dup_of[i] = i;
      found = mi_malloc(sizeof(deduplicate_user_t));
      if (found) {
        found->user_id = uid;
        found->original_index = i;
        HASH_ADD(hh, dup_map, user_id, sizeof(int64_t), found);
      }
    }

    void *data = NULL;
    csc_cache_entry_t *entry = NULL;

    int state = csc_cache_flight(global_valkey->cache, keys[i], cmd, &data, &entry);

    switch (state) {
      case CSC_CACHE_HIT:
        out_statuses[i] = (uintptr_t)data != 0;
        break;

      case CSC_CACHE_WAITER:
        csc_cache_entry_wait(entry, NULL, &data);
        out_statuses[i] = (uintptr_t)data != 0;
        break;

      case CSC_CACHE_LEADER:
        // Collect indices that need a network fetch
        leaders_idx[n_leaders++] = i;
        break;

      default:
        break;
    }
  }

  if (n_leaders == 0) {
    /* Propagate HIT/WAITER results to duplicate slots. */
    for (size_t i = 0; i < n_ids; i++) {
      if (dup_of[i] != i && dup_of[i] != SIZE_MAX) {
        out_statuses[i] = out_statuses[dup_of[i]];
      }
    }
    mi_free(keys);
    mi_free(leaders_idx);
    mi_free(dup_of);
    return;
  }

  // Estimate command buffer size: "MGET " + (key length + space) * count
  size_t cmd_sz = 8 + (n_leaders * 64);
  char *mget_cmd = mi_malloc(cmd_sz);
  if (!mget_cmd) {
    for (size_t m = 0; m < n_leaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, ENOMEM);
    }
    mi_free(keys);
    mi_free(leaders_idx);
    mi_free(dup_of);
    return;
  }

  int off = snprintf(mget_cmd, cmd_sz, "MGET");
  for (size_t m = 0; m < n_leaders; m++) {
    off += snprintf(mget_cmd + off, cmd_sz - off, " %s", keys[leaders_idx[m]]);
  }

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    for (size_t m = 0; m < n_leaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, EIO);
    }
    mi_free(mget_cmd);
    mi_free(keys);
    mi_free(leaders_idx);
    mi_free(dup_of);
    return;
  }
  valkeyReply *v_reply = valkeyCommand(vc, mget_cmd);
  mi_free(mget_cmd);

  if (!v_reply || v_reply->type != VALKEY_REPLY_ARRAY) {
    for (size_t m = 0; m < n_leaders; m++) {
      csc_cache_cancel(global_valkey->cache, keys[leaders_idx[m]], cmd, EIO);
    }
    if (v_reply) {
      freeReplyObject(v_reply);
    }
    mi_free(keys);
    mi_free(leaders_idx);
    mi_free(dup_of);
    return;
  }

  for (size_t m = 0; m < n_leaders && m < v_reply->elements; m++) {
    size_t idx = leaders_idx[m];
    valkeyReply *elem = v_reply->element[m];

    bool is_online = false;
    if (elem && elem->type == VALKEY_REPLY_STRING) {
      // strVal == "1" check from Go
      is_online = (strcmp(elem->str, "1") == 0);
    }

    out_statuses[idx] = is_online;

    // Store the result as a pointer-sized integer (0 or 1)
    csc_cache_update(global_valkey->cache, keys[idx], cmd, (void *)(uintptr_t)is_online);
  }

  freeReplyObject(v_reply);

  for (size_t i = 0; i < n_ids; i++) {
    if (dup_of[i] != i && dup_of[i] != SIZE_MAX) {
      out_statuses[i] = out_statuses[dup_of[i]];
    }
  }

  deduplicate_user_t *curr_item, *tmp_item;
  HASH_ITER(hh, dup_map, curr_item, tmp_item) {
    HASH_DEL(dup_map, curr_item);
    mi_free(curr_item);
  }

  mi_free(keys);
  mi_free(leaders_idx);
  mi_free(dup_of);
}

int mcache_delete_online(int64_t user_id) {
  if (user_id == 0) {
    return -EINVAL;
  }

  char key[64];
  snprintf(key, sizeof(key), "onl:%lld", (long long)user_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "DEL %s", key);

  if (!v_reply) {
    log_error("Valkey connection error during mcache_delete_online for user %lld", (long long)user_id);
    return -EIO;
  }

  int result = 0;
  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey Error (DeleteOnline): %s", v_reply->str);
    result = -EIO;
  }

  freeReplyObject(v_reply);
  return result;
}

int mcache_save_online(int64_t user_id) {
  if (user_id == 0) {
    return -EINVAL;
  }

  char key[64];
  snprintf(key, sizeof(key), "onl:%lld", (long long)user_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "SET %s 1", key);

  if (!v_reply) {
    log_error("Valkey connection error during mcache_save_online for user %lld", (long long)user_id);
    return -EIO;
  }

  int result = 0;
  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey Error (SaveOnline): %s", v_reply->str);
    result = -EIO;
  }

  freeReplyObject(v_reply);
  return result;
}

static const char *k_session_rotation_script =
    "local aidKey = KEYS[1] "
    "local newSidKey = KEYS[2] "
    "local data = ARGV[1] "
    "local newSidRaw = ARGV[2] "
    "local currentOldSid = redis.call('GET', aidKey) "
    "if currentOldSid then "
    "    redis.call('DEL', 'sid:' .. currentOldSid) "
    "end "
    "redis.call('SET', newSidKey, data) "
    "redis.call('SET', aidKey, newSidRaw) "
    "return 0";

int mcache_update_session(int64_t user_id, uint8_t auth_src, const char *new_sid, const uint8_t *value, size_t value_len) {
  if (user_id == 0 || !new_sid || !value) {
    return -EINVAL;
  }

  char aid_key[64];
  char sid_key[128];

  // Construct keys following the "aid:ID" and "sid:SID" format
  snprintf(aid_key, sizeof(aid_key), "aid:%lld%u", (long long)user_id, auth_src);
  snprintf(sid_key, sizeof(sid_key), "sid:%s", new_sid);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    return -ENOTCONN;
  }
  valkeyReply *v_reply = valkeyCommand(vc, "EVAL %s 2 %s %s %b %s", k_session_rotation_script, aid_key, sid_key, (void *)value, value_len, new_sid);

  if (!v_reply) {
    log_error("Valkey connection error during mcache_update_session");
    return -EIO;
  }

  int result = 0;
  if (v_reply->type == VALKEY_REPLY_ERROR) {
    log_error("Valkey Script Error (UpdateSession): %s", v_reply->str);
    result = -EIO;
  }

  freeReplyObject(v_reply);
  return result;
}

uint8_t *get_uidctrlk_cache(int64_t user_id, size_t *out_len, bool *hit) {
  static const char cache_key[] = "cache:uidctrlk";

  char cmd[64] = {0};
  snprintf(cmd, sizeof(cmd), "HGET %lld", (long long)user_id);

  *out_len = 0;
  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT: {
      valkeyReply *cached_reply = (valkeyReply *)data;
      if (hit != NULL) {
        *hit = true;
      }
      if (!cached_reply) {
        return NULL;
      }
      *out_len = cached_reply->len;
      return (uint8_t *)cached_reply->str;
    }

    case CSC_CACHE_WAITER: {
      csc_cache_entry_wait(entry, NULL, &data);
      valkeyReply *cached_reply = (valkeyReply *)data;
      if (hit != NULL) {
        *hit = true;
      }
      if (!cached_reply) {
        return NULL;
      }
      *out_len = cached_reply->len;
      return (uint8_t *)cached_reply->str;
    }

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }

      valkeyReply *reply = valkeyCommand(vc, "HGET cache:uidctrlk %lld", (long long)user_id);

      if (reply == NULL) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOENT);
        log_warn("cache miss for key: %s field: %lld", cache_key, (long long)user_id);
        return NULL;
      }

      if (reply->type != VALKEY_REPLY_STRING) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, reply->type == VALKEY_REPLY_NIL ? ENOENT : EIO);
        log_warn("cache miss for key: %s field: %lld (reply type=%d)", cache_key, (long long)user_id, reply->type);
        freeReplyObject(reply);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, reply, free_cached_valkey_reply) != 0) {
        return NULL;
      }

      if (hit != NULL) {
        *hit = true;
      }
      *out_len = reply->len;
      return (uint8_t *)reply->str;
    }

    default:
      return NULL;
  }
}

static void free_cached_voice_channel_user(void *data) { mezon__api__voice_channel_user__free_unpacked((Mezon__Api__VoiceChannelUser *)data, NULL); }

Mezon__Api__VoiceChannelUser *get_voice_channel_user(int64_t channel_id) {
  char cache_key[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "lppt:%lld", (long long)channel_id);
  static const char cmd[] = "GET";

  void *data = NULL;
  csc_cache_entry_t *entry = NULL;

  int status = csc_cache_flight(global_valkey->cache, cache_key, cmd, &data, &entry);

  switch (status) {
    case CSC_CACHE_HIT:
      return (Mezon__Api__VoiceChannelUser *)data;

    case CSC_CACHE_WAITER:
      csc_cache_entry_wait(entry, NULL, &data);
      return (Mezon__Api__VoiceChannelUser *)data;

    case CSC_CACHE_LEADER: {
      valkeyContext *vc = valkey_sync();
      if (!vc) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOTCONN);
        return NULL;
      }
      valkeyReply *v_reply = valkeyCommand(vc, "GET %s", cache_key);

      if (!v_reply || v_reply->type != VALKEY_REPLY_STRING) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, (!v_reply || v_reply->type == VALKEY_REPLY_NIL) ? ENOENT : EIO);
        if (v_reply) {
          freeReplyObject(v_reply);
        }
        log_warn("cache miss for key: %s", cache_key);
        return NULL;
      }

      Mezon__Api__VoiceChannelUser *chl = mezon__api__voice_channel_user__unpack(NULL, v_reply->len, (const uint8_t *)v_reply->str);
      freeReplyObject(v_reply);

      if (!chl) {
        csc_cache_cancel(global_valkey->cache, cache_key, cmd, ENOMEM);
        return NULL;
      }

      if (csc_cache_update_owned(global_valkey->cache, cache_key, cmd, chl, free_cached_voice_channel_user) != 0) {
        mezon__api__voice_channel_user__free_unpacked(chl, NULL);
        return NULL;
      }

      return chl;
    }

    default:
      return NULL;
  }
}

int get_expired_ttl_seconds(int64_t user_id, int64_t channel_id, int32_t *out_ttl) {
  char cache_key[64] = {0};
  snprintf(cache_key, sizeof(cache_key), "ucns:%lld:%lld", (long long)user_id, (long long)channel_id);

  valkeyContext *vc = valkey_sync();
  if (!vc) {
    log_warn("get_expired_ttl_seconds: no valkey connection, key: %s", cache_key);
    *out_ttl = 0;
    return -1;
  }

  valkeyReply *v_reply = valkeyCommand(vc, "TTL %s", cache_key);
  if (!v_reply || v_reply->type != VALKEY_REPLY_INTEGER) {
    log_warn("get_expired_ttl_seconds: TTL failed for key: %s", cache_key);
    if (v_reply) {
      freeReplyObject(v_reply);
    }
    *out_ttl = 0;
    return -1;
  }

  long long ttl = v_reply->integer;
  freeReplyObject(v_reply);

  *out_ttl = (ttl > 0) ? (int32_t)ttl : 0;
  return 0;
}
