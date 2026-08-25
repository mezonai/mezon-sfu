#include "net/io_backend.h"

#ifdef USE_AF_XDP

#include "memory/refcount.h"
#include "memory/worker_packet_arena.h"
#include "net/af_xdp_frame.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/metrics.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <xdp/xsk.h>

#ifndef SFU_XDP_OBJECT_PATH
#define SFU_XDP_OBJECT_PATH "sfu_xdp_kern.o"
#endif

#define SFU_AF_XDP_TX_QUEUE_CAPACITY 4096u
#define SFU_AF_XDP_BATCH 256u

typedef enum {
  SFU_XDP_FRAME_RX_FREE = 0,
  SFU_XDP_FRAME_RX_FILL,
  SFU_XDP_FRAME_RX_APP,
  SFU_XDP_FRAME_TX_FREE,
  SFU_XDP_FRAME_TX_KERNEL,
  SFU_XDP_FRAME_TX_COMPLETE_WAIT,
} sfu_xdp_frame_state_t;

typedef struct {
  sfu_packet_t *packet;
  sfu_ring_t *origin;
  uint8_t state;
} sfu_xdp_frame_meta_t;

typedef struct {
  uint16_t media_port;
  uint16_t reserved;
  uint32_t queue_id;
} sfu_xdp_bpf_config_t;

typedef struct {
  struct xsk_umem *umem;
  struct xsk_socket *xsk;
  struct xsk_ring_prod fill;
  struct xsk_ring_cons completion;
  struct xsk_ring_cons rx;
  struct xsk_ring_prod tx;
  struct bpf_object *bpf_object;
  void *umem_area;
  uint64_t *rx_free;
  uint64_t *tx_free;
  sfu_xdp_frame_meta_t *frames;
  uint32_t rx_free_count;
  uint32_t tx_free_count;
  uint32_t frame_count;
  uint32_t frame_size;
  uint32_t rx_frame_count;
  uint32_t tx_frame_count;
  uint32_t queue_id;
  uint32_t xdp_flags;
  uint32_t xdp_mode_flags;
  uint32_t xdp_program_id;
  int socket_fd;
  int ifindex;
  int xsks_map_fd;
  bool xdp_attached;
  bool initialized;
  char interface_name[IFNAMSIZ];
  uint8_t local_mac[ETH_ALEN];
  struct in_addr local_ip;
  struct in_addr netmask;
  struct in_addr gateway;
  uint16_t media_port;
} sfu_xdp_device_t;

static sfu_xdp_device_t g_xdp;

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool interface_queue_exists(const char *interface_name, uint32_t queue_id) {
  char path[256];
  int written = snprintf(path, sizeof(path), "/sys/class/net/%s/queues/rx-%u", interface_name, queue_id);
  return written > 0 && (size_t)written < sizeof(path) && access(path, F_OK) == 0;
}

static int read_interface_addresses(sfu_xdp_device_t *d) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", d->interface_name);
  if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
    close(fd);
    return -1;
  }
  memcpy(d->local_mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
  if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
    close(fd);
    return -1;
  }
  d->local_ip = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
  if (ioctl(fd, SIOCGIFNETMASK, &ifr) != 0) {
    close(fd);
    return -1;
  }
  d->netmask = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr;
  close(fd);
  return 0;
}

static void read_default_gateway(sfu_xdp_device_t *d) {
  FILE *fp = fopen("/proc/net/route", "r");
  if (!fp) {
    return;
  }
  char line[256];
  if (!fgets(line, sizeof(line), fp)) {
    fclose(fp);
    return;
  }
  while (fgets(line, sizeof(line), fp)) {
    char iface[IFNAMSIZ];
    unsigned long destination, gateway;
    unsigned flags;
    if (sscanf(line, "%15s %lx %lx %x", iface, &destination, &gateway, &flags) == 4 && strcmp(iface, d->interface_name) == 0 && destination == 0 &&
        (flags & 0x2u)) {
      d->gateway.s_addr = (in_addr_t)gateway;
      break;
    }
  }
  fclose(fp);
}

static int lookup_neighbor(const struct in_addr *destination, uint8_t mac[ETH_ALEN]) {
  struct in_addr next_hop = *destination;
  if ((destination->s_addr & g_xdp.netmask.s_addr) != (g_xdp.local_ip.s_addr & g_xdp.netmask.s_addr) && g_xdp.gateway.s_addr != 0) {
    next_hop = g_xdp.gateway;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct arpreq req;
  memset(&req, 0, sizeof(req));
  struct sockaddr_in *sin = (struct sockaddr_in *)&req.arp_pa;
  sin->sin_family = AF_INET;
  sin->sin_addr = next_hop;
  snprintf(req.arp_dev, sizeof(req.arp_dev), "%s", g_xdp.interface_name);
  int rc = ioctl(fd, SIOCGARP, &req);
  close(fd);
  if (rc != 0 || !(req.arp_flags & ATF_COM)) {
    return -1;
  }
  memcpy(mac, req.arp_ha.sa_data, ETH_ALEN);
  return 0;
}

static int attach_xdp_program(sfu_xdp_device_t *d, const char *mode) {
  d->bpf_object = bpf_object__open_file(SFU_XDP_OBJECT_PATH, NULL);
  if (libbpf_get_error(d->bpf_object)) {
    d->bpf_object = NULL;
    return -1;
  }
  if (bpf_object__load(d->bpf_object) != 0) {
    return -1;
  }
  struct bpf_program *program = bpf_object__find_program_by_name(d->bpf_object, "sfu_xdp_redirect");
  struct bpf_map *xsks = bpf_object__find_map_by_name(d->bpf_object, "xsks_map");
  struct bpf_map *config = bpf_object__find_map_by_name(d->bpf_object, "config_map");
  if (!program || !xsks || !config) {
    return -1;
  }

  int program_fd = bpf_program__fd(program);
  struct bpf_prog_info program_info;
  uint32_t program_info_len = sizeof(program_info);
  memset(&program_info, 0, sizeof(program_info));
  if (bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_len) != 0 || program_info.id == 0) {
    return -1;
  }

  uint32_t flags = XDP_FLAGS_UPDATE_IF_NOEXIST;
  if (strcmp(mode, "skb") == 0) {
    flags |= XDP_FLAGS_SKB_MODE;
  } else {
    flags |= XDP_FLAGS_DRV_MODE;
  }
  int rc = bpf_xdp_attach(d->ifindex, program_fd, flags, NULL);
  if (rc != 0 && strcmp(mode, "auto") == 0) {
    flags = XDP_FLAGS_UPDATE_IF_NOEXIST | XDP_FLAGS_SKB_MODE;
    rc = bpf_xdp_attach(d->ifindex, bpf_program__fd(program), flags, NULL);
  }
  if (rc != 0) {
    return -1;
  }
  d->xdp_flags = flags;
  d->xdp_mode_flags = flags & (XDP_FLAGS_SKB_MODE | XDP_FLAGS_DRV_MODE);
  d->xdp_program_id = program_info.id;
  d->xdp_attached = true;
  uint32_t attached_program_id = 0;
  if (bpf_xdp_query_id(d->ifindex, d->xdp_mode_flags, &attached_program_id) != 0 || attached_program_id != d->xdp_program_id) {
    return -1;
  }
  d->xsks_map_fd = bpf_map__fd(xsks);

  uint32_t key = 0;
  sfu_xdp_bpf_config_t value = {.media_port = htons(d->media_port), .queue_id = d->queue_id};
  if (bpf_map_update_elem(bpf_map__fd(config), &key, &value, BPF_ANY) != 0) {
    return -1;
  }
  return 0;
}

static void refill_rx(void) {
  while (g_xdp.rx_free_count > 0) {
    uint32_t index;
    if (xsk_ring_prod__reserve(&g_xdp.fill, 1, &index) != 1) {
      break;
    }
    uint64_t frame = g_xdp.rx_free[--g_xdp.rx_free_count];
    *xsk_ring_prod__fill_addr(&g_xdp.fill, index) = frame * g_xdp.frame_size;
    g_xdp.frames[frame].state = SFU_XDP_FRAME_RX_FILL;
    xsk_ring_prod__submit(&g_xdp.fill, 1);
  }
}

static void recycle_rx_frame(uint32_t frame) {
  if (frame >= g_xdp.rx_frame_count) {
    sfu_metric_inc("af_xdp_invalid_rx_frame");
    return;
  }
  g_xdp.frames[frame].state = SFU_XDP_FRAME_RX_FREE;
  g_xdp.rx_free[g_xdp.rx_free_count++] = frame;
}

int sfu_ring_backend_init(int fd, const char *interface_name, uint32_t queue_id, uint16_t media_port, uint32_t frame_count, uint32_t frame_size,
                          const char *xdp_mode) {
  memset(&g_xdp, 0, sizeof(g_xdp));
  g_xdp.xsks_map_fd = -1;
  if (!interface_name || !*interface_name || frame_count < 8 || frame_size < 2048) {
    return -1;
  }
  g_xdp.socket_fd = fd;
  g_xdp.queue_id = queue_id;
  g_xdp.media_port = media_port;
  g_xdp.frame_count = frame_count;
  g_xdp.frame_size = frame_size;
  if (!sfu_af_xdp_partition_frames(frame_count, &g_xdp.rx_frame_count, &g_xdp.tx_frame_count)) {
    SFU_LOG_ERROR("AF_XDP: frame_count must be a power of two >= 8 (got %u)", frame_count);
    goto fail;
  }
  snprintf(g_xdp.interface_name, sizeof(g_xdp.interface_name), "%s", interface_name);
  g_xdp.ifindex = if_nametoindex(interface_name);
  if (!g_xdp.ifindex || !interface_queue_exists(interface_name, queue_id) || read_interface_addresses(&g_xdp) != 0) {
    SFU_LOG_ERROR("AF_XDP: cannot resolve interface '%s' queue %u", interface_name, queue_id);
    goto fail;
  }
  read_default_gateway(&g_xdp);

  size_t umem_size = (size_t)frame_count * frame_size;
  if (posix_memalign(&g_xdp.umem_area, (size_t)getpagesize(), umem_size) != 0) {
    goto fail;
  }
  memset(g_xdp.umem_area, 0, umem_size);
  g_xdp.rx_free = SFU_CALLOC(g_xdp.rx_frame_count, sizeof(*g_xdp.rx_free));
  g_xdp.tx_free = SFU_CALLOC(g_xdp.tx_frame_count, sizeof(*g_xdp.tx_free));
  g_xdp.frames = SFU_CALLOC(frame_count, sizeof(*g_xdp.frames));
  if (!g_xdp.rx_free || !g_xdp.tx_free || !g_xdp.frames) {
    goto fail;
  }

  struct xsk_umem_config umem_config = {
      .fill_size = g_xdp.rx_frame_count,
      .comp_size = g_xdp.tx_frame_count,
      .frame_size = frame_size,
      .frame_headroom = 0,
      .flags = 0,
  };
  if (xsk_umem__create(&g_xdp.umem, g_xdp.umem_area, umem_size, &g_xdp.fill, &g_xdp.completion, &umem_config) != 0) {
    goto fail;
  }
  if (attach_xdp_program(&g_xdp, xdp_mode ? xdp_mode : "native") != 0) {
    SFU_LOG_ERROR("AF_XDP: failed to load or attach %s", SFU_XDP_OBJECT_PATH);
    goto fail;
  }

  struct xsk_socket_config socket_config = {
      .rx_size = g_xdp.rx_frame_count,
      .tx_size = g_xdp.tx_frame_count,
      .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
      .xdp_flags = g_xdp.xdp_flags & (XDP_FLAGS_SKB_MODE | XDP_FLAGS_DRV_MODE),
      .bind_flags = XDP_USE_NEED_WAKEUP,
  };
  if (xsk_socket__create(&g_xdp.xsk, interface_name, queue_id, g_xdp.umem, &g_xdp.rx, &g_xdp.tx, &socket_config) != 0) {
    SFU_LOG_ERROR("AF_XDP: xsk_socket__create failed on %s queue %u: %s", interface_name, queue_id, strerror(errno));
    goto fail;
  }
  int xsk_fd = xsk_socket__fd(g_xdp.xsk);
  if (bpf_map_update_elem(g_xdp.xsks_map_fd, &queue_id, &xsk_fd, BPF_ANY) != 0) {
    goto fail;
  }

  for (uint32_t i = 0; i < g_xdp.rx_frame_count; i++) {
    g_xdp.rx_free[g_xdp.rx_free_count++] = i;
    g_xdp.frames[i].state = SFU_XDP_FRAME_RX_FREE;
  }
  for (uint32_t i = g_xdp.rx_frame_count; i < frame_count; i++) {
    g_xdp.tx_free[g_xdp.tx_free_count++] = i;
    g_xdp.frames[i].state = SFU_XDP_FRAME_TX_FREE;
  }
  refill_rx();
  g_xdp.initialized = true;
  SFU_LOG_INFO("AF_XDP initialized on %s queue %u: %u frames x %u bytes (rx=%u tx=%u)", interface_name, queue_id, frame_count, frame_size,
               g_xdp.rx_frame_count, g_xdp.tx_frame_count);
  return 0;

fail:
  sfu_ring_backend_destroy();
  return -1;
}

void sfu_ring_backend_destroy(void) {
  if (g_xdp.xsks_map_fd >= 0) {
    (void)bpf_map_delete_elem(g_xdp.xsks_map_fd, &g_xdp.queue_id);
  }
  if (g_xdp.xsk) {
    xsk_socket__delete(g_xdp.xsk);
  }
  if (g_xdp.umem) {
    xsk_umem__delete(g_xdp.umem);
  }
  if (g_xdp.xdp_attached) {
    uint32_t current_program_id = 0;
    if (bpf_xdp_query_id(g_xdp.ifindex, g_xdp.xdp_mode_flags, &current_program_id) == 0 && current_program_id == g_xdp.xdp_program_id) {
      (void)bpf_xdp_detach(g_xdp.ifindex, g_xdp.xdp_mode_flags, NULL);
    } else if (current_program_id != 0) {
      SFU_LOG_WARN("AF_XDP: not detaching XDP program %u; owned program was %u", current_program_id, g_xdp.xdp_program_id);
    }
  }
  if (g_xdp.bpf_object) {
    bpf_object__close(g_xdp.bpf_object);
  }
  SFU_FREE(g_xdp.frames);
  SFU_FREE(g_xdp.tx_free);
  SFU_FREE(g_xdp.rx_free);
  free(g_xdp.umem_area);
  memset(&g_xdp, 0, sizeof(g_xdp));
  g_xdp.xsks_map_fd = -1;
}

uint32_t sfu_ring_recv_overhead(void) { return (uint32_t)(sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr)); }
uint32_t sfu_ring_recv_slot_size(uint32_t payload_cap) { return g_xdp.frame_size ? g_xdp.frame_size : payload_cap + sfu_ring_recv_overhead(); }

int sfu_ring_init(sfu_ring_t *r, int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t buf_count, uint32_t buf_size, int bgid, bool with_recv_bufs) {
  (void)cq_entries;
  (void)buf_count;
  (void)buf_size;
  (void)bgid;
  memset(r, 0, sizeof(*r));
  r->fd = fd;
  r->with_recv_bufs = with_recv_bufs;
  if (!with_recv_bufs) {
    uint32_t capacity = sq_entries;
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      capacity = SFU_AF_XDP_TX_QUEUE_CAPACITY;
    }
    if (sfu_spsc_ring_init(&r->tx_pending, capacity) != 0 || sfu_spsc_ring_init(&r->tx_completed, capacity) != 0) {
      sfu_spsc_ring_destroy(&r->tx_pending);
      return -1;
    }
    r->queue_capacity = capacity;
    r->queues_initialized = true;
  }
  return with_recv_bufs ? (g_xdp.initialized ? 0 : -1) : 0;
}

void sfu_ring_destroy(sfu_ring_t *r) {
  if (r && r->queues_initialized) {
    sfu_spsc_ring_destroy(&r->tx_completed);
    sfu_spsc_ring_destroy(&r->tx_pending);
    r->queues_initialized = false;
  }
}

int sfu_ring_arm_recv(sfu_ring_t *r) { return r && r->with_recv_bufs && g_xdp.initialized ? 0 : -1; }

int sfu_ring_queue_send_zc(sfu_ring_t *r, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len) {
  if (!r || !r->queues_initialized || !pkt || !dst || dst->sa_family != AF_INET || dst_len != sizeof(struct sockaddr_in)) {
    return -1;
  }
  memcpy(&pkt->peer_addr, dst, dst_len);
  pkt->peer_addr_len = dst_len;
  sfu_packet_retain(pkt, 1);
  if (!sfu_spsc_ring_push(&r->tx_pending, pkt)) {
    (void)sfu_packet_release(pkt);
    return -1;
  }
  r->outstanding_sends++;
  return 0;
}

int sfu_ring_submit(sfu_ring_t *r) {
  (void)r;
  return 0;
}

static bool parse_rx_packet(uint8_t *frame, uint32_t length, sfu_packet_t *pkt) {
  sfu_af_xdp_parse_result_t parsed;
  if (!sfu_af_xdp_parse_frame(frame, length, g_xdp.frame_size, g_xdp.media_port, &parsed)) {
    return false;
  }

  struct sockaddr_in *peer = (struct sockaddr_in *)&pkt->peer_addr;
  memset(peer, 0, sizeof(*peer));
  peer->sin_family = AF_INET;
  peer->sin_addr = parsed.source_ip;
  peer->sin_port = parsed.source_port;
  pkt->peer_addr_len = sizeof(*peer);
  pkt->data = parsed.payload;
  pkt->len = parsed.payload_len;
  pkt->cap = parsed.payload_cap;
  pkt->recv_ts_ns = monotonic_ns();
  return true;
}

static unsigned reap_rx(unsigned max_count, sfu_packet_pool_t *pp, sfu_on_recv_fn on_recv, void *user_data) {
  uint32_t index;
  unsigned count = xsk_ring_cons__peek(&g_xdp.rx, max_count, &index);
  for (unsigned i = 0; i < count; i++) {
    const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&g_xdp.rx, index + i);
    uint64_t address = xsk_umem__extract_addr(desc->addr);
    uint32_t frame_id = (uint32_t)(address / g_xdp.frame_size);
    uint8_t *frame = xsk_umem__get_data(g_xdp.umem_area, address);
    sfu_packet_t *pkt = NULL;
    if (frame_id < g_xdp.rx_frame_count && desc->len <= g_xdp.frame_size) {
      pkt = sfu_packet_pool_alloc_meta(pp);
    }
    if (!pkt || !parse_rx_packet(frame, desc->len, pkt)) {
      if (pkt) {
        sfu_packet_pool_free_meta(pp, pkt);
      }
      recycle_rx_frame(frame_id);
      continue;
    }
    pkt->kbuf_index = frame_id;
    pkt->buf_source = SFU_BUF_SOURCE_AF_XDP;
    pkt->buf_owner = &g_xdp;
    g_xdp.frames[frame_id].state = SFU_XDP_FRAME_RX_APP;
    on_recv(user_data, pkt);
  }
  if (count) {
    xsk_ring_cons__release(&g_xdp.rx, count);
  }
  refill_rx();
  return count;
}

static unsigned reap_worker_completions(sfu_ring_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher,
                                        sfu_on_send_complete_fn on_send_complete, void *user_data) {
  unsigned count = 0;
  void *item;
  while (count < max_count && sfu_spsc_ring_pop(&r->tx_completed, &item)) {
    sfu_packet_t *pkt = item;
    if (r->outstanding_sends > 0) {
      r->outstanding_sends--;
    }
    if (on_send_complete) {
      on_send_complete(user_data, pkt);
    }
    if (release_to_dispatcher) {
      sfu_worker_release_packet(pp, release_to_dispatcher, pkt);
    } else {
      sfu_ring_release_packet(r, pp, pkt);
    }
    count++;
  }
  return count;
}

unsigned sfu_ring_reap(sfu_ring_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher, sfu_on_recv_fn on_recv,
                       sfu_on_send_complete_fn on_send_complete, void *user_data) {
  if (!r || !g_xdp.initialized) {
    return 0;
  }
  if (r->with_recv_bufs) {
    return on_recv ? reap_rx(max_count, pp, on_recv, user_data) : 0;
  }
  return reap_worker_completions(r, max_count, pp, release_to_dispatcher, on_send_complete, user_data);
}

void sfu_ring_release_packet(sfu_ring_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
  (void)r;
  if (!sfu_packet_release(pkt)) {
    return;
  }
  if (pkt->buf_source == SFU_BUF_SOURCE_AF_XDP) {
    uint32_t frame = pkt->kbuf_index;
    sfu_packet_pool_free_meta(pp, pkt);
    recycle_rx_frame(frame);
    refill_rx();
  } else if (pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_packet_arena_free(pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

void sfu_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher, sfu_packet_t *pkt) {
  if (!sfu_packet_release(pkt)) {
    return;
  }
  if (pkt->buf_source == SFU_BUF_SOURCE_AF_XDP) {
    uint32_t frame = pkt->kbuf_index;
    sfu_packet_pool_free_meta(pp, pkt);
    void *item = (void *)(uintptr_t)((uint64_t)frame + 1u);
    while (!sfu_spsc_ring_push(to_dispatcher, item)) {
      sched_yield();
    }
  } else if (pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_packet_arena_free(pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

unsigned sfu_ring_drain_kernel_buffer_returns(sfu_ring_t *r, sfu_spsc_ring_t *from_worker, unsigned max_count) {
  (void)r;
  unsigned count = 0;
  void *item;
  while (count < max_count && sfu_spsc_ring_pop(from_worker, &item)) {
    uint32_t frame = (uint32_t)((uintptr_t)item - 1u);
    recycle_rx_frame(frame);
    count++;
  }
  refill_rx();
  return count;
}

typedef enum {
  SFU_AF_XDP_TX_BUILD_ERROR = -1,
  SFU_AF_XDP_TX_KERNEL_FALLBACK = 0,
  SFU_AF_XDP_TX_FRAME_READY = 1,
} sfu_af_xdp_tx_build_result_t;

static sfu_af_xdp_tx_build_result_t build_tx_frame(uint32_t frame_id, sfu_packet_t *pkt, uint32_t *out_length) {
  struct sockaddr_in *destination = (struct sockaddr_in *)&pkt->peer_addr;
  uint8_t destination_mac[ETH_ALEN];
  if (lookup_neighbor(&destination->sin_addr, destination_mac) != 0) {
    sfu_metric_inc("af_xdp_neighbor_miss");
    return SFU_AF_XDP_TX_KERNEL_FALLBACK;
  }

  uint8_t *frame = xsk_umem__get_data(g_xdp.umem_area, (uint64_t)frame_id * g_xdp.frame_size);
  sfu_af_xdp_frame_params_t params = {
      .source_ip = g_xdp.local_ip,
      .destination_ip = destination->sin_addr,
      .source_port = htons(g_xdp.media_port),
      .destination_port = destination->sin_port,
      .payload = pkt->data,
      .payload_len = pkt->len,
  };
  memcpy(params.source_mac, g_xdp.local_mac, sizeof(params.source_mac));
  memcpy(params.destination_mac, destination_mac, sizeof(params.destination_mac));
  return sfu_af_xdp_build_frame(frame, g_xdp.frame_size, &params, out_length) ? SFU_AF_XDP_TX_FRAME_READY : SFU_AF_XDP_TX_BUILD_ERROR;
}

static int kernel_fallback_send(sfu_packet_t *pkt) {
  ssize_t sent = sendto(g_xdp.socket_fd, pkt->data, pkt->len, MSG_DONTWAIT, (const struct sockaddr *)&pkt->peer_addr, pkt->peer_addr_len);
  if (sent == (ssize_t)pkt->len) {
    sfu_metric_inc("af_xdp_tx_kernel_fallback");
    return 1;
  }
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOBUFS)) {
    sfu_metric_inc("af_xdp_tx_kernel_fallback_backpressure");
    return 0;
  }
  sfu_metric_inc("af_xdp_tx_kernel_fallback_error");
  return -1;
}

static unsigned complete_waiting_frames(void) {
  unsigned count = 0;
  for (uint32_t i = g_xdp.rx_frame_count; i < g_xdp.frame_count; i++) {
    sfu_xdp_frame_meta_t *meta = &g_xdp.frames[i];
    if (meta->state != SFU_XDP_FRAME_TX_COMPLETE_WAIT || !meta->origin || !meta->packet) {
      continue;
    }
    if (!sfu_spsc_ring_push(&meta->origin->tx_completed, meta->packet)) {
      continue;
    }
    meta->packet = NULL;
    meta->origin = NULL;
    meta->state = SFU_XDP_FRAME_TX_FREE;
    g_xdp.tx_free[g_xdp.tx_free_count++] = i;
    count++;
  }
  return count;
}

static unsigned reap_tx_completions(void) {
  uint32_t index;
  unsigned count = xsk_ring_cons__peek(&g_xdp.completion, SFU_AF_XDP_BATCH, &index);
  for (unsigned i = 0; i < count; i++) {
    uint64_t address = *xsk_ring_cons__comp_addr(&g_xdp.completion, index + i);
    uint32_t frame = (uint32_t)(xsk_umem__extract_addr(address) / g_xdp.frame_size);
    if (frame >= g_xdp.rx_frame_count && frame < g_xdp.frame_count && g_xdp.frames[frame].state == SFU_XDP_FRAME_TX_KERNEL) {
      g_xdp.frames[frame].state = SFU_XDP_FRAME_TX_COMPLETE_WAIT;
    } else {
      sfu_metric_inc("af_xdp_invalid_tx_completion");
    }
  }
  if (count) {
    xsk_ring_cons__release(&g_xdp.completion, count);
  }
  return count + complete_waiting_frames();
}

unsigned sfu_ring_backend_service(sfu_ring_t *recv_ring, sfu_ring_t *send_ring, uint32_t send_ring_count, unsigned max_count) {
  (void)recv_ring;
  if (!g_xdp.initialized) {
    return 0;
  }
  unsigned work = reap_tx_completions();
  for (uint32_t ring_index = 0; ring_index < send_ring_count; ring_index++) {
    sfu_ring_t *ring = &send_ring[ring_index];
    for (unsigned n = 0; n < max_count && g_xdp.tx_free_count > 0; n++) {
      sfu_packet_t *pkt = ring->tx_retry;
      if (!pkt) {
        void *item;
        if (!sfu_spsc_ring_pop(&ring->tx_pending, &item)) {
          break;
        }
        pkt = item;
        ring->tx_retry = pkt;
      }

      uint32_t frame = (uint32_t)g_xdp.tx_free[--g_xdp.tx_free_count];
      uint32_t length = 0;
      bool delivered_by_fallback = false;
      sfu_af_xdp_tx_build_result_t build_result = build_tx_frame(frame, pkt, &length);
      if (build_result == SFU_AF_XDP_TX_KERNEL_FALLBACK) {
        int fallback = kernel_fallback_send(pkt);
        if (fallback == 0) {
          g_xdp.tx_free[g_xdp.tx_free_count++] = frame;
          break;
        }
        if (fallback > 0) {
          delivered_by_fallback = true;
          sfu_metric_inc("af_xdp_tx_kernel_fallback_completed");
        }
        build_result = SFU_AF_XDP_TX_BUILD_ERROR;
      }

      if (build_result == SFU_AF_XDP_TX_BUILD_ERROR) {
        if (!delivered_by_fallback) {
          sfu_metric_inc("af_xdp_tx_permanent_failure");
        }
        ring->tx_retry = NULL;
        if (sfu_spsc_ring_push(&ring->tx_completed, pkt)) {
          g_xdp.tx_free[g_xdp.tx_free_count++] = frame;
        } else {
          g_xdp.frames[frame].packet = pkt;
          g_xdp.frames[frame].origin = ring;
          g_xdp.frames[frame].state = SFU_XDP_FRAME_TX_COMPLETE_WAIT;
        }
        work++;
        continue;
      }

      uint32_t tx_index;
      if (xsk_ring_prod__reserve(&g_xdp.tx, 1, &tx_index) != 1) {
        g_xdp.tx_free[g_xdp.tx_free_count++] = frame;
        sfu_metric_inc("af_xdp_tx_ring_backpressure");
        break;
      }
      struct xdp_desc *desc = xsk_ring_prod__tx_desc(&g_xdp.tx, tx_index);
      desc->addr = (uint64_t)frame * g_xdp.frame_size;
      desc->len = length;
      g_xdp.frames[frame].packet = pkt;
      g_xdp.frames[frame].origin = ring;
      g_xdp.frames[frame].state = SFU_XDP_FRAME_TX_KERNEL;
      ring->tx_retry = NULL;
      xsk_ring_prod__submit(&g_xdp.tx, 1);
      work++;
    }
  }
  if (work && xsk_ring_prod__needs_wakeup(&g_xdp.tx)) {
    (void)sendto(xsk_socket__fd(g_xdp.xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
  }
  return work;
}

unsigned sfu_ring_backend_cancel(sfu_ring_t *send_rings, uint32_t send_ring_count) {
  if (!g_xdp.initialized || !send_rings) {
    return 0;
  }

  unsigned work = 0;
  for (uint32_t ring_index = 0; ring_index < send_ring_count; ring_index++) {
    sfu_ring_t *ring = &send_rings[ring_index];
    if (!ring->tx_retry) {
      void *item = NULL;
      if (sfu_spsc_ring_pop(&ring->tx_pending, &item)) {
        ring->tx_retry = item;
      }
    }
    if (ring->tx_retry && sfu_spsc_ring_push(&ring->tx_completed, ring->tx_retry)) {
      ring->tx_retry = NULL;
      sfu_metric_inc("af_xdp_tx_canceled_shutdown");
      work++;
    }
  }

  for (uint32_t frame = g_xdp.rx_frame_count; frame < g_xdp.frame_count; frame++) {
    sfu_xdp_frame_meta_t *meta = &g_xdp.frames[frame];
    if (!meta->packet || !meta->origin) {
      continue;
    }
    if (!sfu_spsc_ring_push(&meta->origin->tx_completed, meta->packet)) {
      continue;
    }
    bool kernel_owned = meta->state == SFU_XDP_FRAME_TX_KERNEL;
    meta->packet = NULL;
    meta->origin = NULL;
    meta->state = SFU_XDP_FRAME_TX_FREE;
    if (!kernel_owned) {
      g_xdp.tx_free[g_xdp.tx_free_count++] = frame;
    }
    sfu_metric_inc("af_xdp_tx_canceled_shutdown");
    work++;
  }
  return work;
}

uint32_t sfu_ring_outstanding_sends(const sfu_ring_t *r) { return r ? atomic_load_explicit(&r->outstanding_sends, memory_order_relaxed) : 0; }

#endif /* USE_AF_XDP */
