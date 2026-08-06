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

  sfu_peer_session_t *s1_again = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1_again == s1);

  sfu_peer_session_t *s2 = sfu_session_table_get_or_create(&table, &addr2, len2);
  assert(s2 != NULL);
  assert(s2 != s1);

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
 * Routing table tests (unchanged behavior)
 * ------------------------------------------------------------------------- */
static void test_routing_table(void) {
  sfu_routing_table_t rtable;
  assert(sfu_routing_table_init(&rtable) == 0);

  sfu_room_t dummy_room;
  memset(&dummy_room, 0, sizeof(dummy_room));

  sfu_register_ufrag_room(&rtable, "ufrag_bob", &dummy_room, 10);
  sfu_register_ufrag_room(&rtable, "ufrag_eve", &dummy_room, 11);

  sfu_routing_table_set_pending_answer(&rtable, "ufrag_bob", 111, 222, 333, 96, 97, 0, false);

  sfu_routing_table_unregister_fd(&rtable, 10);
  assert(rtable.count == 1);
  assert(strcmp(rtable.entries[0].ufrag, "ufrag_eve") == 0);

  sfu_routing_table_destroy(&rtable);
}

int main(void) {
  test_basic_lifecycle();
  test_failed_construction();
  test_concurrent_find_vs_close();
  test_routing_table();

  printf("test_session_table: OK\n");
  return 0;
}
