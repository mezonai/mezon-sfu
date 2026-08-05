#include "config/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/log.h"

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

void sfu_config_set_defaults(sfu_config_t *cfg) {
  if (!cfg) {
    return;
  }
  memset(cfg, 0, sizeof(*cfg));

  cfg->media_port = 7000;
  cfg->signaling_port = 8000;
  snprintf(cfg->public_host, sizeof(cfg->public_host), "127.0.0.1");

  snprintf(cfg->nats_url, sizeof(cfg->nats_url), "nats://172.16.100.183:4222");
  snprintf(cfg->nats_client_name, sizeof(cfg->nats_client_name), "sfu_nats_client");

  cfg->packet_buf_size = 1600;
  cfg->packet_pool_capacity = 65536;
  cfg->provided_buf_count = 8192;
  cfg->provided_buf_group_id = 0;

  cfg->worker_queue_capacity = 16384;
  cfg->fanout_ring_capacity = 4096;
  cfg->fanout_job_pool_capacity = 16384;
  cfg->release_queue_capacity = 8192;
}

int sfu_config_load_ini(sfu_config_t *cfg, const char *filepath) {
  sfu_config_set_defaults(cfg);

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

    if (strcmp(key, "media_port") == 0) {
      cfg->media_port = (uint16_t)atoi(val);
    } else if (strcmp(key, "signaling_port") == 0) {
      cfg->signaling_port = (uint16_t)atoi(val);
    } else if (strcmp(key, "public_host") == 0) {
      snprintf(cfg->public_host, sizeof(cfg->public_host), "%s", val);
    } else if (strcmp(key, "nats_url") == 0) {
      snprintf(cfg->nats_url, sizeof(cfg->nats_url), "%s", val);
    } else if (strcmp(key, "nats_client_name") == 0) {
      snprintf(cfg->nats_client_name, sizeof(cfg->nats_client_name), "%s", val);
    } else if (strcmp(key, "packet_buf_size") == 0) {
      cfg->packet_buf_size = (uint32_t)atoi(val);
    } else if (strcmp(key, "packet_pool_capacity") == 0) {
      cfg->packet_pool_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "provided_buf_count") == 0) {
      cfg->provided_buf_count = (uint32_t)atoi(val);
    } else if (strcmp(key, "provided_buf_group_id") == 0) {
      cfg->provided_buf_group_id = atoi(val);
    } else if (strcmp(key, "worker_queue_capacity") == 0) {
      cfg->worker_queue_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "fanout_ring_capacity") == 0) {
      cfg->fanout_ring_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "fanout_job_pool_capacity") == 0) {
      cfg->fanout_job_pool_capacity = (uint32_t)atoi(val);
    } else if (strcmp(key, "release_queue_capacity") == 0) {
      cfg->release_queue_capacity = (uint32_t)atoi(val);
    }
  }

  fclose(fp);
  SFU_LOG_INFO("successfully loaded runtime config from '%s'", filepath);
  return 0;
}
