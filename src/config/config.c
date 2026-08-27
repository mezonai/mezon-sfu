#include "config/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/log.h"

sfu_config_t g_sfu_config;

const sfu_config_t *sfu_config_get(void) { return &g_sfu_config; }

static char *trim_whitespace(char *str) {
  while (isspace((unsigned char)*str)) {
    str++;
  }
  if (*str == 0) {
    return str;
  }
  char *end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end)) {
    end--;
  }
  end[1] = '\0';
  return str;
}
void sfu_config_set_defaults(void) {
  memset(&g_sfu_config, 0, sizeof(g_sfu_config));

  g_sfu_config.log_level = 1;
  g_sfu_config.media_port = 7000;
  g_sfu_config.signaling_port = 8000;
  snprintf(g_sfu_config.public_host, sizeof(g_sfu_config.public_host), "127.0.0.1");

  snprintf(g_sfu_config.nats_url, sizeof(g_sfu_config.nats_url), "nats://127.0.0.1:4222");
  snprintf(g_sfu_config.nats_client_name, sizeof(g_sfu_config.nats_client_name), "sfu_nats_client");
  snprintf(g_sfu_config.nats_hook_topic, sizeof(g_sfu_config.nats_hook_topic), "mezon_sfu_hook_event");
  g_sfu_config.jwt_secret[0] = '\0';

  g_sfu_config.packet_buf_size = 2048;
  g_sfu_config.packet_pool_capacity = 262144;
  g_sfu_config.provided_buf_count = 8192;
  g_sfu_config.provided_buf_group_id = 0;

  g_sfu_config.worker_queue_capacity = 16384;
  g_sfu_config.fanout_ring_capacity = 4096;
  g_sfu_config.fanout_job_pool_capacity = 16384;
  g_sfu_config.release_queue_capacity = 8192;

  g_sfu_config.bandwidth_video_pool_percent = 85;
  g_sfu_config.bandwidth_source_admission_bps = 240000;
  g_sfu_config.bandwidth_screen_preferred_bps = 800000;
  g_sfu_config.bandwidth_screen_mid_bps = 1440000;
  g_sfu_config.bandwidth_screen_cap_bps = 2500000;
  g_sfu_config.bandwidth_camera_mid_bps = 720000;
  g_sfu_config.bandwidth_camera_cap_bps = 1000000;

  snprintf(g_sfu_config.af_xdp_interface, sizeof(g_sfu_config.af_xdp_interface), "eth0");
  snprintf(g_sfu_config.af_xdp_queues, sizeof(g_sfu_config.af_xdp_queues), "auto");
  g_sfu_config.af_xdp_queue_id = 0;
  g_sfu_config.af_xdp_queue_id_set = false;
  g_sfu_config.af_xdp_queues_set = false;
  g_sfu_config.af_xdp_frame_count = 16384;
  g_sfu_config.af_xdp_frame_size = 4096;
  snprintf(g_sfu_config.af_xdp_mode, sizeof(g_sfu_config.af_xdp_mode), "native");
}

int sfu_config_validate(const sfu_config_t *config) {
  if (!config) {
    return -1;
  }
#define REQUIRE_POWER_OF_TWO(field)                                             \
  do {                                                                          \
    uint32_t value = config->field;                                             \
    if (value == 0 || (value & (value - 1)) != 0) {                             \
      SFU_LOG_ERROR(#field " must be a non-zero power of two (got %u)", value); \
      return -1;                                                                \
    }                                                                           \
  } while (0)

  if (config->media_port == 0 || config->signaling_port == 0 || config->packet_buf_size < 1200 || config->packet_pool_capacity == 0) {
    SFU_LOG_ERROR("invalid ports or packet buffers (media=%u signaling=%u packet_buf_size=%u packet_pool_capacity=%u)", config->media_port,
                  config->signaling_port, config->packet_buf_size, config->packet_pool_capacity);
    return -1;
  }
  REQUIRE_POWER_OF_TWO(provided_buf_count);
  REQUIRE_POWER_OF_TWO(worker_queue_capacity);
  REQUIRE_POWER_OF_TWO(fanout_ring_capacity);
  REQUIRE_POWER_OF_TWO(release_queue_capacity);
  if (config->fanout_job_pool_capacity < config->fanout_ring_capacity) {
    SFU_LOG_ERROR("fanout_job_pool_capacity (%u) must be >= fanout_ring_capacity (%u)", config->fanout_job_pool_capacity, config->fanout_ring_capacity);
    return -1;
  }
  if (config->packet_pool_capacity > 16777216u) {
    SFU_LOG_ERROR("packet_pool_capacity (%u) exceeds maximum 16777216 (16M slots)", config->packet_pool_capacity);
    return -1;
  }
  if (config->bandwidth_video_pool_percent == 0 || config->bandwidth_video_pool_percent > 100 || config->bandwidth_source_admission_bps == 0 ||
      config->bandwidth_source_admission_bps > config->bandwidth_screen_preferred_bps ||
      config->bandwidth_screen_preferred_bps > config->bandwidth_screen_mid_bps || config->bandwidth_screen_mid_bps > config->bandwidth_screen_cap_bps ||
      config->bandwidth_source_admission_bps > config->bandwidth_camera_mid_bps || config->bandwidth_camera_mid_bps > config->bandwidth_camera_cap_bps) {
    SFU_LOG_ERROR("invalid bandwidth policy (pool=%u admission=%u screen=%u/%u/%u camera=%u/%u)", config->bandwidth_video_pool_percent,
                  config->bandwidth_source_admission_bps, config->bandwidth_screen_preferred_bps, config->bandwidth_screen_mid_bps,
                  config->bandwidth_screen_cap_bps, config->bandwidth_camera_mid_bps, config->bandwidth_camera_cap_bps);
    return -1;
  }
#undef REQUIRE_POWER_OF_TWO
  return 0;
}

int sfu_config_load_ini(const char *filepath) {
  sfu_config_set_defaults();

  if (!filepath) {
    return 0;
  }

  FILE *fp = fopen(filepath, "r");
  if (!fp) {
    SFU_LOG_WARN("config file '%s' not found, using default values", filepath);
    return -1;
  }

  char line[512];
  char section[64] = "";

  while (fgets(line, sizeof(line), fp)) {
    char *p = trim_whitespace(line);
    if (*p == '#' || *p == ';' || *p == '\0') {
      continue;
    }

    if (*p == '[') {
      char *end = strchr(p, ']');
      if (end) {
        *end = '\0';
        snprintf(section, sizeof(section), "%s", trim_whitespace(p + 1));
      }
      continue;
    }

    char *delim = strchr(p, '=');
    if (!delim) {
      continue;
    }

    *delim = '\0';
    char *key = trim_whitespace(p);
    char *val = trim_whitespace(delim + 1);

    if (strcmp(key, "log_level") == 0) {
      g_sfu_config.log_level = (uint8_t)atoi(val);
    } else if (strcmp(key, "media_port") == 0) {
      g_sfu_config.media_port = (uint16_t)atoi(val);
    } else if (strcmp(key, "signaling_port") == 0) {
      g_sfu_config.signaling_port = (uint16_t)atoi(val);
    } else if (strcmp(key, "public_host") == 0) {
      snprintf(g_sfu_config.public_host, sizeof(g_sfu_config.public_host), "%s", val);
    } else if (strcmp(key, "nats_url") == 0) {
      snprintf(g_sfu_config.nats_url, sizeof(g_sfu_config.nats_url), "%s", val);
    } else if (strcmp(key, "nats_client_name") == 0) {
      snprintf(g_sfu_config.nats_client_name, sizeof(g_sfu_config.nats_client_name), "%s", val);
    } else if (strcmp(key, "nats_hook_topic") == 0) {
      snprintf(g_sfu_config.nats_hook_topic, sizeof(g_sfu_config.nats_hook_topic), "%s", val);
    } else if (strcmp(key, "jwt_secret") == 0) {
      snprintf(g_sfu_config.jwt_secret, sizeof(g_sfu_config.jwt_secret), "%s", val);
    } else if (strcmp(key, "packet_buf_size") == 0) {
      g_sfu_config.packet_buf_size = (uint32_t)atoi(val);
    } else if (strcmp(key, "packet_pool_capacity") == 0) {
      g_sfu_config.packet_pool_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "provided_buf_count") == 0) {
      g_sfu_config.provided_buf_count = (uint32_t)atoi(val);
    } else if (strcmp(key, "provided_buf_group_id") == 0) {
      g_sfu_config.provided_buf_group_id = atoi(val);
    } else if (strcmp(key, "worker_queue_capacity") == 0) {
      g_sfu_config.worker_queue_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "fanout_ring_capacity") == 0) {
      g_sfu_config.fanout_ring_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "fanout_job_pool_capacity") == 0) {
      g_sfu_config.fanout_job_pool_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "release_queue_capacity") == 0) {
      g_sfu_config.release_queue_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "video_pool_percent") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_video_pool_percent = (uint32_t)atoi(val);
    } else if (strcmp(key, "source_admission_bps") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_source_admission_bps = (uint32_t)atoi(val);
    } else if (strcmp(key, "screen_preferred_bps") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_screen_preferred_bps = (uint32_t)atoi(val);
    } else if (strcmp(key, "screen_mid_bps") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_screen_mid_bps = (uint32_t)atoi(val);
    } else if (strcmp(key, "screen_cap_bps") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_screen_cap_bps = (uint32_t)atoi(val);
    } else if (strcmp(key, "camera_mid_bps") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_camera_mid_bps = (uint32_t)atoi(val);
    } else if (strcmp(key, "camera_cap_bps") == 0 && strcmp(section, "bandwidth") == 0) {
      g_sfu_config.bandwidth_camera_cap_bps = (uint32_t)atoi(val);
    } else if (strcmp(key, "interface") == 0 && strcmp(section, "af_xdp") == 0) {
      snprintf(g_sfu_config.af_xdp_interface, sizeof(g_sfu_config.af_xdp_interface), "%s", val);
    } else if (strcmp(key, "queues") == 0 && strcmp(section, "af_xdp") == 0) {
      snprintf(g_sfu_config.af_xdp_queues, sizeof(g_sfu_config.af_xdp_queues), "%s", val);
      g_sfu_config.af_xdp_queues_set = true;
    } else if (strcmp(key, "queue_id") == 0 && strcmp(section, "af_xdp") == 0) {
      g_sfu_config.af_xdp_queue_id = (uint32_t)atoi(val);
      g_sfu_config.af_xdp_queue_id_set = true;
    } else if (strcmp(key, "frame_count") == 0 && strcmp(section, "af_xdp") == 0) {
      g_sfu_config.af_xdp_frame_count = (uint32_t)atoi(val);
    } else if (strcmp(key, "frame_size") == 0 && strcmp(section, "af_xdp") == 0) {
      g_sfu_config.af_xdp_frame_size = (uint32_t)atoi(val);
    } else if (strcmp(key, "mode") == 0 && strcmp(section, "af_xdp") == 0) {
      snprintf(g_sfu_config.af_xdp_mode, sizeof(g_sfu_config.af_xdp_mode), "%s", val);
    }
  }

  fclose(fp);
  SFU_LOG_INFO("successfully loaded runtime config from '%s'", filepath);
  return 0;
}
