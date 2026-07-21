#include "runtime/routing_context.h"
#include <string.h>

void sfu_routing_table_init(sfu_routing_table_t *table) {
  if (!table) {
    return;
  }

  /* Clear out all initial entries */
  memset(table->entries, 0, sizeof(table->entries));
  table->count = 0;

  /* Initialize thread-safety lock */
  pthread_mutex_init(&table->mutex, NULL);
}
