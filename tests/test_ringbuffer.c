#include "util/ringbuffer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  sfu_spsc_ring_t ring;
  assert(sfu_spsc_ring_init(&ring, 4) == 0);

  void *out;
  assert(sfu_spsc_ring_pop(&ring, &out) == false); /* empty */

  int a = 1, b = 2, c = 3, d = 4, e = 5;
  assert(sfu_spsc_ring_push(&ring, &a));
  assert(sfu_spsc_ring_push(&ring, &b));
  assert(sfu_spsc_ring_push(&ring, &c));
  assert(sfu_spsc_ring_push(&ring, &d));
  assert(sfu_spsc_ring_push(&ring, &e) == false); /* full at capacity=4 */
  assert(sfu_spsc_ring_high_water(&ring) == 4);
  assert(sfu_spsc_ring_push_failures(&ring) == 1);

  assert(sfu_spsc_ring_pop(&ring, &out) && out == &a);
  assert(sfu_spsc_ring_pop(&ring, &out) && out == &b);

  /* Wrap-around: push two more after popping two. */
  assert(sfu_spsc_ring_push(&ring, &e));
  int f = 6;
  assert(sfu_spsc_ring_push(&ring, &f));

  assert(sfu_spsc_ring_pop(&ring, &out) && out == &c);
  assert(sfu_spsc_ring_pop(&ring, &out) && out == &d);
  assert(sfu_spsc_ring_pop(&ring, &out) && out == &e);
  assert(sfu_spsc_ring_pop(&ring, &out) && out == &f);
  assert(sfu_spsc_ring_pop(&ring, &out) == false); /* empty again */

  sfu_spsc_ring_destroy(&ring);
  printf("test_ringbuffer: OK\n");
  return 0;
}
