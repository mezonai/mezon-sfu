#include "net/af_xdp_frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/if_ether.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERATIONS 1000000ull
#define DEFAULT_WARMUP 10000ull
#define DEFAULT_PACKET_SIZE 1200u
#define QUICK_ITERATIONS 1000ull
#define QUICK_WARMUP 100ull
#define FRAME_CAPACITY 65536u
#define MEDIA_PORT 7000u

typedef enum {
  BENCH_ALL = 0,
  BENCH_PARSE_IPV4_UDP,
  BENCH_PARSE_VLAN_UDP,
  BENCH_BUILD_IPV4_UDP,
  BENCH_CHECKSUM_IPV4,
} bench_kind_t;

typedef struct {
  bench_kind_t kind;
  uint64_t iterations;
  uint64_t warmup;
  uint32_t packet_size;
  bool csv;
} bench_config_t;

static volatile uint64_t g_sink;

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    perror("clock_gettime");
    exit(1);
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool parse_u64(const char *text, uint64_t *value) {
  char *end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (errno || end == text || *end != '\0') {
    return false;
  }
  *value = parsed;
  return true;
}

static bool parse_kind(const char *text, bench_kind_t *kind) {
  if (strcmp(text, "all") == 0) {
    *kind = BENCH_ALL;
  } else if (strcmp(text, "parse_ipv4_udp") == 0) {
    *kind = BENCH_PARSE_IPV4_UDP;
  } else if (strcmp(text, "parse_vlan_udp") == 0) {
    *kind = BENCH_PARSE_VLAN_UDP;
  } else if (strcmp(text, "build_ipv4_udp") == 0) {
    *kind = BENCH_BUILD_IPV4_UDP;
  } else if (strcmp(text, "checksum_ipv4") == 0) {
    *kind = BENCH_CHECKSUM_IPV4;
  } else {
    return false;
  }
  return true;
}

static void usage(const char *program) {
  fprintf(stderr,
          "usage: %s [all|parse_ipv4_udp|parse_vlan_udp|build_ipv4_udp|checksum_ipv4] [options]\n"
          "  --iterations N\n"
          "  --warmup N\n"
          "  --packet-size N\n"
          "  --csv\n"
          "  --quick\n",
          program);
}

static bool parse_args(int argc, char **argv, bench_config_t *config) {
  *config = (bench_config_t){.kind = BENCH_ALL, .iterations = DEFAULT_ITERATIONS, .warmup = DEFAULT_WARMUP, .packet_size = DEFAULT_PACKET_SIZE};
  int i = 1;
  if (i < argc && argv[i][0] != '-') {
    if (!parse_kind(argv[i++], &config->kind)) {
      return false;
    }
  }
  while (i < argc) {
    const char *arg = argv[i++];
    uint64_t value;
    if (strcmp(arg, "--quick") == 0) {
      config->iterations = QUICK_ITERATIONS;
      config->warmup = QUICK_WARMUP;
    } else if (strcmp(arg, "--csv") == 0) {
      config->csv = true;
    } else if (strcmp(arg, "--help") == 0) {
      usage(argv[0]);
      exit(0);
    } else if (strcmp(arg, "--iterations") == 0 && i < argc && parse_u64(argv[i++], &value)) {
      config->iterations = value;
    } else if (strcmp(arg, "--warmup") == 0 && i < argc && parse_u64(argv[i++], &value)) {
      config->warmup = value;
    } else if (strcmp(arg, "--packet-size") == 0 && i < argc && parse_u64(argv[i++], &value) && value <= UINT32_MAX) {
      config->packet_size = (uint32_t)value;
    } else {
      return false;
    }
  }
  return config->iterations > 0 && config->packet_size > 0 && config->packet_size <= UINT16_MAX - SFU_AF_XDP_IPV4_HEADER_SIZE - SFU_AF_XDP_UDP_HEADER_SIZE;
}

static sfu_af_xdp_frame_params_t make_params(const uint8_t *payload, uint32_t payload_len) {
  sfu_af_xdp_frame_params_t params = {
      .source_mac = {0x02, 0, 0, 0, 0, 1},
      .destination_mac = {0x02, 0, 0, 0, 0, 2},
      .source_port = htons(50000),
      .destination_port = htons(MEDIA_PORT),
      .payload = payload,
      .payload_len = payload_len,
  };
  if (inet_pton(AF_INET, "192.0.2.10", &params.source_ip) != 1 || inet_pton(AF_INET, "198.51.100.20", &params.destination_ip) != 1) {
    abort();
  }
  return params;
}

static void print_result(const char *name, const bench_config_t *config, uint64_t total_ns) {
  double ns_per_op = (double)total_ns / (double)config->iterations;
  double ops_per_sec = (double)config->iterations * 1000000000.0 / (double)total_ns;
  if (config->csv) {
    printf("%s,%" PRIu64 ",%u,%" PRIu64 ",%.2f,%.2f\n", name, config->iterations, config->packet_size, total_ns, ns_per_op, ops_per_sec);
  } else {
    printf("benchmark=%s iterations=%" PRIu64 " packet_size=%u total_ns=%" PRIu64 " ns_per_op=%.2f ops_per_sec=%.2f\n", name, config->iterations,
           config->packet_size, total_ns, ns_per_op, ops_per_sec);
  }
}

static uint32_t make_vlan_frame(uint8_t *vlan, const uint8_t *plain, uint32_t plain_len) {
  memcpy(vlan, plain, 12);
  uint16_t protocol = htons(ETH_P_8021Q);
  uint16_t tci = htons(1);
  uint16_t encapsulated = htons(ETH_P_IP);
  memcpy(vlan + 12, &protocol, 2);
  memcpy(vlan + 14, &tci, 2);
  memcpy(vlan + 16, &encapsulated, 2);
  memcpy(vlan + 18, plain + 14, plain_len - 14);
  return plain_len + 4;
}

static void run_parse(const bench_config_t *config, const char *name, uint8_t *frame, uint32_t frame_len) {
  sfu_af_xdp_parse_result_t result;
  for (uint64_t i = 0; i < config->warmup; i++) {
    if (!sfu_af_xdp_parse_frame(frame, frame_len, FRAME_CAPACITY, MEDIA_PORT, &result)) {
      abort();
    }
    g_sink += result.payload_len;
  }
  uint64_t start = now_ns();
  for (uint64_t i = 0; i < config->iterations; i++) {
    if (!sfu_af_xdp_parse_frame(frame, frame_len, FRAME_CAPACITY, MEDIA_PORT, &result)) {
      abort();
    }
    g_sink += result.payload[0];
  }
  print_result(name, config, now_ns() - start);
}

static void run_build(const bench_config_t *config, uint8_t *frame, const sfu_af_xdp_frame_params_t *params) {
  uint32_t frame_len;
  for (uint64_t i = 0; i < config->warmup; i++) {
    if (!sfu_af_xdp_build_frame(frame, FRAME_CAPACITY, params, &frame_len)) {
      abort();
    }
    g_sink += frame_len;
  }
  uint64_t start = now_ns();
  for (uint64_t i = 0; i < config->iterations; i++) {
    if (!sfu_af_xdp_build_frame(frame, FRAME_CAPACITY, params, &frame_len)) {
      abort();
    }
    g_sink += frame[frame_len - 1];
  }
  print_result("build_ipv4_udp", config, now_ns() - start);
}

static void run_checksum(const bench_config_t *config, const uint8_t *frame) {
  const uint8_t *ip = frame + SFU_AF_XDP_ETH_HEADER_SIZE;
  for (uint64_t i = 0; i < config->warmup; i++) {
    g_sink += sfu_af_xdp_checksum(ip, SFU_AF_XDP_IPV4_HEADER_SIZE);
  }
  uint64_t start = now_ns();
  for (uint64_t i = 0; i < config->iterations; i++) {
    g_sink += sfu_af_xdp_checksum(ip, SFU_AF_XDP_IPV4_HEADER_SIZE);
  }
  print_result("checksum_ipv4", config, now_ns() - start);
}

int main(int argc, char **argv) {
  bench_config_t config;
  if (!parse_args(argc, argv, &config)) {
    usage(argv[0]);
    return 2;
  }
  if (config.csv) {
    printf("benchmark,iterations,packet_size,total_ns,ns_per_op,ops_per_sec\n");
  }

  uint8_t *payload = malloc(config.packet_size);
  uint8_t *plain = malloc(FRAME_CAPACITY);
  uint8_t *vlan = malloc(FRAME_CAPACITY);
  if (!payload || !plain || !vlan) {
    perror("malloc");
    return 1;
  }
  memset(payload, 0xab, config.packet_size);
  sfu_af_xdp_frame_params_t params = make_params(payload, config.packet_size);
  uint32_t plain_len;
  if (!sfu_af_xdp_build_frame(plain, FRAME_CAPACITY, &params, &plain_len)) {
    abort();
  }
  uint32_t vlan_len = make_vlan_frame(vlan, plain, plain_len);

  if (config.kind == BENCH_ALL || config.kind == BENCH_PARSE_IPV4_UDP) {
    run_parse(&config, "parse_ipv4_udp", plain, plain_len);
  }
  if (config.kind == BENCH_ALL || config.kind == BENCH_PARSE_VLAN_UDP) {
    run_parse(&config, "parse_vlan_udp", vlan, vlan_len);
  }
  if (config.kind == BENCH_ALL || config.kind == BENCH_BUILD_IPV4_UDP) {
    run_build(&config, plain, &params);
  }
  if (config.kind == BENCH_ALL || config.kind == BENCH_CHECKSUM_IPV4) {
    run_checksum(&config, plain);
  }

  free(vlan);
  free(plain);
  free(payload);
  return 0;
}
