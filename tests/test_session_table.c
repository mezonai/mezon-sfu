#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "runtime/routing_context.h"
#include "transport/dtls/dtls.h"
#include "util/alloc.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
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
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

  struct sockaddr_storage addr1, addr2, addr3;
  socklen_t len1, len2, len3;
  make_addr(&addr1, &len1, "127.0.0.1", 5001);
  make_addr(&addr2, &len2, "127.0.0.1", 5002);
  make_addr(&addr3, &len3, "192.168.1.100", 6000);

  /* Create sessions (each returns a caller pin). */
  sfu_peer_session_t *s1 = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1 != NULL);
  assert(s1->active == true);
  assert(sfu_session_accepts_work(s1));
  assert(atomic_load(&s1->lifecycle) == SFU_SESSION_LIFECYCLE_OPEN);
  assert(!sfu_session_video_runtime_ready(s1));
  assert(s1->gcc_ctx == NULL && s1->twcc_history == NULL && s1->twcc_recv == NULL && s1->schedulers == NULL && s1->rtx_cache == NULL);
  assert(sfu_session_ensure_video_runtime(s1));
  assert(sfu_session_video_runtime_ready(s1));
  assert(s1->gcc_ctx && s1->twcc_history && s1->twcc_recv && s1->schedulers && s1->rtx_cache);

  sfu_peer_session_t *s1_again = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1_again == s1);

  sfu_peer_session_t *s2 = sfu_session_table_get_or_create(&table, &addr2, len2);
  assert(s2 != NULL);
  assert(s2 != s1);
  assert(!sfu_session_video_runtime_ready(s2));
  assert(s2->rtx_cache == NULL);

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
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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
  volatile uint64_t acquired_ok;
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
  __atomic_fetch_add(&ctx->acquired_ok, ok, __ATOMIC_RELAXED);
  return NULL;
}

static void test_concurrent_find_vs_close(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);

  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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

  assert(ctx.acquired_ok > 0); /* readers did acquire pins before/around close */
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

  sfu_routing_table_unregister_fd(&rtable, 10);
  assert(rtable.count == 0);
  sfu_routing_table_destroy(&rtable);
}

static void test_duplicate_ufrag_rejected(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);
  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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
    if (table.sessions[i]) members++;
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
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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
  assert(s->uplink_audio.ssrc == 0 && !s->uplink_audio.active);
  assert(s->uplink_video.ssrc == 0 && !s->uplink_video.active);
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
  assert(s->uplink_video.ssrc == 0);
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
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

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

int main(void) {
  test_basic_lifecycle();
  test_failed_construction();
  test_concurrent_find_vs_close();
  test_routing_table();
  test_duplicate_ufrag_rejected();
  test_concurrent_same_ufrag_creation();
  test_pending_answer_application();
  test_established_session_rebind();
  test_established_rebind_conflict();

  printf("test_session_table: OK\n");
  return 0;
}
