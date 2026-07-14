#include "runtime/fanout.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  sfu_fanout_mesh_t *mesh;
  int count;
  struct sockaddr_storage last_dst;
} collector_t;

static void collect(void *user_data, sfu_fanout_job_t *job) {
  collector_t *c = (collector_t *)user_data;
  c->count++;
  c->last_dst = job->dst;
  sfu_fanout_mesh_free_job(c->mesh, job);
}

static struct sockaddr_storage make_addr(uint16_t port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  struct sockaddr_storage s;
  memset(&s, 0, sizeof(s));
  memcpy(&s, &a, sizeof(a));
  return s;
}

int main(void) {
  sfu_fanout_mesh_t mesh;
  assert(sfu_fanout_mesh_init(&mesh, 4, 128, 64) == 0);

  sfu_packet_t fake_pkt;
  memset(&fake_pkt, 0, sizeof(fake_pkt));

  struct sockaddr_storage s1 = make_addr(1111);
  struct sockaddr_storage s2 = make_addr(2222);
  struct sockaddr_storage s3 = make_addr(3333);

  /* worker 0 -> worker 1 (dst s1), worker 0 -> worker 2 (dst s2),
   * worker 3 -> worker 1 (dst s3). Mirrors a publisher on core 0
   * reaching subscribers on cores 1 and 2, plus another publisher on
   * core 3 reaching a subscriber on core 1. */
  assert(sfu_fanout_mesh_enqueue(&mesh, 0, 1, &fake_pkt, &s1,
                                 sizeof(struct sockaddr_in)));
  assert(sfu_fanout_mesh_enqueue(&mesh, 0, 2, &fake_pkt, &s2,
                                 sizeof(struct sockaddr_in)));
  assert(sfu_fanout_mesh_enqueue(&mesh, 3, 1, &fake_pkt, &s3,
                                 sizeof(struct sockaddr_in)));

  collector_t c = {.mesh = &mesh, .count = 0};

  /* Worker 1's column should see both jobs addressed to it, from two
   * different source workers (0 and 3). */
  unsigned drained = sfu_fanout_mesh_drain(&mesh, 1, 16, collect, &c);
  assert(drained == 2);
  assert(c.count == 2);

  /* Worker 2's column: exactly the one job from worker 0. */
  c.count = 0;
  drained = sfu_fanout_mesh_drain(&mesh, 2, 16, collect, &c);
  assert(drained == 1);
  assert(c.count == 1);

  /* Worker 0's column: nothing was ever addressed to worker 0. */
  c.count = 0;
  drained = sfu_fanout_mesh_drain(&mesh, 0, 16, collect, &c);
  assert(drained == 0);

  /* Job pool round-trips correctly: having freed every job drained
   * above, we should be able to allocate the full pool capacity again
   * without exhaustion. */
  for (int i = 0; i < 64; i++) {
    assert(sfu_fanout_mesh_enqueue(&mesh, 0, 1, &fake_pkt, &s1,
                                   sizeof(struct sockaddr_in)));
  }
  c.count = 0;
  drained = sfu_fanout_mesh_drain(&mesh, 1, 100, collect, &c);
  assert(drained == 64);

  sfu_fanout_mesh_destroy(&mesh);
  printf("test_fanout_mesh: OK\n");
  return 0;
}
