#ifndef SFU_CONFIG_INI_H
#define SFU_CONFIG_INI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t log_level;
  uint16_t media_port;
  uint16_t signaling_port;
  char public_host[256];

  char nats_url[256];
  char nats_client_name[128];

  char jwt_secret[256];

  uint32_t packet_buf_size;
  uint32_t packet_pool_capacity;
  uint32_t provided_buf_count;
  int provided_buf_group_id;

  uint32_t worker_queue_capacity;
  uint32_t fanout_ring_capacity;
  uint32_t fanout_job_pool_capacity;
  uint32_t release_queue_capacity;
} sfu_config_t;

extern sfu_config_t g_sfu_config;

const sfu_config_t *sfu_config_get(void);
void sfu_config_set_defaults();
int sfu_config_load_ini(const char *filepath);

#endif /* SFU_CONFIG_INI_H */
