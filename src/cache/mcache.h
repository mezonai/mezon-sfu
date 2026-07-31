#ifndef MEZON_CACHE_H
#define MEZON_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "api.pb-c.h"
#include "realtime.pb-c.h"
#include "uthash.h"
#include "valkey_client.h"

#define HASH_FIND_INT64(head, findint64, out) HASH_FIND(hh, head, findint64, sizeof(int64_t), out)

#define HASH_ADD_INT64(head, int64field, add) HASH_ADD(hh, head, int64field, sizeof(int64_t), add)

typedef struct {
  const char **channels;
  size_t n_channels;
} channel_id_list_t;

typedef struct {
  int64_t channel_id;  /* The Map Key */
  int32_t badge_count; /* The Map Value */
  UT_hash_handle hh;   /* Makes this structure hashable */
} channel_badge_hash_node_t;

typedef struct {
  int32_t total_badge_count;
  channel_badge_hash_node_t *channel_badge_map; /* Head pointer of hash map */
} clan_badge_count_t;

#define unlikely(x) __builtin_expect(!!(x), 0)

bool check_user_in_clan(int64_t user_id, int64_t clan_id);
bool check_user_in_channel(int64_t user_id, int64_t channel_id, Mezon__Api__ChannelDescription *channel);
Mezon__Api__ChannelDescription *get_channel_from_id(int64_t channel_id);
bool check_user_in_channel_vdb(int64_t user_id, int64_t channel_id);
Mezon__Realtime__UserProfileRedis *get_user_profile(int64_t user_id);
Mezon__Api__ClanDesc *get_clan_by_clan_id(int64_t clan_id);
void get_users_from_ids(const int64_t *ids, size_t nids, Mezon__Realtime__UserProfileRedis **out_profiles);
void user_profile_set_status(Mezon__Realtime__UserProfileRedis *profile, const char *status);
Mezon__Api__ChannelDescription *get_dm_channel_cache(const char *cache_key);
int save_custom_status(int64_t user_id, const char *status);
int save_usrs(Mezon__Realtime__UserProfileRedis *profile);
int add_closed_direct_message(int64_t channel_id);
int get_seed_gid(int64_t channel_id, int64_t *out);
Mezon__Api__ChannelDescription *get_channel_from_id(int64_t channel_id);
Mezon__Api__ClanProfile *get_clan_profile(int64_t clan_id, int64_t user_id);
int save_lseen(int64_t channel_id, int64_t user_id, const uint8_t *msg_header_bytes, size_t msg_header_len);
int save_lsent_lseen(int64_t channel_id, int64_t user_id, const uint8_t *msg_header_bytes, size_t msg_header_len);
int remove_chtks(int64_t channel_id, const char **tokens, size_t ntokens);
int remove_chtkds(int64_t channel_id, const char **tokens, size_t ntokens);
int reset_channel_badge_count(int64_t clan_id, int64_t channel_id, int64_t user_id);
void user_profile_set_status(Mezon__Realtime__UserProfileRedis *profile, const char *status);
Mezon__Api__ChannelDescList *verify_list_channel_cache(int64_t clan_id, int64_t user_id, size_t *n_channels);
Mezon__Api__FriendList *verify_list_friend_cache(int64_t user_id, size_t *n_friends);
int64_t get_next_msg_id(const char *channel_id);
bool check_send_permission(int64_t user_id, int64_t channel_id);
Mezon__Api__ClanDescList *verify_list_clandesc_cache(int64_t user_id, size_t *n_clans);
int32_t get_badge_count_clan_total(int64_t clan_id, int64_t user_id);
bool has_unread_badge(int64_t clan_id, int64_t user_id);
int save_lpin(int64_t channel_id, int64_t user_id, int64_t message_id);
uint8_t *verify_cache_req(const char *cache_key, size_t *out_len, bool *hit);
channel_id_list_t *verify_list_channel_id_cache(int64_t clan_id, int64_t user_id, size_t *n_channels);
void get_msg_header_from_keys(const char **keys, size_t nkeys, Mezon__Api__ChannelMessageHeader **out_headers);
bool check_user_channel_mute(int64_t user_id, int64_t channel_id);
uint8_t *get_list_user_permission_cache(int64_t user_id, int64_t channel_id, int64_t clan_id, size_t *out_len, bool *hit);
Mezon__Api__ClanUserList *verify_list_clan_users_cache(int64_t clan_id, size_t *n_users);
void users_online(const Mezon__Api__ClanUserList *user_ids, size_t n_ids, bool *out_statuses);
int mcache_save_online(int64_t user_id);
int mcache_delete_online(int64_t user_id);
int mcache_update_session(int64_t user_id, uint8_t auth_src, const char *new_sid, const uint8_t *value, size_t value_len);
clan_badge_count_t *get_badge_count_clan(int64_t clan_id, int64_t user_id);
void free_clan_badge_count(clan_badge_count_t *badge);
uint8_t *get_uidctrlk_cache(int64_t user_id, size_t *out_len, bool *hit);
Mezon__Api__VoiceChannelUser *get_voice_channel_user(int64_t channel_id);
int get_expired_ttl_seconds(int64_t user_id, int64_t channel_id, int32_t *out_ttl);

#endif
