#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>

struct sfu_vlan_hdr {
  __be16 tci;
  __be16 encapsulated_proto;
};

struct {
  __uint(type, BPF_MAP_TYPE_XSKMAP);
  __uint(max_entries, 128);
  __type(key, __u32);
  __type(value, __u32);
} xsks_map SEC(".maps");

struct sfu_xdp_config {
  __u16 media_port;
  __u16 reserved;
  __u32 queue_id;
};

struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct sfu_xdp_config);
} config_map SEC(".maps");

SEC("xdp")
int sfu_xdp_redirect(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;
  struct ethhdr *eth = data;
  __u16 protocol;
  __u64 offset = sizeof(*eth);

  if ((void *)(eth + 1) > data_end) {
    return XDP_PASS;
  }
  protocol = eth->h_proto;

  if (protocol == bpf_htons(ETH_P_8021Q) || protocol == bpf_htons(ETH_P_8021AD)) {
    struct sfu_vlan_hdr *vlan = data + offset;
    if ((void *)(vlan + 1) > data_end) {
      return XDP_PASS;
    }
    protocol = vlan->encapsulated_proto;
    offset += sizeof(*vlan);
  }

  if (protocol != bpf_htons(ETH_P_IP)) {
    return XDP_PASS;
  }

  struct iphdr *ip = data + offset;
  if ((void *)(ip + 1) > data_end || ip->version != 4 || ip->ihl < 5) {
    return XDP_PASS;
  }
  if ((void *)ip + ip->ihl * 4 > data_end || ip->protocol != IPPROTO_UDP) {
    return XDP_PASS;
  }
  if (ip->frag_off & bpf_htons(0x3fff)) {
    return XDP_PASS;
  }

  struct udphdr *udp = (void *)ip + ip->ihl * 4;
  if ((void *)(udp + 1) > data_end) {
    return XDP_PASS;
  }

  __u32 zero = 0;
  struct sfu_xdp_config *config = bpf_map_lookup_elem(&config_map, &zero);
  if (!config || udp->dest != config->media_port) {
    return XDP_PASS;
  }
  if (ctx->rx_queue_index != config->queue_id) {
    return XDP_PASS;
  }

  return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_DROP);
}

char LICENSE[] SEC("license") = "GPL";
