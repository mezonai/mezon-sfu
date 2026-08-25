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

uint16_t sfu_af_xdp_checksum(const void *data, size_t len) {
  if (!data && len != 0) {
    return 0;
  }

  const uint8_t *bytes = data;
  uint32_t sum = 0;
  while (len >= 2) {
    sum += ((uint32_t)bytes[0] << 8) | bytes[1];
    bytes += 2;
    len -= 2;
  }
  if (len != 0) {
    sum += (uint32_t)bytes[0] << 8;
  }
  while (sum >> 16) {
    sum = (sum & 0xffffu) + (sum >> 16);
  }
  return htons((uint16_t)~sum);
}

bool sfu_af_xdp_parse_frame(uint8_t *frame, uint32_t frame_len, uint32_t frame_capacity, uint16_t media_port,
                            sfu_af_xdp_parse_result_t *result) {
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
  if (ip.version != 4 || ihl < sizeof(ip) || ip_total_len < ihl + sizeof(struct udphdr) || frame_len < offset + ihl ||
      ip_total_len > frame_len - offset || ip.protocol != IPPROTO_UDP || (ntohs(ip.frag_off) & SFU_AF_XDP_IPV4_FRAGMENT_MASK)) {
    return false;
  }

  struct udphdr udp;
  memcpy(&udp, frame + offset + ihl, sizeof(udp));
  uint32_t udp_len = ntohs(udp.len);
  if (udp_len <= sizeof(udp) || udp_len > ip_total_len - ihl || offset + ihl + udp_len > frame_len || ntohs(udp.dest) != media_port) {
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
  if (params->payload_len == 0 || params->payload_len > UINT16_MAX - sizeof(struct iphdr) - sizeof(struct udphdr) ||
      params->payload_len > frame_capacity || SFU_AF_XDP_FRAME_HEADER_SIZE > frame_capacity - params->payload_len) {
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
  udp.len = htons((uint16_t)(sizeof(udp) + params->payload_len));
  memcpy(frame + sizeof(eth) + sizeof(ip), &udp, sizeof(udp));
  memcpy(frame + SFU_AF_XDP_FRAME_HEADER_SIZE, params->payload, params->payload_len);

  *out_len = SFU_AF_XDP_FRAME_HEADER_SIZE + params->payload_len;
  return true;
}
