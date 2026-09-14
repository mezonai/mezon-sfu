#include <assert.h>
#include <stdint.h>
#include "room/room_registry.h"

static uint32_t test_bucket(uint64_t room_id) {
  room_id ^= room_id >> 30;
  room_id *= UINT64_C(0xbf58476d1ce4e5b9);
  room_id ^= room_id >> 27;
  room_id *= UINT64_C(0x94d049bb133111eb);
  room_id ^= room_id >> 31;
  return (uint32_t)room_id & (SFU_ROOM_REGISTRY_INDEX_SIZE - 1);
}

int main(void) {
  sfu_room_registry_t registry;
  assert(sfu_room_registry_init(&registry) == 0);

  const uint64_t first_id = UINT64_C(42);
  uint64_t collision_id = first_id + 1;
  while (test_bucket(collision_id) != test_bucket(first_id)) {
    collision_id++;
  }

  sfu_room_t *first = sfu_room_registry_get_or_create(&registry, first_id);
  assert(first != NULL);
  assert(sfu_room_registry_get_or_create(&registry, first_id) == first);

  sfu_room_t *collision = sfu_room_registry_get_or_create(&registry, collision_id);
  assert(collision != NULL && collision != first);
  assert(sfu_room_registry_get_or_create(&registry, collision_id) == collision);

  for (uint64_t room_id = UINT64_C(100000); registry.room_count < SFU_MAX_ROOMS; room_id++) {
    assert(sfu_room_registry_get_or_create(&registry, room_id) != NULL);
  }

  assert(sfu_room_registry_get_or_create(&registry, first_id) == first);
  assert(sfu_room_registry_get_or_create(&registry, collision_id) == collision);
  assert(sfu_room_registry_get_or_create(&registry, UINT64_MAX) == NULL);
  assert(registry.room_count == SFU_MAX_ROOMS);

  sfu_room_registry_destroy(&registry);
  return 0;
}
