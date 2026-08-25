#ifndef SFU_NET_AF_XDP_FRAME_H
#define SFU_NET_AF_XDP_FRAME_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_AF_XDP_ETH_HEADER_SIZE 14u
#define SFU_AF_XDP_VLAN_HEADER_SIZE 4u
#define SFU_AF_XDP_IPV4_HEADER_SIZE 20u
#define SFU_AF_XDP_UDP_HEADER_SIZE 8u
#define SFU_AF_XDP_FRAME_HEADER_SIZE (SFU_AF_XDP_ETH_HEADER_SIZE + SFU_AF_XDP_IPV4_HEADER_SIZE + SFU_AF_XDP_UDP_HEADER_SIZE)

typedef struct sfu_af_xdp_parse_result {
  uint8_t *payload;
  uint32_t payload_len;
  uint32_t payload_cap;
  struct in_addr source_ip;
  uint16_t source_port;
  uint32_t header_len;
  bool vlan;
} sfu_af_xdp_parse_result_t;

typedef struct sfu_af_xdp_frame_params {
  uint8_t source_mac[6];
  uint8_t destination_mac[6];
  struct in_addr source_ip;
  struct in_addr destination_ip;
  uint16_t source_port;
  uint16_t destination_port;
  const uint8_t *payload;
  uint32_t payload_len;
} sfu_af_xdp_frame_params_t;

uint16_t sfu_af_xdp_checksum(const void *data, size_t len);
bool sfu_af_xdp_partition_frames(uint32_t total, uint32_t *rx, uint32_t *tx);
bool sfu_af_xdp_parse_frame(uint8_t *frame, uint32_t frame_len, uint32_t frame_capacity, uint16_t media_port,
                            sfu_af_xdp_parse_result_t *result);
bool sfu_af_xdp_build_frame(uint8_t *frame, uint32_t frame_capacity, const sfu_af_xdp_frame_params_t *params, uint32_t *out_len);
uint32_t sfu_af_xdp_frames_per_queue(uint32_t total_frames, uint32_t queue_count);
bool sfu_af_xdp_encode_rx_return(uint32_t queue_slot, uint32_t frame, uintptr_t *token);
bool sfu_af_xdp_decode_rx_return(uintptr_t token, uint32_t *queue_slot, uint32_t *frame);

#endif /* SFU_NET_AF_XDP_FRAME_H */
