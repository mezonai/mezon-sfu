#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "runtime/routing_context.h"
#include "transport/dtls/dtls.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_addr(struct sockaddr_storage *addr, socklen_t *len, const char *ip, uint16_t port) {
  memset(addr, 0, sizeof(*addr));
  struct sockaddr_in *in = (struct sockaddr_in *)addr;
  in->sin_family = AF_INET;
  in->sin_port = htons(port);
  inet_pton(AF_INET, ip, &in->sin_addr);
  *len = sizeof(struct sockaddr_in);
}

int main(void) {
  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);

  sfu_session_table_t table;
  assert(sfu_session_table_init(&table, &dtls_ctx) == 0);

  struct sockaddr_storage addr1, addr2, addr3;
  socklen_t len1, len2, len3;
  make_addr(&addr1, &len1, "127.0.0.1", 5001);
  make_addr(&addr2, &len2, "127.0.0.1", 5002);
  make_addr(&addr3, &len3, "192.168.1.100", 6000);

  /* 1. Create sessions */
  sfu_peer_session_t *s1 = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1 != NULL);
  assert(s1->active == true);

  /* Fetching same address returns existing session */
  sfu_peer_session_t *s1_again = sfu_session_table_get_or_create(&table, &addr1, len1);
  assert(s1_again == s1);

  sfu_peer_session_t *s2 = sfu_session_table_get_or_create(&table, &addr2, len2);
  assert(s2 != NULL);
  assert(s2 != s1);

  /* 2. Lookup by address */
  assert(sfu_session_table_find(&table, &addr1, len1) == s1);
  assert(sfu_session_table_find(&table, &addr2, len2) == s2);
  assert(sfu_session_table_find(&table, &addr3, len3) == NULL);

  /* 3. Index & Lookup by ufrag */
  strncpy(s1->ufrag, "ufrag_alice", sizeof(s1->ufrag) - 1);
  sfu_session_table_index_ufrag(&table, s1);

  strncpy(s2->ufrag, "ufrag_bob", sizeof(s2->ufrag) - 1);
  sfu_session_table_index_ufrag(&table, s2);

  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_alice") == s1);
  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_bob") == s2);
  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_charlie") == NULL);

  /* 4. Rebind address */
  sfu_session_table_rebind_addr(&table, s1, &addr3, len3);
  assert(sfu_session_table_find(&table, &addr1, len1) == NULL);
  assert(sfu_session_table_find(&table, &addr3, len3) == s1);

  /* 5. Remove session & tombstone checks */
  sfu_session_table_remove(&table, s1);
  assert(sfu_session_table_find(&table, &addr3, len3) == NULL);
  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_alice") == NULL);

  /* Ensure s2 is unaffected */
  assert(sfu_session_table_find(&table, &addr2, len2) == s2);
  assert(sfu_session_table_find_by_ufrag(&table, "ufrag_bob") == s2);

  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);

  /* 6. Routing table tests */
  sfu_routing_table_t rtable;
  assert(sfu_routing_table_init(&rtable) == 0);

  sfu_room_t dummy_room;
  memset(&dummy_room, 0, sizeof(dummy_room));

  sfu_register_ufrag_room(&rtable, "ufrag_bob", &dummy_room, 10);
  sfu_register_ufrag_room(&rtable, "ufrag_eve", &dummy_room, 11);

  sfu_routing_table_set_pending_answer(&rtable, "ufrag_bob", 111, 222, 333, 96, 97);

  /* Unregister by fd */
  sfu_routing_table_unregister_fd(&rtable, 10);
  /* Verify entry with fd 10 was removed */
  assert(rtable.count == 1);
  assert(strcmp(rtable.entries[0].ufrag, "ufrag_eve") == 0);

  sfu_routing_table_destroy(&rtable);

  printf("test_session_table: OK\n");
  return 0;
}
