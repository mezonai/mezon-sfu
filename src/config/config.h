#ifndef SFU_CONFIG_H
#define SFU_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_MAX_WORKERS 16
#define SFU_CACHELINE_SIZE 64

#define SFU_LIKELY(x) __builtin_expect(!!(x), 1)
#define SFU_UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct {
  uint16_t media_port;
  uint16_t signaling_port;
  char public_host[256];

  char nats_url[256];
  char nats_client_name[128];

  uint32_t packet_buf_size;
  uint32_t packet_pool_capacity;
  uint32_t provided_buf_count;
  int provided_buf_group_id;

  uint32_t worker_queue_capacity;
  uint32_t fanout_ring_capacity;
  uint32_t fanout_job_pool_capacity;
  uint32_t release_queue_capacity;
} sfu_config_t;

void sfu_config_set_defaults(sfu_config_t *cfg);
int sfu_config_load_ini(sfu_config_t *cfg, const char *filepath);

#endif /* SFU_CONFIG_H */
