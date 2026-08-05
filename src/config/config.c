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

  snprintf(g_sfu_config.nats_url, sizeof(g_sfu_config.nats_url), "nats://172.16.100.183:4222");
  snprintf(g_sfu_config.nats_client_name, sizeof(g_sfu_config.nats_client_name), "sfu_nats_client");

  g_sfu_config.packet_buf_size = 1600;
  g_sfu_config.packet_pool_capacity = 65536;
  g_sfu_config.provided_buf_count = 8192;
  g_sfu_config.provided_buf_group_id = 0;

  g_sfu_config.worker_queue_capacity = 16384;
  g_sfu_config.fanout_ring_capacity = 4096;
  g_sfu_config.fanout_job_pool_capacity = 16384;
  g_sfu_config.release_queue_capacity = 8192;
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
    }
  }

  fclose(fp);
  SFU_LOG_INFO("successfully loaded runtime config from '%s'", filepath);
  return 0;
}
