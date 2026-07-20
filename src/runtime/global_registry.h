#ifndef SFU_GLOBAL_REGISTRY_H
#define SFU_GLOBAL_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize the shared global registry memory state */
void sfu_global_registry_init(void);

/* Query if a ufrag is already claimed by a worker thread */
bool sfu_global_registry_lookup(const char *ufrag, uint32_t *out_worker_index);

/* Bind a ufrag to a specific worker index */
void sfu_global_registry_register_owner(const char *ufrag, uint32_t worker_index);

/* Remove a ufrag from the registry when a peer disconnects */
void sfu_global_registry_unregister(const char *ufrag);

#endif /* SFU_GLOBAL_REGISTRY_H */
