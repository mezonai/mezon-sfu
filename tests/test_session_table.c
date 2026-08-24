#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "room/room.h"
#include "room/room_media_graph.h"
#include "runtime/routing_context.h"
#include "transport/dtls/dtls.h"
#include "util/alloc.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_addr(struct sockaddr_storage *addr, socklen_t *len, const char *ip, uint16_t port) {
  memset(addr, 0, sizeof(*addr));
  struct sockaddr_in *in = (struct sockaddr_in *)addr;
  in->sin_family = AF_INET;
  in->sin_port = htons(port);
  inet_pton(AF_INET, ip, &in->sin_addr);
  *len = sizeof(struct sockaddr_in);
}

/* ---------------------------------------------------------------------------
 * Single-threaded lifecycle tests
 * ------------------------------------------------------------------------- */
static void test_basic_lifecycle(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);

  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  struct sockaddr_storage addr1, addr2, addr3;
  socklen_t len1, len2, len3;
  make_addr(&addr1, &len1, "127.0.0.1", 5001);
  make_addr(&addr2, &len2, "127.0.0.1", 5002);
  make_addr(&addr3, &len3, "192.168.1.100", 6000);

  /* Create sessions (each returns a caller pin). */
  sfu_peer_session_t *s1 = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1 != NULL);
  assert(s1->cold->table_index == 0);
  assert(table.active_count == 1);
  assert(s1->active == true);
  assert(sfu_session_accepts_work(s1));
  assert(atomic_load(&s1->lifecycle) == SFU_SESSION_LIFECYCLE_OPEN);
  assert(!atomic_load(&s1->media.ptt_active));
  assert(atomic_load(&s1->media.camera_enabled));
  assert(!atomic_load(&s1->media.screen_enabled));
  assert(!atomic_load(&s1->media.camera_rtp_observed));
  assert(!atomic_load(&s1->media.screen_rtp_observed));
  assert(!s1->negotiation.negotiation_needed);
  assert(!s1->negotiation.offer_outstanding);
  assert(!s1->negotiation.renegotiation_pending);
  assert(s1->negotiation.desired_offer_revision == 0);
  assert(s1->negotiation.offered_revision == 0);
  assert(s1->negotiation.answered_revision == 0);
  assert(s1->negotiation.negotiation_retry_count == 0);
  assert(!sfu_session_video_runtime_ready(s1));
  pthread_mutex_lock(&s1->media.lock);
  s1->media.uplink_video.ssrc = 1234;
  atomic_store(&s1->media.video_send_negotiated, true);
  atomic_store(&s1->media.camera_rtp_observed, true);
  assert(sfu_session_recompute_video_activity_locked(s1));
  assert(s1->media.uplink_video.active);
  atomic_store(&s1->media.camera_enabled, false);
  assert(sfu_session_recompute_video_activity_locked(s1));
  assert(!s1->media.uplink_video.active);
  s1->media.uplink_video.ssrc = 0;
  atomic_store(&s1->media.video_send_negotiated, false);
  atomic_store(&s1->media.camera_rtp_observed, false);
  atomic_store(&s1->media.camera_enabled, true);
  pthread_mutex_unlock(&s1->media.lock);
  assert(s1->egress.gcc_ctx == NULL && s1->egress.twcc_history == NULL && s1->egress.twcc_recv == NULL && s1->egress.schedulers == NULL &&
         s1->egress.rtx_cache == NULL);
  assert(sfu_session_ensure_video_runtime(s1));
  assert(sfu_session_video_runtime_ready(s1));
  assert(s1->egress.gcc_ctx && s1->egress.twcc_history && s1->egress.twcc_recv && s1->egress.schedulers && s1->egress.rtx_cache);

  sfu_peer_session_t *s1_again = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1_again == s1);

  sfu_peer_session_t *s2 = sfu_session_table_get_or_create(&table, &addr2, len2);
  assert(s2 != NULL);
  assert(s2 != s1);
  assert(!sfu_session_video_runtime_ready(s2));
  assert(s2->egress.rtx_cache == NULL);

  /* Lookup by address (acquired pins). */
  sfu_peer_session_t *f;
  f = sfu_session_table_find(&table, &addr1, len1);
  assert(f == s1);
  sfu_session_release(f);
  f = sfu_session_table_find(&table, &addr2, len2);
  assert(f == s2);
  sfu_session_release(f);
  assert(sfu_session_table_find(&table, &addr3, len3) == NULL);

  /* Index & lookup by ufrag. */
  strncpy(s1->cold->ufrag, "ufrag_alice", sizeof(s1->cold->ufrag) - 1);
  assert(sfu_session_table_index_ufrag(&table, s1));

  strncpy(s2->cold->ufrag, "ufrag_bob", sizeof(s2->cold->ufrag) - 1);
  assert(sfu_session_table_index_ufrag(&table, s2));

  f = sfu_session_table_find_by_ufrag(&table, "ufrag_alice");
  assert(f == s1);
  sfu_session_release(f);
  f = sfu_session_table_find_by_ufrag(&table, "ufrag_bob");
  assert(f == s2);
  sfu_session_release(f);
  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_charlie") == NULL);

  /* Rebind address. */
  assert(sfu_session_table_rebind_addr(&table, s1, &addr3, len3));
  assert(sfu_session_table_find(&table, &addr1, len1) == NULL);
  f = sfu_session_table_find(&table, &addr3, len3);
  assert(f == s1);
  sfu_session_release(f);

  /* Close s1: first transition is effective, repeats are idempotent. */
  assert(sfu_session_begin_close(&table, s1));
  assert(s1->cold->table_index == UINT32_MAX);
  assert(table.active_count == 1);
  assert(table.free_count == 1);
  assert(!sfu_session_begin_close(&table, s1));
  assert(!sfu_session_accepts_work(s1));
  assert(atomic_load(&s1->lifecycle) == SFU_SESSION_LIFECYCLE_CLOSING);
  /* active stays true through logical close so teardown can destroy DTLS. */
  assert(s1->active == true);

  /* Acquired pointer remains fully usable after begin_close. */
  assert(s1->cold != NULL);
  assert(s1->cold->addr_len == len3);

  /* Lookup-after-close returns NULL. */
  assert(sfu_session_table_find(&table, &addr3, len3) == NULL);
  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_alice") == NULL);

  /* s2 unaffected. */
  f = sfu_session_table_find(&table, &addr2, len2);
  assert(f == s2);
  sfu_session_release(f);
  f = sfu_session_table_find_by_ufrag(&table, "ufrag_bob");
  assert(f == s2);
  sfu_session_release(f);

  /* Closing/nonmember rebind+index are rejected. */
  assert(!sfu_session_table_rebind_addr(&table, s1, &addr1, len1));
  assert(!sfu_session_table_index_ufrag(&table, s1));

  /* Hole reuse: a new create reuses s1's cleared slot (count must not grow
   * past the high-water mark of 2). */
  uint32_t count_before = table.count;
  sfu_peer_session_t *s3 = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s3 != NULL);
  assert(s3 != s1);
  assert(s3->cold->table_index == 0);
  assert(table.active_count == 2);
  assert(table.free_count == 0);
  assert(table.count == count_before);

  /* Release caller pins; s1's final release frees it exactly once (the table
   * ref was already dropped by begin_close). */
  sfu_session_release(s1_again);
  sfu_session_release(s1);
  sfu_session_release(s3);
  sfu_session_release(s2);

  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

/* Failed construction (invalid address) leaves no slot, no hole, no hash. */
static void test_failed_construction(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);

  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  struct sockaddr_storage addr1;
  socklen_t len1;
  make_addr(&addr1, &len1, "127.0.0.1", 5001);

  assert(sfu_session_table_get_or_create(&table, NULL, 0) == NULL);
  assert(sfu_session_table_get_or_create(&table, &addr1, 0) == NULL);
  assert(sfu_session_table_get_or_create(&table, &addr1, sizeof(addr1) + 1) == NULL);
  assert(table.count == 0);
  assert(sfu_session_table_find(&table, &addr1, len1) == NULL);

  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

/* ---------------------------------------------------------------------------
 * Concurrent find-acquire vs begin_close (TSan target)
 * ------------------------------------------------------------------------- */

#define RACE_ITERS 200
#define RACE_READERS 4

typedef struct {
  sfu_session_table_t *table;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  pthread_barrier_t barrier;
  _Atomic uint64_t acquired_ok;
} race_ctx_t;

static void *race_reader(void *arg) {
  race_ctx_t *ctx = arg;
  pthread_barrier_wait(&ctx->barrier);
  uint64_t ok = 0;
  for (int i = 0; i < RACE_ITERS; i++) {
    sfu_peer_session_t *s = sfu_session_table_find(ctx->table, &ctx->addr, ctx->addr_len);
    if (s) {
      /* Acquired pin must be usable even if begin_close races us. */
      assert(s->cold != NULL);
      assert(s->cold->addr_len == ctx->addr_len);
      sfu_session_release(s);
      ok++;
    }
  }
  atomic_fetch_add_explicit(&ctx->acquired_ok, ok, memory_order_relaxed);
  return NULL;
}

static void test_concurrent_find_vs_close(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);

  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  race_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.table = &table;
  make_addr(&ctx.addr, &ctx.addr_len, "10.0.0.1", 7000);
  pthread_barrier_init(&ctx.barrier, NULL, RACE_READERS + 1);

  sfu_peer_session_t *s = sfu_session_table_get_or_create(&table, &ctx.addr, ctx.addr_len);
  assert(s != NULL);

  pthread_t readers[RACE_READERS];
  for (int i = 0; i < RACE_READERS; i++) {
    assert(pthread_create(&readers[i], NULL, race_reader, &ctx) == 0);
  }

  pthread_barrier_wait(&ctx.barrier);
  /* Let readers acquire some pins, then close concurrently with the remaining
   * lookups; only one transition wins. */
  for (volatile int spin = 0; spin < 100000; spin++) {
  }
  assert(sfu_session_begin_close(&table, s));
  assert(!sfu_session_begin_close(&table, s));

  for (int i = 0; i < RACE_READERS; i++) {
    pthread_join(readers[i], NULL);
  }

  assert(atomic_load_explicit(&ctx.acquired_ok, memory_order_relaxed) > 0); /* readers did acquire pins before/around close */
  assert(sfu_session_table_find(&table, &ctx.addr, ctx.addr_len) == NULL);

  sfu_session_release(s); /* caller pin */
  pthread_barrier_destroy(&ctx.barrier);
  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

/* ---------------------------------------------------------------------------
 * Routing table tests
 * ------------------------------------------------------------------------- */
static sfu_pending_answer_t make_pending(uint32_t peer_id, bool is_audience) {
  sfu_pending_answer_t answer;
  memset(&answer, 0, sizeof(answer));
  answer.audio_ssrc = 111;
  answer.video_ssrc = 222;
  answer.rtx_ssrc = 333;
  answer.video_pt = 96;
  answer.rtx_pt = 97;
  answer.video_codec = SFU_VIDEO_CODEC_VP8;
  answer.twcc_recv_extmap_id = 6;
  answer.twcc_send_extmap_id = 5;
  answer.peer_id = peer_id;
  answer.user_id = 9001;
  answer.generation = 1;
  answer.audio_section_present = true;
  answer.video_section_present = true;
  answer.audio_sends = !is_audience;
  answer.video_sends = !is_audience;
  answer.is_audience = is_audience;
  answer.valid = true;
  return answer;
}

static void test_routing_table(void) {
  sfu_routing_table_t rtable;
  assert(sfu_routing_table_init(&rtable) == 0);

  sfu_room_t dummy_room;
  memset(&dummy_room, 0, sizeof(dummy_room));

  sfu_pending_answer_t answer = make_pending(42, true);
  uint32_t generation = 0;
  assert(sfu_routing_table_register_answer(&rtable, "ufrag_bob", &dummy_room, 10, &answer, &generation) == SFU_ROUTING_REGISTER_OK);
  assert(generation != 0);

  sfu_room_t other_room;
  memset(&other_room, 0, sizeof(other_room));
  assert(sfu_routing_table_register_answer(&rtable, "", &dummy_room, 10, &answer, NULL) == SFU_ROUTING_REGISTER_INVALID_ARGUMENT);
  assert(sfu_routing_table_register_answer(&rtable, "ufrag_bob", &other_room, 11, &answer, NULL) == SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT);

  sfu_routing_snapshot_t route;
  assert(sfu_routing_table_lookup_route(&rtable, "ufrag_bob", 3, &route));
  assert(route.room == &dummy_room);
  assert(route.fd == 10);
  assert(route.pending_generation == generation);

  /* A later answer gets a new generation and preserves stable peer identity. */
  answer = make_pending(999, false);
  uint32_t generation2 = 0;
  assert(sfu_routing_table_register_answer(&rtable, "ufrag_bob", &dummy_room, 10, &answer, &generation2) == SFU_ROUTING_REGISTER_OK);
  assert(generation2 != generation);
  assert(sfu_routing_table_lookup_route(&rtable, "ufrag_bob", 3, &route));
  assert(route.pending_generation == generation2);

  /* Indexed lookup survives dense swap-delete and repeated removals by fd. */
  answer = make_pending(100, false);
  assert(sfu_routing_table_register_answer(&rtable, "route_a", &dummy_room, 20, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(sfu_routing_table_register_answer(&rtable, "route_b", &dummy_room, 30, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(sfu_routing_table_register_answer(&rtable, "route_c", &dummy_room, 40, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(sfu_routing_table_register_answer(&rtable, "route_d", &dummy_room, 20, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(rtable.count == 5);

  sfu_routing_table_unregister_fd(&rtable, 20);
  assert(rtable.count == 3);
  assert(!sfu_routing_table_peek_route(&rtable, "route_a", &route));
  assert(!sfu_routing_table_peek_route(&rtable, "route_d", &route));
  assert(sfu_routing_table_peek_route(&rtable, "route_b", &route) && route.fd == 30);
  assert(sfu_routing_table_peek_route(&rtable, "route_c", &route) && route.fd == 40);
  assert(sfu_routing_table_peek_route(&rtable, "ufrag_bob", &route) && route.fd == 10);

  char max_ufrag[32];
  memset(max_ufrag, 'x', sizeof(max_ufrag));
  max_ufrag[31] = '\0';
  assert(sfu_routing_table_register_answer(&rtable, max_ufrag, &dummy_room, 50, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(sfu_routing_table_peek_route(&rtable, max_ufrag, &route) && route.fd == 50);
  char too_long_ufrag[33];
  memset(too_long_ufrag, 'y', sizeof(too_long_ufrag));
  too_long_ufrag[32] = '\0';
  uint32_t count_before_invalid = rtable.count;
  assert(sfu_routing_table_register_answer(&rtable, too_long_ufrag, &dummy_room, 51, &answer, NULL) == SFU_ROUTING_REGISTER_INVALID_ARGUMENT);
  assert(rtable.count == count_before_invalid);

  sfu_routing_table_unregister_fd(&rtable, 10);
  sfu_routing_table_unregister_fd(&rtable, 30);
  sfu_routing_table_unregister_fd(&rtable, 40);
  sfu_routing_table_unregister_fd(&rtable, 50);
  assert(rtable.count == 0);

  /* Deliberately exercise collision chains and tombstone reuse. */
  int32_t first_by_bucket[SFU_ROUTING_UFRAG_HASH_SLOTS];
  for (uint32_t i = 0; i < SFU_ROUTING_UFRAG_HASH_SLOTS; i++) {
    first_by_bucket[i] = -1;
  }
  char collision_a[32] = {0};
  char collision_b[32] = {0};
  for (uint32_t i = 0; i < 200000 && collision_b[0] == '\0'; i++) {
    char candidate[32];
    snprintf(candidate, sizeof(candidate), "collision_%u", i);
    uint32_t bucket = fnv1a(candidate, strlen(candidate)) & (SFU_ROUTING_UFRAG_HASH_SLOTS - 1);
    if (first_by_bucket[bucket] >= 0) {
      snprintf(collision_a, sizeof(collision_a), "collision_%u", (uint32_t)first_by_bucket[bucket]);
      snprintf(collision_b, sizeof(collision_b), "%s", candidate);
    } else {
      first_by_bucket[bucket] = (int32_t)i;
    }
  }
  assert(collision_a[0] != '\0' && collision_b[0] != '\0');
  assert(sfu_routing_table_register_answer(&rtable, collision_a, &dummy_room, 60, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(sfu_routing_table_register_answer(&rtable, collision_b, &dummy_room, 61, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  sfu_routing_table_unregister_fd(&rtable, 60);
  assert(!sfu_routing_table_peek_route(&rtable, collision_a, &route));
  assert(sfu_routing_table_peek_route(&rtable, collision_b, &route) && route.fd == 61);
  assert(sfu_routing_table_register_answer(&rtable, collision_a, &dummy_room, 62, &answer, NULL) == SFU_ROUTING_REGISTER_OK);
  assert(sfu_routing_table_peek_route(&rtable, collision_a, &route) && route.fd == 62);
  sfu_routing_table_unregister_fd(&rtable, 61);
  sfu_routing_table_unregister_fd(&rtable, 62);
  assert(rtable.count == 0 && rtable.deleted_slots == 0);

  sfu_routing_answer_reservation_t reservation;
  assert(sfu_routing_table_prepare_answer(&rtable, "reserve_a", &dummy_room, 70, &answer, &reservation) == SFU_ROUTING_REGISTER_OK);
  assert(rtable.count == 1);
  sfu_routing_table_cancel_answer(&reservation);
  assert(rtable.count == 0);
  assert(sfu_routing_table_prepare_answer(&rtable, "reserve_a", &dummy_room, 70, &answer, &reservation) == SFU_ROUTING_REGISTER_OK);
  uint32_t reserved_generation = 0;
  assert(sfu_routing_table_commit_answer(&reservation, &reserved_generation));
  assert(reserved_generation != 0);
  assert(sfu_routing_table_peek_route(&rtable, "reserve_a", &route) && route.fd == 70);
  sfu_routing_answer_reservation_t conflict;
  assert(sfu_routing_table_prepare_answer(&rtable, "reserve_a", &other_room, 71, &answer, &conflict) == SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT);
  sfu_routing_table_unregister_fd(&rtable, 70);
  sfu_routing_table_destroy(&rtable);
}

static void test_duplicate_ufrag_rejected(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  struct sockaddr_storage addr1, addr2;
  socklen_t len1, len2;
  make_addr(&addr1, &len1, "127.0.0.1", 9201);
  make_addr(&addr2, &len2, "127.0.0.1", 9202);

  sfu_peer_session_t *s1 = sfu_session_table_get_or_create(&table, &addr1, len1);
  sfu_peer_session_t *s2 = sfu_session_table_get_or_create(&table, &addr2, len2);
  assert(s1 && s2 && s1 != s2);
  snprintf(s1->cold->ufrag, sizeof(s1->cold->ufrag), "same_ufrag");
  snprintf(s2->cold->ufrag, sizeof(s2->cold->ufrag), "same_ufrag");
  assert(sfu_session_table_index_ufrag(&table, s1));
  assert(!sfu_session_table_index_ufrag(&table, s2));

  sfu_peer_session_t *found = sfu_session_table_find_by_ufrag(&table, "same_ufrag");
  assert(found == s1);
  sfu_session_release(found);
  sfu_session_release(s1);
  sfu_session_release(s2);
  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

#define ICE_CREATORS 8
typedef struct {
  sfu_session_table_t *table;
  pthread_barrier_t barrier;
  sfu_peer_session_t *results[ICE_CREATORS];
  struct sockaddr_storage addrs[ICE_CREATORS];
  socklen_t lens[ICE_CREATORS];
} ice_create_ctx_t;

typedef struct {
  ice_create_ctx_t *ctx;
  int index;
} ice_create_arg_t;

static void *ice_creator(void *arg_) {
  ice_create_arg_t *arg = arg_;
  pthread_barrier_wait(&arg->ctx->barrier);
  arg->ctx->results[arg->index] =
      sfu_session_table_get_or_create_by_ufrag(arg->ctx->table, &arg->ctx->addrs[arg->index], arg->ctx->lens[arg->index], "shared", false, NULL);
  return NULL;
}

static void test_concurrent_same_ufrag_creation(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  ice_create_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.table = &table;
  pthread_barrier_init(&ctx.barrier, NULL, ICE_CREATORS);

  pthread_t threads[ICE_CREATORS];
  ice_create_arg_t args[ICE_CREATORS];
  for (int i = 0; i < ICE_CREATORS; i++) {
    char ip[32];
    snprintf(ip, sizeof(ip), "10.0.1.%d", i + 1);
    make_addr(&ctx.addrs[i], &ctx.lens[i], ip, (uint16_t)(9300 + i));
    args[i].ctx = &ctx;
    args[i].index = i;
    assert(pthread_create(&threads[i], NULL, ice_creator, &args[i]) == 0);
  }
  for (int i = 0; i < ICE_CREATORS; i++) {
    pthread_join(threads[i], NULL);
  }

  assert(ctx.results[0] != NULL);
  for (int i = 1; i < ICE_CREATORS; i++) {
    assert(ctx.results[i] == ctx.results[0]);
  }
  uint32_t members = 0;
  for (uint32_t i = 0; i < table.count; i++) {
    if (table.sessions[i]) {
      members++;
    }
  }
  assert(members == 1);

  for (int i = 0; i < ICE_CREATORS; i++) {
    sfu_session_release(ctx.results[i]);
  }
  pthread_barrier_destroy(&ctx.barrier);
  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

static void test_pending_answer_application(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  struct sockaddr_storage addr;
  socklen_t addr_len;
  make_addr(&addr, &addr_len, "127.0.0.1", 9400);
  sfu_peer_session_t *s = sfu_session_table_get_or_create_by_ufrag(&table, &addr, addr_len, "ufrag_aud", true, NULL);
  assert(s != NULL);

  sfu_pending_answer_t answer = make_pending(77, true);
  sfu_routing_table_t rtable;
  sfu_room_t room;
  memset(&room, 0, sizeof(room));
  assert(sfu_routing_table_init(&rtable) == 0);
  uint32_t generation = 0;
  assert(sfu_routing_table_register_answer(&rtable, "ufrag_aud", &room, 20, &answer, &generation) == SFU_ROUTING_REGISTER_OK);
  bool role_changed = false;
  bool media_changed = false;
  assert(!sfu_routing_table_reconcile_answer(&rtable, "ufrag_aud", &room, 21, generation, s, &role_changed, &media_changed));
  assert(sfu_routing_table_reconcile_answer(&rtable, "ufrag_aud", &room, 20, generation, s, &role_changed, &media_changed));
  assert(!sfu_routing_table_reconcile_answer(&rtable, "ufrag_aud", &room, 20, generation, s, NULL, NULL));
  assert(atomic_load(&s->is_audience));
  assert(s->peer_id == 77);
  assert(s->fd == 20);
  assert(s->media.uplink_audio.ssrc == 0 && !s->media.uplink_audio.active);
  assert(s->media.uplink_video.ssrc == 0 && !s->media.uplink_video.active);
  assert(media_changed);

  /* A stale generation cannot revert a newer applied answer. */
  sfu_pending_answer_t stale = answer;
  stale.generation = 1;
  stale.is_audience = false;
  stale.audio_sends = true;
  stale.video_sends = true;
  stale.audio_ssrc = 999;
  stale.video_ssrc = 888;
  assert(!sfu_session_apply_pending_answer(s, &stale, 21, &role_changed, &media_changed));
  assert(atomic_load(&s->is_audience));
  assert(s->media.uplink_video.ssrc == 0);
  assert(s->fd == 20);

  sfu_routing_table_destroy(&rtable);
  sfu_session_release(s);
  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

static void test_established_session_rebind(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  struct sockaddr_storage addr1, addr2;
  socklen_t len1, len2;
  make_addr(&addr1, &len1, "127.0.0.1", 9501);
  make_addr(&addr2, &len2, "127.0.0.1", 9502);

  sfu_session_rebind_result_t rebind = SFU_SESSION_REBIND_UNCHANGED;
  sfu_peer_session_t *session = sfu_session_table_get_or_create_by_ufrag(&table, &addr1, len1, "rebind", true, &rebind);
  assert(session != NULL);
  assert(rebind == SFU_SESSION_REBIND_UNCHANGED);
  session->state = SFU_SESSION_ESTABLISHED;
  session->room = (sfu_room_t *)(uintptr_t)0x1234;
  uint32_t peer_id = session->peer_id;

  sfu_peer_session_t *same = sfu_session_table_get_or_create_by_ufrag(&table, &addr2, len2, "rebind", false, &rebind);
  assert(same == session);
  assert(rebind == SFU_SESSION_REBIND_UNCHANGED);
  sfu_session_release(same);
  same = sfu_session_table_find(&table, &addr1, len1);
  assert(same == session);
  sfu_session_release(same);
  assert(sfu_session_table_find(&table, &addr2, len2) == NULL);

  same = sfu_session_table_get_or_create_by_ufrag(&table, &addr2, len2, "rebind", true, &rebind);
  assert(same == session);
  assert(rebind == SFU_SESSION_REBIND_APPLIED);
  assert(same->state == SFU_SESSION_ESTABLISHED);
  assert(same->peer_id == peer_id);
  assert(same->room == (sfu_room_t *)(uintptr_t)0x1234);
  sfu_session_release(same);
  assert(sfu_session_table_find(&table, &addr1, len1) == NULL);
  same = sfu_session_table_find(&table, &addr2, len2);
  assert(same == session);
  sfu_session_release(same);
  same = sfu_session_table_find_by_ufrag(&table, "rebind");
  assert(same == session);
  sfu_session_release(same);

  session->room = NULL;
  sfu_session_release(session);
  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

static void test_established_rebind_conflict(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);

  struct sockaddr_storage addr1, addr2;
  socklen_t len1, len2;
  make_addr(&addr1, &len1, "127.0.0.1", 9511);
  make_addr(&addr2, &len2, "127.0.0.1", 9512);
  sfu_peer_session_t *a = sfu_session_table_get_or_create_by_ufrag(&table, &addr1, len1, "rebind_a", true, NULL);
  sfu_peer_session_t *b = sfu_session_table_get_or_create_by_ufrag(&table, &addr2, len2, "rebind_b", true, NULL);
  assert(a && b && a != b);
  a->state = SFU_SESSION_ESTABLISHED;
  b->state = SFU_SESSION_ESTABLISHED;

  sfu_session_rebind_result_t rebind = SFU_SESSION_REBIND_UNCHANGED;
  assert(sfu_session_table_get_or_create_by_ufrag(&table, &addr2, len2, "rebind_a", true, &rebind) == NULL);
  assert(rebind == SFU_SESSION_REBIND_REJECTED);
  sfu_peer_session_t *found = sfu_session_table_find(&table, &addr1, len1);
  assert(found == a);
  sfu_session_release(found);
  found = sfu_session_table_find(&table, &addr2, len2);
  assert(found == b);
  sfu_session_release(found);

  sfu_session_release(a);
  sfu_session_release(b);
  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

typedef struct {
  sfu_session_table_t *table;
  sfu_peer_session_t *session;
  sfu_room_t *room;
  _Atomic bool started;
  bool result;
} close_add_race_ctx_t;

static void *begin_close_thread(void *arg) {
  close_add_race_ctx_t *ctx = arg;
  atomic_store_explicit(&ctx->started, true, memory_order_release);
  ctx->result = sfu_session_begin_close(ctx->table, ctx->session);
  return NULL;
}

static void *room_add_thread(void *arg) {
  close_add_race_ctx_t *ctx = arg;
  ctx->result = room_add_peer(ctx->room, ctx->session);
  return NULL;
}

static void test_begin_close_serializes_room_admission(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);
  sfu_room_t room;
  assert(sfu_room_init(&room, 17) == 0);

  struct sockaddr_storage addr;
  socklen_t addr_len;
  make_addr(&addr, &addr_len, "127.0.0.1", 9701);
  sfu_peer_session_t *session = sfu_session_table_get_or_create(&table, &addr, addr_len);
  assert(session);

  /* Hold the inner table lock so begin_close deterministically holds the outer
   * membership lock while waiting.  Admission started afterward must wait and
   * then observe CLOSING rather than commit a room membership. */
  pthread_rwlock_wrlock(&table.lock);
  close_add_race_ctx_t close_ctx = {.table = &table, .session = session};
  pthread_t close_tid;
  assert(pthread_create(&close_tid, NULL, begin_close_thread, &close_ctx) == 0);
  while (!atomic_load_explicit(&close_ctx.started, memory_order_acquire)) {
    sched_yield();
  }
  for (;;) {
    int rc = pthread_mutex_trylock(&session->membership_lock);
    if (rc == EBUSY) {
      break;
    }
    assert(rc == 0);
    pthread_mutex_unlock(&session->membership_lock);
    sched_yield();
  }

  close_add_race_ctx_t add_ctx = {.session = session, .room = &room};
  pthread_t add_tid;
  assert(pthread_create(&add_tid, NULL, room_add_thread, &add_ctx) == 0);
  pthread_rwlock_unlock(&table.lock);

  pthread_join(close_tid, NULL);
  pthread_join(add_tid, NULL);
  assert(close_ctx.result);
  assert(!add_ctx.result);
  assert(atomic_load(&session->lifecycle) == SFU_SESSION_LIFECYCLE_CLOSING);
  assert(session->room == NULL && session->room_slot == UINT32_MAX);
  assert(room.peer_count == 0 && room.membership_revision == 0);

  sfu_session_release(session);
  sfu_session_table_destroy(&table);
  sfu_room_destroy(&room);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

static void test_destroy_cleans_room_memberships(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx, NULL, 0) == 0);
  sfu_room_t room;
  assert(sfu_room_init(&room, 16) == 0);

  struct sockaddr_storage addr1, addr2;
  socklen_t len1, len2;
  make_addr(&addr1, &len1, "127.0.0.1", 9601);
  make_addr(&addr2, &len2, "127.0.0.1", 9602);
  sfu_peer_session_t *a = sfu_session_table_get_or_create(&table, &addr1, len1);
  sfu_peer_session_t *b = sfu_session_table_get_or_create(&table, &addr2, len2);
  assert(a && b);
  assert(room_add_peer(&room, a));
  assert(room_add_peer(&room, b));
  assert(room.peer_count == 2);

  sfu_session_table_destroy(&table);

  assert(room.peer_count == 0);
  assert(a->room == NULL && a->room_slot == UINT32_MAX);
  assert(b->room == NULL && b->room_slot == UINT32_MAX);
  assert(atomic_load(&a->lifecycle) == SFU_SESSION_LIFECYCLE_CLOSING);
  assert(atomic_load(&b->lifecycle) == SFU_SESSION_LIFECYCLE_CLOSING);
  sfu_session_release(a);
  sfu_session_release(b);
  sfu_room_destroy(&room);
  sfu_dtls_ctx_destroy(&dtls_ctx);
}

static void test_remote_slot_convergence(void) {
  sfu_peer_session_t session = {0};
  assert(pthread_mutex_init(&session.graph.lock, NULL) == 0);

  uint32_t slot0, slot1;
  uint64_t generation0, generation1;
  uint32_t active_unapplied = 0;
  uint32_t obsolete_applied = 0;
  assert(sfu_session_remote_slot_reserve(&session, 3001, 31, &slot0, &generation0));
  assert(sfu_session_remote_slots_pending(&session, &active_unapplied, &obsolete_applied));
  assert(active_unapplied == 1 && obsolete_applied == 0);

  sfu_remote_offer_manifest_t *first = sfu_session_remote_offer_capture(&session);
  assert(first && sfu_session_remote_offer_install(&session, first));
  assert(sfu_session_remote_slot_reserve(&session, 3002, 32, &slot1, &generation1));
  assert(sfu_session_remote_offer_apply_answer(&session, first));
  sfu_remote_offer_manifest_release(first);
  assert(sfu_session_remote_slots_pending(&session, &active_unapplied, &obsolete_applied));
  assert(active_unapplied == 1 && obsolete_applied == 0);
  assert(sfu_session_remote_slot_authorized(&session, slot0, generation0));
  assert(!sfu_session_remote_slot_authorized(&session, slot1, generation1));

  sfu_remote_offer_manifest_t *second = sfu_session_remote_offer_capture(&session);
  assert(second && sfu_session_remote_offer_install(&session, second));
  assert(sfu_session_remote_slot_retire(&session, slot1, generation1));
  assert(sfu_session_remote_offer_apply_answer(&session, second));
  sfu_remote_offer_manifest_release(second);
  assert(sfu_session_remote_slots_pending(&session, &active_unapplied, &obsolete_applied));
  assert(active_unapplied == 0 && obsolete_applied == 1);

  sfu_remote_offer_manifest_t *omission = sfu_session_remote_offer_capture(&session);
  assert(omission && omission->assignment_generations[slot1] == 0);
  assert(sfu_session_remote_offer_install(&session, omission));
  assert(sfu_session_remote_offer_apply_answer(&session, omission));
  sfu_remote_offer_manifest_release(omission);
  assert(!sfu_session_remote_slots_pending(&session, &active_unapplied, &obsolete_applied));
  assert(active_unapplied == 0 && obsolete_applied == 0);

  uint32_t reused;
  uint64_t reused_generation;
  assert(sfu_session_remote_slot_reserve(&session, 3003, 33, &reused, &reused_generation));
  assert(reused == slot1 && reused_generation != generation1);
  assert(!sfu_session_remote_slot_authorized(&session, reused, reused_generation));
  assert(sfu_session_remote_slots_pending(&session, &active_unapplied, &obsolete_applied));
  sfu_remote_offer_manifest_t *reuse = sfu_session_remote_offer_capture(&session);
  assert(reuse && sfu_session_remote_offer_install(&session, reuse));
  assert(sfu_session_remote_offer_apply_answer(&session, reuse));
  sfu_remote_offer_manifest_release(reuse);
  assert(sfu_session_remote_slot_authorized(&session, reused, reused_generation));
  assert(!sfu_session_remote_slots_pending(&session, NULL, NULL));

  sfu_session_remote_slots_teardown(&session);
  pthread_mutex_destroy(&session.graph.lock);
}

static void test_remote_slot_lifecycle(void) {
  sfu_peer_session_t session = {0};
  session.room_slot = UINT32_MAX;
  assert(pthread_mutex_init(&session.graph.lock, NULL) == 0);

  uint32_t slot0, slot1;
  uint64_t generation0, generation1;
  assert(sfu_session_remote_slot_reserve(&session, 1001, 11, &slot0, &generation0));
  assert(slot0 == 0 && generation0 != 0);
  assert(sfu_session_remote_slot_reserve(&session, 1002, 12, &slot1, &generation1));
  assert(slot1 == 1 && generation1 != 0 && generation1 != generation0);
  assert(sfu_session_remote_slot_high_water(&session) == 2);

  sfu_remote_offer_manifest_t *first = sfu_session_remote_offer_capture(&session);
  assert(first && first->high_water_slots == 2);
  assert(first->assignment_generations[slot0] == generation0);
  assert(first->assignment_generations[slot1] == generation1);
  assert(!sfu_session_remote_slot_authorized(&session, slot0, generation0));
  assert(sfu_session_remote_offer_install(&session, first));
  assert(sfu_session_remote_offer_apply_answer(&session, first));
  assert(sfu_session_remote_slot_authorized(&session, slot0, generation0));
  assert(sfu_session_remote_slot_authorized(&session, slot1, generation1));
  sfu_remote_offer_manifest_release(first);

  assert(sfu_session_remote_slot_retire(&session, slot0, generation0));
  uint32_t no_slot;
  uint64_t no_generation;
  assert(sfu_session_remote_slot_reserve(&session, 1003, 13, &no_slot, &no_generation));
  assert(no_slot == 2); /* RETIRING is not reusable before its omission is answered. */
  sfu_remote_offer_manifest_t *second = sfu_session_remote_offer_capture(&session);
  assert(second && second->assignment_generations[slot0] == 0);
  assert(sfu_session_remote_offer_install(&session, second));
  assert(sfu_session_remote_offer_apply_answer(&session, second));
  assert(!sfu_session_remote_slot_authorized(&session, slot0, generation0));
  sfu_remote_offer_manifest_release(second);

  uint32_t reused;
  uint64_t reused_generation;
  assert(sfu_session_remote_slot_reserve(&session, 1004, 14, &reused, &reused_generation));
  assert(reused == slot0 && reused_generation != 0 && reused_generation != generation0);

  sfu_remote_offer_manifest_t *stale = sfu_session_remote_offer_capture(&session);
  sfu_remote_offer_manifest_t *current = sfu_session_remote_offer_capture(&session);
  assert(stale && current && stale->offer_generation != current->offer_generation);
  assert(sfu_session_remote_offer_install(&session, current));
  assert(!sfu_session_remote_offer_apply_answer(&session, stale));
  assert(sfu_session_remote_offer_apply_answer(&session, current));
  sfu_remote_offer_manifest_release(stale);
  sfu_remote_offer_manifest_release(current);

  /* Trailing FREE slots trim high-water after the inactive offer is answered. */
  assert(sfu_session_remote_slot_retire(&session, no_slot, no_generation));
  sfu_remote_offer_manifest_t *trim = sfu_session_remote_offer_capture(&session);
  assert(sfu_session_remote_offer_install(&session, trim));
  assert(sfu_session_remote_offer_apply_answer(&session, trim));
  sfu_remote_offer_manifest_release(trim);
  assert(sfu_session_remote_slot_high_water(&session) <= 2);

  assert(sfu_session_remote_slot_retire(&session, slot1, generation1));
  sfu_remote_offer_manifest_t *trim_slot1 = sfu_session_remote_offer_capture(&session);
  assert(sfu_session_remote_offer_install(&session, trim_slot1));
  assert(sfu_session_remote_offer_apply_answer(&session, trim_slot1));
  sfu_remote_offer_manifest_release(trim_slot1);

  uint32_t last_slot = reused;
  uint64_t last_generation = reused_generation;
  for (int i = 0; i < 1000; i++) {
    assert(sfu_session_remote_slot_retire(&session, last_slot, last_generation));
    sfu_remote_offer_manifest_t *inactive = sfu_session_remote_offer_capture(&session);
    assert(inactive && inactive->assignment_generations[last_slot] == 0);
    assert(sfu_session_remote_offer_install(&session, inactive));
    assert(sfu_session_remote_offer_apply_answer(&session, inactive));
    sfu_remote_offer_manifest_release(inactive);
    assert(sfu_session_remote_slot_reserve(&session, 2000 + i, (uint32_t)(20 + i), &last_slot, &last_generation));
    sfu_remote_offer_manifest_t *active = sfu_session_remote_offer_capture(&session);
    assert(active && active->assignment_generations[last_slot] == last_generation);
    assert(!sfu_session_remote_slot_authorized(&session, last_slot, last_generation));
    assert(sfu_session_remote_offer_install(&session, active));
    assert(sfu_session_remote_offer_apply_answer(&session, active));
    assert(sfu_session_remote_slot_authorized(&session, last_slot, last_generation));
    sfu_remote_offer_manifest_release(active);
  }
  assert(last_slot == slot0);
  sfu_session_graph_assert_invariants(&session);

  sfu_session_remote_slots_teardown(&session);
  assert(sfu_session_remote_slot_high_water(&session) == 0);
  pthread_mutex_destroy(&session.graph.lock);
}

int main(void) {
  sfu_signaling_server_t signaling;
  sfu_signaling_membership_test_server_init(&signaling);
  signaling.test_auto_drain = true;
  test_basic_lifecycle();
  test_remote_slot_convergence();
  test_remote_slot_lifecycle();
  test_failed_construction();
  test_concurrent_find_vs_close();
  test_routing_table();
  test_duplicate_ufrag_rejected();
  test_concurrent_same_ufrag_creation();
  test_pending_answer_application();
  test_established_session_rebind();
  test_established_rebind_conflict();
  test_begin_close_serializes_room_admission();
  test_destroy_cleans_room_memberships();
  sfu_signaling_membership_test_server_stop(&signaling);

  printf("test_session_table: OK\n");
  return 0;
}
