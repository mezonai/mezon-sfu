#include "net/af_xdp_frame.h"

#include <arpa/inet.h>
#include <limits.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <string.h>

#define SFU_AF_XDP_IPV4_FRAGMENT_MASK 0x3fffu

typedef struct {
  uint16_t tci;
  uint16_t encapsulated_proto;
} sfu_vlan_header_t;

typedef struct {
  uint32_t sum;
  uint8_t high_byte;
  bool odd;
} sfu_checksum_state_t;

static void checksum_add(sfu_checksum_state_t *state, const void *data, size_t len) {
  const uint8_t *bytes = data;
  if (state->odd && len != 0) {
    state->sum += ((uint32_t)state->high_byte << 8) | *bytes++;
    len--;
    state->odd = false;
  }
  while (len >= 2) {
    state->sum += ((uint32_t)bytes[0] << 8) | bytes[1];
    bytes += 2;
    len -= 2;
  }
  if (len != 0) {
    state->high_byte = bytes[0];
    state->odd = true;
  }
}

static uint16_t checksum_finish(sfu_checksum_state_t *state) {
  if (state->odd) {
    state->sum += (uint32_t)state->high_byte << 8;
  }
  while (state->sum >> 16) {
    state->sum = (state->sum & 0xffffu) + (state->sum >> 16);
  }
  return htons((uint16_t)~state->sum);
}

static uint16_t udp_checksum(const struct iphdr *ip, const void *udp, uint16_t udp_len) {
  sfu_checksum_state_t state = {0};
  uint8_t protocol[2] = {0, IPPROTO_UDP};
  uint16_t network_len = htons(udp_len);
  checksum_add(&state, &ip->saddr, sizeof(ip->saddr));
  checksum_add(&state, &ip->daddr, sizeof(ip->daddr));
  checksum_add(&state, protocol, sizeof(protocol));
  checksum_add(&state, &network_len, sizeof(network_len));
  checksum_add(&state, udp, udp_len);
  return checksum_finish(&state);
}

uint16_t sfu_af_xdp_checksum(const void *data, size_t len) {
  if (!data && len != 0) {
    return 0;
  }
  sfu_checksum_state_t state = {0};
  checksum_add(&state, data, len);
  return checksum_finish(&state);
}

bool sfu_af_xdp_partition_frames(uint32_t total, uint32_t *rx, uint32_t *tx) {
  if (!rx || !tx || total < 8u || (total & (total - 1u)) != 0) {
    return false;
  }
  *rx = total / 2u;
  *tx = total / 2u;
  return true;
}

bool sfu_af_xdp_parse_frame(uint8_t *frame, uint32_t frame_len, uint32_t frame_capacity, uint16_t media_port, sfu_af_xdp_parse_result_t *result) {
  if (!frame || !result || frame_len > frame_capacity || frame_len < sizeof(struct ethhdr)) {
    return false;
  }

  memset(result, 0, sizeof(*result));
  struct ethhdr eth;
  memcpy(&eth, frame, sizeof(eth));
  uint16_t protocol = ntohs(eth.h_proto);
  uint32_t offset = sizeof(eth);

  if (protocol == ETH_P_8021Q || protocol == ETH_P_8021AD) {
    if (frame_len < offset + sizeof(sfu_vlan_header_t)) {
      return false;
    }
    sfu_vlan_header_t vlan;
    memcpy(&vlan, frame + offset, sizeof(vlan));
    protocol = ntohs(vlan.encapsulated_proto);
    offset += sizeof(vlan);
    result->vlan = true;
  }

  if (protocol != ETH_P_IP || frame_len < offset + sizeof(struct iphdr)) {
    return false;
  }

  struct iphdr ip;
  memcpy(&ip, frame + offset, sizeof(ip));
  uint32_t ihl = (uint32_t)ip.ihl * 4u;
  uint32_t ip_total_len = ntohs(ip.tot_len);
  if (ip.version != 4 || ihl < sizeof(ip) || ip_total_len < ihl + sizeof(struct udphdr) || frame_len < offset + ihl || ip_total_len > frame_len - offset ||
      ip.protocol != IPPROTO_UDP || (ntohs(ip.frag_off) & SFU_AF_XDP_IPV4_FRAGMENT_MASK) || sfu_af_xdp_checksum(frame + offset, ihl) != 0) {
    return false;
  }

  struct udphdr udp;
  memcpy(&udp, frame + offset + ihl, sizeof(udp));
  uint32_t udp_len = ntohs(udp.len);
  if (udp_len <= sizeof(udp) || udp_len > ip_total_len - ihl || offset + ihl + udp_len > frame_len || ntohs(udp.dest) != media_port ||
      (udp.check != 0 && udp_checksum(&ip, frame + offset + ihl, (uint16_t)udp_len) != 0)) {
    return false;
  }

  uint32_t header_len = offset + ihl + sizeof(udp);
  result->payload = frame + header_len;
  result->payload_len = udp_len - sizeof(udp);
  result->payload_cap = frame_capacity - header_len;
  result->source_ip.s_addr = ip.saddr;
  result->source_port = udp.source;
  result->header_len = header_len;
  return true;
}

bool sfu_af_xdp_build_frame(uint8_t *frame, uint32_t frame_capacity, const sfu_af_xdp_frame_params_t *params, uint32_t *out_len) {
  if (!frame || !params || !out_len || (!params->payload && params->payload_len != 0)) {
    return false;
  }
  if (params->payload_len == 0 || params->payload_len > UINT16_MAX - sizeof(struct iphdr) - sizeof(struct udphdr) || params->payload_len > frame_capacity ||
      SFU_AF_XDP_FRAME_HEADER_SIZE > frame_capacity - params->payload_len) {
    return false;
  }

  struct ethhdr eth;
  memcpy(eth.h_dest, params->destination_mac, ETH_ALEN);
  memcpy(eth.h_source, params->source_mac, ETH_ALEN);
  eth.h_proto = htons(ETH_P_IP);
  memcpy(frame, &eth, sizeof(eth));

  struct iphdr ip;
  memset(&ip, 0, sizeof(ip));
  ip.version = 4;
  ip.ihl = 5;
  ip.ttl = 64;
  ip.protocol = IPPROTO_UDP;
  ip.tot_len = htons((uint16_t)(sizeof(ip) + sizeof(struct udphdr) + params->payload_len));
  ip.saddr = params->source_ip.s_addr;
  ip.daddr = params->destination_ip.s_addr;
  ip.check = sfu_af_xdp_checksum(&ip, sizeof(ip));
  memcpy(frame + sizeof(eth), &ip, sizeof(ip));

  struct udphdr udp;
  memset(&udp, 0, sizeof(udp));
  udp.source = params->source_port;
  udp.dest = params->destination_port;
  uint16_t udp_len = (uint16_t)(sizeof(udp) + params->payload_len);
  udp.len = htons(udp_len);
  memcpy(frame + sizeof(eth) + sizeof(ip), &udp, sizeof(udp));
  memcpy(frame + SFU_AF_XDP_FRAME_HEADER_SIZE, params->payload, params->payload_len);
  udp.check = udp_checksum(&ip, frame + sizeof(eth) + sizeof(ip), udp_len);
  if (udp.check == 0) {
    udp.check = htons(0xffffu);
  }
  memcpy(frame + sizeof(eth) + sizeof(ip), &udp, sizeof(udp));

  *out_len = SFU_AF_XDP_FRAME_HEADER_SIZE + params->payload_len;
  return true;
}
