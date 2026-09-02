#include "net/net.h"


#include "memory/refcount.h"
#include "memory/worker_packet_arena.h"
#include "net/af_xdp_frame.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/metrics.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <dirent.h>
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
#define SFU_AF_XDP_MAX_QUEUES 128u

typedef struct sfu_xdp_queue sfu_xdp_queue_t;

struct sfu_net {
  sfu_xdp_queue_t *queue;
  sfu_spsc_ring_t tx_pending;
  sfu_spsc_ring_t tx_completed;
  sfu_packet_t *tx_retry;
  _Atomic uint32_t outstanding_sends;
  uint32_t queue_capacity;
  uint32_t queue_slot;
  int fd;
  bool with_recv_bufs;
  bool queues_initialized;
  bool queue_bound;
};

typedef enum {
  SFU_XDP_FRAME_RX_FREE = 0,
  SFU_XDP_FRAME_RX_FILL,
  SFU_XDP_FRAME_RX_APP,
  SFU_XDP_FRAME_TX_FREE,
  SFU_XDP_FRAME_TX_KERNEL,
  SFU_XDP_FRAME_TX_COMPLETE_WAIT,
  SFU_XDP_FRAME_TX_KERNEL_ORPHANED,
} sfu_xdp_frame_state_t;

typedef struct {
  sfu_packet_t *packet;
  sfu_net_t *origin;
  uint8_t state;
} sfu_xdp_frame_meta_t;

typedef struct {
  uint16_t media_port;
  uint16_t enabled;
  uint32_t reserved;
} sfu_xdp_bpf_config_t;

typedef struct sfu_xdp_queue {
  struct xsk_umem *umem;
  struct xsk_socket *xsk;
  struct xsk_ring_prod fill;
  struct xsk_ring_cons completion;
  struct xsk_ring_cons rx;
  struct xsk_ring_prod tx;
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
  uint32_t slot;
  bool map_published;
  bool initialized;
} sfu_xdp_queue_t;

typedef struct {
  struct bpf_object *bpf_object;
  sfu_xdp_queue_t *queues;
  uint32_t queue_count;
  uint32_t rx_cursor;
  uint32_t tx_cursor;
  uint32_t frame_size;
  uint32_t xdp_flags;
  uint32_t xdp_mode_flags;
  uint32_t xdp_program_id;
  int socket_fd;
  int ifindex;
  int xsks_map_fd;
  int config_map_fd;
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

_Static_assert(sizeof(uintptr_t) >= sizeof(uint64_t), "AF_XDP multi-queue return tokens require 64-bit uintptr_t");

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u32(const void *lhs, const void *rhs) {
  uint32_t a = *(const uint32_t *)lhs;
  uint32_t b = *(const uint32_t *)rhs;
  return (a > b) - (a < b);
}

static bool interface_queue_exists(const char *interface_name, uint32_t queue_id) {
  char path[256];
  int written = snprintf(path, sizeof(path), "/sys/class/net/%s/queues/rx-%u", interface_name, queue_id);
  return written > 0 && (size_t)written < sizeof(path) && access(path, F_OK) == 0;
}

static int discover_rx_queues(const char *interface_name, uint32_t *queue_ids, uint32_t *queue_count) {
  char path[256];
  int written = snprintf(path, sizeof(path), "/sys/class/net/%s/queues", interface_name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return -1;
  }
  DIR *dir = opendir(path);
  if (!dir) {
    return -1;
  }
  uint32_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    unsigned id;
    char extra;
    if (sscanf(entry->d_name, "rx-%u%c", &id, &extra) != 1) {
      continue;
    }
    if (id >= SFU_AF_XDP_MAX_QUEUES || count >= SFU_AF_XDP_MAX_QUEUES) {
      closedir(dir);
      return -1;
    }
    queue_ids[count++] = (uint32_t)id;
  }
  closedir(dir);
  if (count == 0) {
    return -1;
  }
  qsort(queue_ids, count, sizeof(*queue_ids), compare_u32);
  *queue_count = count;
  return 0;
}

static int parse_queue_spec(const char *interface_name, const char *spec, uint32_t fallback_queue_id, bool fallback_set, uint32_t *queue_ids,
                            uint32_t *queue_count) {
  if (fallback_set) {
    if (fallback_queue_id >= SFU_AF_XDP_MAX_QUEUES || !interface_queue_exists(interface_name, fallback_queue_id)) {
      return -1;
    }
    queue_ids[0] = fallback_queue_id;
    *queue_count = 1;
    return 0;
  }
  if (!spec || !*spec || strcmp(spec, "auto") == 0) {
    return discover_rx_queues(interface_name, queue_ids, queue_count);
  }

  char copy[256];
  if (snprintf(copy, sizeof(copy), "%s", spec) >= (int)sizeof(copy)) {
    return -1;
  }
  uint32_t count = 0;
  char *save = NULL;
  for (char *token = strtok_r(copy, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
    while (*token == ' ' || *token == '\t') {
      token++;
    }
    char *end = NULL;
    errno = 0;
    unsigned long id = strtoul(token, &end, 10);
    while (end && (*end == ' ' || *end == '\t')) {
      end++;
    }
    if (errno || !*token || !end || *end || id >= SFU_AF_XDP_MAX_QUEUES || count >= SFU_AF_XDP_MAX_QUEUES ||
        !interface_queue_exists(interface_name, (uint32_t)id)) {
      return -1;
    }
    queue_ids[count++] = (uint32_t)id;
  }
  if (count == 0) {
    return -1;
  }
  qsort(queue_ids, count, sizeof(*queue_ids), compare_u32);
  for (uint32_t i = 1; i < count; i++) {
    if (queue_ids[i] == queue_ids[i - 1]) {
      return -1;
    }
  }
  *queue_count = count;
  return 0;
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

static int set_redirect_enabled(bool enabled) {
  if (g_xdp.config_map_fd < 0) {
    return -1;
  }
  uint32_t key = 0;
  sfu_xdp_bpf_config_t value = {.media_port = htons(g_xdp.media_port), .enabled = enabled ? 1u : 0u, .reserved = 0};
  return bpf_map_update_elem(g_xdp.config_map_fd, &key, &value, BPF_ANY);
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
    rc = bpf_xdp_attach(d->ifindex, program_fd, flags, NULL);
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
  d->config_map_fd = bpf_map__fd(config);
  return set_redirect_enabled(false);
}

static void refill_rx(sfu_xdp_queue_t *q) {
  while (q->rx_free_count > 0) {
    uint32_t index;
    if (xsk_ring_prod__reserve(&q->fill, 1, &index) != 1) {
      break;
    }
    uint64_t frame = q->rx_free[--q->rx_free_count];
    *xsk_ring_prod__fill_addr(&q->fill, index) = frame * q->frame_size;
    q->frames[frame].state = SFU_XDP_FRAME_RX_FILL;
    xsk_ring_prod__submit(&q->fill, 1);
  }
}

static void recycle_rx_frame(sfu_xdp_queue_t *q, uint32_t frame) {
  if (!q || frame >= q->rx_frame_count || q->rx_free_count >= q->rx_frame_count) {
    sfu_metric_inc("af_xdp_invalid_rx_frame");
    return;
  }
  q->frames[frame].state = SFU_XDP_FRAME_RX_FREE;
  q->rx_free[q->rx_free_count++] = frame;
}

static void destroy_queue(sfu_xdp_queue_t *q) {
  if (!q) {
    return;
  }
  if (q->map_published && g_xdp.xsks_map_fd >= 0) {
    (void)bpf_map_delete_elem(g_xdp.xsks_map_fd, &q->queue_id);
    q->map_published = false;
  }
  if (q->xsk) {
    xsk_socket__delete(q->xsk);
    q->xsk = NULL;
  }
  if (q->umem) {
    xsk_umem__delete(q->umem);
    q->umem = NULL;
  }
  SFU_FREE(q->frames);
  SFU_FREE(q->tx_free);
  SFU_FREE(q->rx_free);
  free(q->umem_area);
  q->umem_area = NULL;
  q->initialized = false;
}

static int init_queue(sfu_xdp_queue_t *q, uint32_t queue_id, uint32_t slot, uint32_t frame_count, uint32_t frame_size) {
  memset(q, 0, sizeof(*q));
  q->queue_id = queue_id;
  q->slot = slot;
  q->frame_count = frame_count;
  q->frame_size = frame_size;
  if (!sfu_af_xdp_partition_frames(frame_count, &q->rx_frame_count, &q->tx_frame_count)) {
    return -1;
  }

  size_t umem_size = (size_t)frame_count * frame_size;
  if (posix_memalign(&q->umem_area, (size_t)getpagesize(), umem_size) != 0) {
    return -1;
  }
  memset(q->umem_area, 0, umem_size);
  q->rx_free = SFU_CALLOC(q->rx_frame_count, sizeof(*q->rx_free));
  q->tx_free = SFU_CALLOC(q->tx_frame_count, sizeof(*q->tx_free));
  q->frames = SFU_CALLOC(frame_count, sizeof(*q->frames));
  if (!q->rx_free || !q->tx_free || !q->frames) {
    return -1;
  }

  struct xsk_umem_config umem_config = {
      .fill_size = q->rx_frame_count,
      .comp_size = q->tx_frame_count,
      .frame_size = frame_size,
      .frame_headroom = 0,
      .flags = 0,
  };
  int rc = xsk_umem__create(&q->umem, q->umem_area, umem_size, &q->fill, &q->completion, &umem_config);
  if (rc != 0) {
    SFU_LOG_ERROR("AF_XDP: xsk_umem__create failed for queue %u: %s (%d)", queue_id, strerror(-rc), rc);
    return -1;
  }

  struct xsk_socket_config socket_config = {
      .rx_size = q->rx_frame_count,
      .tx_size = q->tx_frame_count,
      .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
      .xdp_flags = g_xdp.xdp_flags & (XDP_FLAGS_SKB_MODE | XDP_FLAGS_DRV_MODE),
      .bind_flags = XDP_USE_NEED_WAKEUP,
  };
  rc = xsk_socket__create(&q->xsk, g_xdp.interface_name, queue_id, q->umem, &q->rx, &q->tx, &socket_config);
  if (rc != 0) {
    SFU_LOG_ERROR("AF_XDP: xsk_socket__create failed on %s queue %u: %s (%d)", g_xdp.interface_name, queue_id, strerror(-rc), rc);
    return -1;
  }
  int xsk_fd = xsk_socket__fd(q->xsk);
  if (bpf_map_update_elem(g_xdp.xsks_map_fd, &queue_id, &xsk_fd, BPF_ANY) != 0) {
    SFU_LOG_ERROR("AF_XDP: failed to publish XSK for queue %u: %s", queue_id, strerror(errno));
    return -1;
  }
  q->map_published = true;

  for (uint32_t i = 0; i < q->rx_frame_count; i++) {
    q->rx_free[q->rx_free_count++] = i;
    q->frames[i].state = SFU_XDP_FRAME_RX_FREE;
  }
  for (uint32_t i = q->rx_frame_count; i < frame_count; i++) {
    q->tx_free[q->tx_free_count++] = i;
    q->frames[i].state = SFU_XDP_FRAME_TX_FREE;
  }
  refill_rx(q);
  q->initialized = true;
  return 0;
}

int sfu_net_backend_init(int fd, const sfu_net_backend_options_t *options) {
  memset(&g_xdp, 0, sizeof(g_xdp));
  g_xdp.xsks_map_fd = -1;
  g_xdp.config_map_fd = -1;
  if (!options || !options->interface_name || !*options->interface_name || options->frame_count < 8 ||
      (options->frame_count & (options->frame_count - 1)) != 0 || (options->frame_size != 2048u && options->frame_size != 4096u) ||
      options->frame_size < sfu_net_recv_overhead() + 1u || (options->queue_spec && options->queue_id_set) ||
      (options->xdp_mode && strcmp(options->xdp_mode, "native") != 0 && strcmp(options->xdp_mode, "skb") != 0 && strcmp(options->xdp_mode, "auto") != 0)) {
    SFU_LOG_ERROR("AF_XDP: invalid backend options");
    return -1;
  }
  g_xdp.socket_fd = fd;
  g_xdp.media_port = options->media_port;
  g_xdp.frame_size = options->frame_size;
  snprintf(g_xdp.interface_name, sizeof(g_xdp.interface_name), "%s", options->interface_name);
  g_xdp.ifindex = if_nametoindex(options->interface_name);
  if (!g_xdp.ifindex || read_interface_addresses(&g_xdp) != 0) {
    SFU_LOG_ERROR("AF_XDP: cannot resolve interface '%s'", options->interface_name);
    goto fail;
  }
  read_default_gateway(&g_xdp);

  uint32_t queue_ids[SFU_AF_XDP_MAX_QUEUES];
  uint32_t queue_count = 0;
  if (parse_queue_spec(options->interface_name, options->queue_spec, options->queue_id, options->queue_id_set, queue_ids, &queue_count) != 0) {
    SFU_LOG_ERROR("AF_XDP: invalid queue selection '%s' on %s", options->queue_spec ? options->queue_spec : "(legacy)", options->interface_name);
    goto fail;
  }
  uint32_t per_queue_frames = sfu_af_xdp_frames_per_queue(options->frame_count, queue_count);
  if (per_queue_frames == 0) {
    SFU_LOG_ERROR("AF_XDP: frame_count %u is too small for %u queues", options->frame_count, queue_count);
    goto fail;
  }
  g_xdp.queues = SFU_CALLOC(queue_count, sizeof(*g_xdp.queues));
  if (!g_xdp.queues) {
    goto fail;
  }
  g_xdp.queue_count = queue_count;

  if (attach_xdp_program(&g_xdp, options->xdp_mode ? options->xdp_mode : "native") != 0) {
    SFU_LOG_ERROR("AF_XDP: failed to load or attach %s", SFU_XDP_OBJECT_PATH);
    goto fail;
  }
  for (uint32_t i = 0; i < queue_count; i++) {
    if (init_queue(&g_xdp.queues[i], queue_ids[i], i, per_queue_frames, options->frame_size) != 0) {
      goto fail;
    }
    SFU_LOG_INFO("AF_XDP queue %u initialized: %u frames x %u bytes (rx=%u tx=%u)", queue_ids[i], per_queue_frames, options->frame_size,
                 g_xdp.queues[i].rx_frame_count, g_xdp.queues[i].tx_frame_count);
  }
  if (set_redirect_enabled(true) != 0) {
    SFU_LOG_ERROR("AF_XDP: failed to enable XDP redirection: %s", strerror(errno));
    goto fail;
  }
  g_xdp.initialized = true;
  uint64_t total_bytes = (uint64_t)queue_count * per_queue_frames * options->frame_size;
  SFU_LOG_INFO("AF_XDP initialized on %s: %u queues, %u frames/queue, total_umem=%llu MiB", options->interface_name, queue_count, per_queue_frames,
               (unsigned long long)(total_bytes / (1024 * 1024)));
  return 0;

fail:
  sfu_net_backend_destroy();
  return -1;
}

void sfu_net_backend_destroy(void) {
  if (g_xdp.config_map_fd >= 0) {
    (void)set_redirect_enabled(false);
  }
  if (g_xdp.queues) {
    for (uint32_t i = g_xdp.queue_count; i > 0; i--) {
      destroy_queue(&g_xdp.queues[i - 1]);
    }
    SFU_FREE(g_xdp.queues);
  }
  if (g_xdp.xdp_attached) {
    uint32_t current_program_id = 0;
    if (bpf_xdp_query_id(g_xdp.ifindex, g_xdp.xdp_mode_flags, &current_program_id) == 0 && current_program_id == g_xdp.xdp_program_id) {
      (void)bpf_xdp_detach(g_xdp.ifindex, g_xdp.xdp_flags, NULL);
    } else if (current_program_id != 0) {
      SFU_LOG_WARN("AF_XDP: not detaching XDP program %u; owned program was %u", current_program_id, g_xdp.xdp_program_id);
    }
  }
  if (g_xdp.bpf_object) {
    bpf_object__close(g_xdp.bpf_object);
  }
  memset(&g_xdp, 0, sizeof(g_xdp));
  g_xdp.xsks_map_fd = -1;
  g_xdp.config_map_fd = -1;
}

uint32_t sfu_net_recv_overhead(void) { return (uint32_t)(sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr)); }
uint32_t sfu_net_recv_slot_size(uint32_t payload_cap) { return g_xdp.frame_size ? g_xdp.frame_size : payload_cap + sfu_net_recv_overhead(); }
uint64_t sfu_net_recv_capacity_bytes(const sfu_net_backend_options_t *backend_options, uint32_t recv_buffer_count, uint32_t payload_cap) {
  (void)recv_buffer_count;
  (void)payload_cap;
  return backend_options ? (uint64_t)backend_options->frame_count * backend_options->frame_size : 0;
}

sfu_net_t *sfu_net_create(const sfu_net_options_t *options) {
  if (!options) {
    return NULL;
  }
  sfu_net_t *r = SFU_CALLOC(1, sizeof(*r));
  if (!r) {
    return NULL;
  }
  int fd = options->fd;
  uint32_t sq_entries = options->send_entries;
  bool with_recv_bufs = options->receive;
  r->fd = fd;
  r->with_recv_bufs = with_recv_bufs;
  r->queue_slot = options->queue_slot;

  if (g_xdp.initialized && options->queue_slot < g_xdp.queue_count) {
    r->queue = &g_xdp.queues[options->queue_slot];
    r->queue_bound = true;
    uint32_t capacity = sq_entries;
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      capacity = SFU_AF_XDP_TX_QUEUE_CAPACITY;
    }
    if (sfu_spsc_ring_init(&r->tx_completed, capacity) == 0) {
      r->queues_initialized = true;
    }
    return r;
  }

  if (!with_recv_bufs) {
    uint32_t capacity = sq_entries;
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      capacity = SFU_AF_XDP_TX_QUEUE_CAPACITY;
    }
    if (sfu_spsc_ring_init(&r->tx_pending, capacity) != 0 || sfu_spsc_ring_init(&r->tx_completed, capacity) != 0) {
      sfu_spsc_ring_destroy(&r->tx_pending);
      SFU_FREE(r);
      return NULL;
    }
    r->queue_capacity = capacity;
    r->queues_initialized = true;
  }
  if (with_recv_bufs && !g_xdp.initialized) {
    SFU_FREE(r);
    return NULL;
  }
  return r;
}

void sfu_net_destroy(sfu_net_t *r) {
  if (r && r->queues_initialized) {
    if (!r->queue_bound) {
      void *item;
      if (r->tx_retry) {
        atomic_fetch_sub_explicit(&r->outstanding_sends, 1, memory_order_relaxed);
        (void)sfu_packet_release(r->tx_retry);
        r->tx_retry = NULL;
      }
      while (sfu_spsc_ring_pop(&r->tx_pending, &item)) {
        atomic_fetch_sub_explicit(&r->outstanding_sends, 1, memory_order_relaxed);
        (void)sfu_packet_release((sfu_packet_t *)item);
      }
    }
    sfu_spsc_ring_destroy(&r->tx_completed);
    if (!r->queue_bound) {
      sfu_spsc_ring_destroy(&r->tx_pending);
    }
    r->queues_initialized = false;
  }
  SFU_FREE(r);
}

int sfu_net_recv(sfu_net_t *r) { return r && (r->queue_bound || (r->with_recv_bufs && g_xdp.initialized)) ? 0 : -1; }

typedef enum {
  SFU_AF_XDP_TX_BUILD_ERROR = -1,
  SFU_AF_XDP_TX_KERNEL_FALLBACK = 0,
  SFU_AF_XDP_TX_FRAME_READY = 1,
} sfu_af_xdp_tx_build_result_t;

static int kernel_fallback_send(sfu_packet_t *pkt);
static sfu_af_xdp_tx_build_result_t build_tx_frame(sfu_xdp_queue_t *q, uint32_t frame_id, sfu_packet_t *pkt, uint32_t *out_length);
static unsigned reap_tx_completions(sfu_xdp_queue_t *q);

int sfu_net_send(sfu_net_t *r, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len) {
  if (!r || !pkt || !dst || dst->sa_family != AF_INET || dst_len != sizeof(struct sockaddr_in)) {
    return -1;
  }

  memcpy(&pkt->peer_addr, dst, dst_len);
  pkt->peer_addr_len = dst_len;

  if (r->queue_bound && r->queue) {
    sfu_xdp_queue_t *q = r->queue;
    if (q->tx_free_count == 0) {
      sfu_metric_inc("af_xdp_tx_frame_starvation");
      return -1;
    }

    uint32_t frame = (uint32_t)q->tx_free[--q->tx_free_count];
    uint32_t length = 0;
    bool delivered_by_fallback = false;
    sfu_af_xdp_tx_build_result_t build_result = build_tx_frame(q, frame, pkt, &length);
    if (build_result == SFU_AF_XDP_TX_KERNEL_FALLBACK) {
      int fallback = kernel_fallback_send(pkt);
      if (fallback == 0) {
        q->tx_free[q->tx_free_count++] = frame;
        return -1;
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
        q->tx_free[q->tx_free_count++] = frame;
        return -1;
      }
      q->tx_free[q->tx_free_count++] = frame;
      memcpy(&pkt->peer_addr, dst, dst_len);
      pkt->peer_addr_len = dst_len;
      sfu_packet_retain(pkt, 1);
      r->outstanding_sends++;
      if (r->queues_initialized && sfu_spsc_ring_push(&r->tx_completed, pkt)) {
        return 0;
      }
      r->outstanding_sends--;
      (void)sfu_packet_release(pkt);
      sfu_metric_inc("af_xdp_tx_ring_backpressure");
      return -1;
    }

    uint32_t tx_index;
    if (xsk_ring_prod__reserve(&q->tx, 1, &tx_index) != 1) {
      q->tx_free[q->tx_free_count++] = frame;
      sfu_metric_inc("af_xdp_tx_ring_backpressure");
      return -1;
    }

    struct xdp_desc *desc = xsk_ring_prod__tx_desc(&q->tx, tx_index);
    desc->addr = (uint64_t)frame * q->frame_size;
    desc->len = length;
    q->frames[frame].packet = pkt;
    q->frames[frame].origin = r;
    q->frames[frame].state = SFU_XDP_FRAME_TX_KERNEL;

    memcpy(&pkt->peer_addr, dst, dst_len);
    pkt->peer_addr_len = dst_len;
    sfu_packet_retain(pkt, 1);
    r->outstanding_sends++;

    xsk_ring_prod__submit(&q->tx, 1);
    if (xsk_ring_prod__needs_wakeup(&q->tx)) {
      (void)sendto(xsk_socket__fd(q->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
    }
    return 0;
  }

  if (!r->queues_initialized) {
    return -1;
  }
  memcpy(&pkt->peer_addr, dst, dst_len);
  pkt->peer_addr_len = dst_len;
  sfu_packet_retain(pkt, 1);
  if (!sfu_spsc_ring_push(&r->tx_pending, pkt)) {
    (void)sfu_packet_release(pkt);
    sfu_metric_inc("af_xdp_pending_full");
    return -1;
  }
  sfu_metric_inc("af_xdp_send_queued");
  r->outstanding_sends++;
  return 0;
}

int sfu_net_flush(sfu_net_t *r) {
  if (r && r->queue_bound && r->queue && xsk_ring_prod__needs_wakeup(&r->queue->tx)) {
    (void)sendto(xsk_socket__fd(r->queue->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
  }
  return 0;
}

static bool parse_rx_packet(sfu_xdp_queue_t *q, uint8_t *frame, uint32_t length, sfu_packet_t *pkt) {
  sfu_af_xdp_parse_result_t parsed;
  if (!sfu_af_xdp_parse_frame(frame, length, q->frame_size, g_xdp.media_port, &parsed)) {
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

static unsigned reap_rx_queue(sfu_xdp_queue_t *q, unsigned max_count, sfu_packet_pool_t *pp, sfu_net_on_recv_fn on_recv, void *user_data) {
  uint32_t index;
  unsigned count = xsk_ring_cons__peek(&q->rx, max_count, &index);
  for (unsigned i = 0; i < count; i++) {
    const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&q->rx, index + i);
    uint64_t address = xsk_umem__extract_addr(desc->addr);
    uint32_t frame_id = (uint32_t)(address / q->frame_size);
    uint8_t *frame = xsk_umem__get_data(q->umem_area, address);
    sfu_packet_t *pkt = NULL;
    if (frame_id < q->rx_frame_count && desc->len <= q->frame_size) {
      pkt = sfu_packet_pool_alloc_meta(pp);
    }
    if (!pkt || !parse_rx_packet(q, frame, desc->len, pkt)) {
      if (pkt) {
        sfu_packet_pool_free_meta(pp, pkt);
      }
      recycle_rx_frame(q, frame_id);
      continue;
    }
    pkt->kbuf_index = frame_id;
    pkt->buf_source = SFU_BUF_SOURCE_AF_XDP;
    pkt->buf_owner = q;
    q->frames[frame_id].state = SFU_XDP_FRAME_RX_APP;
#ifdef SFU_DIAG_LOG
    if (pkt->len >= 20 && (pkt->data[0] & 0xc0) == 0 && pkt->data[4] == 0x21 && pkt->data[5] == 0x12 && pkt->data[6] == 0xa4 && pkt->data[7] == 0x42) {
      const struct sockaddr_in *peer = (const struct sockaddr_in *)&pkt->peer_addr;
      char ip[INET_ADDRSTRLEN] = "?";
      char transaction_id[25];
      (void)inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof(ip));
      for (size_t j = 0; j < 12; j++) {
        snprintf(transaction_id + j * 2, 3, "%02x", (unsigned)pkt->data[8 + j]);
      }
      uint16_t message_type = (uint16_t)(((uint16_t)pkt->data[0] << 8) | pkt->data[1]);
      SFU_LOG_INFO("AF_XDP RX STUN queue=%u frame=%u src=%s:%u len=%u type=0x%04x transaction_id=%s", q->queue_id, frame_id, ip,
                   (unsigned)ntohs(peer->sin_port), pkt->len, (unsigned)message_type, transaction_id);
    }
#endif
    on_recv(user_data, pkt);
  }
  if (count) {
    xsk_ring_cons__release(&q->rx, count);
  }
  refill_rx(q);
  return count;
}

static unsigned reap_rx(unsigned max_count, sfu_packet_pool_t *pp, sfu_net_on_recv_fn on_recv, void *user_data) {
  unsigned total = 0;
  uint32_t idle = 0;
  while (total < max_count && idle < g_xdp.queue_count) {
    uint32_t slot = g_xdp.rx_cursor++ % g_xdp.queue_count;
    unsigned budget = max_count - total;
    if (budget > SFU_AF_XDP_BATCH) {
      budget = SFU_AF_XDP_BATCH;
    }
    unsigned count = reap_rx_queue(&g_xdp.queues[slot], budget, pp, on_recv, user_data);
    total += count;
    idle = count ? 0 : idle + 1;
  }
  return total;
}

static unsigned reap_worker_completions(sfu_net_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher,
                                        sfu_net_on_send_complete_fn on_send_complete, void *user_data) {
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
      sfu_net_worker_release_packet(pp, release_to_dispatcher, pkt);
    } else {
      sfu_net_release_packet(r, pp, pkt);
    }
    count++;
  }
  return count;
}

unsigned sfu_net_poll(sfu_net_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher, sfu_net_on_recv_fn on_recv,
                      sfu_net_on_send_complete_fn on_send_complete, void *user_data) {
  if (!r) {
    return 0;
  }
  if (r->queue_bound && r->queue) {
    unsigned reaped = 0;
    if (on_recv) {
      reaped += reap_rx_queue(r->queue, max_count, pp, on_recv, user_data);
    }
    reaped += reap_tx_completions(r->queue);
    reaped += reap_worker_completions(r, max_count, pp, release_to_dispatcher, on_send_complete, user_data);
    return reaped;
  }
  if (!g_xdp.initialized) {
    return reap_worker_completions(r, max_count, pp, release_to_dispatcher, on_send_complete, user_data);
  }
  if (r->with_recv_bufs) {
    return on_recv ? reap_rx(max_count, pp, on_recv, user_data) : 0;
  }
  return reap_worker_completions(r, max_count, pp, release_to_dispatcher, on_send_complete, user_data);
}

void sfu_net_release_packet(sfu_net_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
  (void)r;
  if (!sfu_packet_release(pkt)) {
    return;
  }
  if (pkt->buf_source == SFU_BUF_SOURCE_AF_XDP) {
    sfu_xdp_queue_t *q = pkt->buf_owner;
    uint32_t frame = pkt->kbuf_index;
    sfu_packet_pool_free_meta(pp, pkt);
    recycle_rx_frame(q, frame);
    refill_rx(q);
  } else if (pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_packet_arena_free(pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

bool sfu_net_worker_release_packet_routed(sfu_net_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt, uint32_t current_worker,
                                          uint32_t *owner_worker, uintptr_t *token) {
  if (!r || !r->queue_bound || !pkt || pkt->buf_source != SFU_BUF_SOURCE_AF_XDP) {
    return false;
  }
  if (!sfu_packet_release(pkt)) {
    return true;
  }
  sfu_xdp_queue_t *q = pkt->buf_owner;
  uint32_t frame = pkt->kbuf_index;
  sfu_packet_pool_free_meta(pp, pkt);
  if (!q || q->slot >= g_xdp.queue_count || frame >= q->rx_frame_count) {
    sfu_metric_inc("af_xdp_invalid_rx_return");
    return true;
  }
  if (q->slot == current_worker) {
    recycle_rx_frame(q, frame);
    refill_rx(q);
    return true;
  }
  if (!owner_worker || !token || !sfu_af_xdp_encode_rx_return(q->slot, frame, token)) {
    sfu_metric_inc("af_xdp_invalid_rx_return");
    return true;
  }
  *owner_worker = q->slot;
  return true;
}

void sfu_net_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher, sfu_packet_t *pkt) {
  if (!sfu_packet_release(pkt)) {
    return;
  }
  if (pkt->buf_source == SFU_BUF_SOURCE_AF_XDP) {
    sfu_xdp_queue_t *q = pkt->buf_owner;
    uint32_t frame = pkt->kbuf_index;
    sfu_packet_pool_free_meta(pp, pkt);
    uintptr_t token;
    if (!sfu_af_xdp_encode_rx_return(q->slot, frame, &token)) {
      sfu_metric_inc("af_xdp_invalid_rx_return");
      return;
    }
    void *item = (void *)token;
    while (!sfu_spsc_ring_push(to_dispatcher, item)) {
      sched_yield();
    }
  } else if (pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_packet_arena_free(pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

bool sfu_net_recycle_rx_token(sfu_net_t *r, uintptr_t token) {
  uint32_t slot, frame;
  if (!r || !r->queue_bound || !r->queue || !sfu_af_xdp_decode_rx_return(token, &slot, &frame) || slot != r->queue_slot ||
      frame >= r->queue->rx_frame_count) {
    sfu_metric_inc("af_xdp_invalid_rx_return");
    return false;
  }
  recycle_rx_frame(r->queue, frame);
  refill_rx(r->queue);
  return true;
}

unsigned sfu_net_drain_buffer_returns(sfu_net_t *r, sfu_spsc_ring_t *from_worker, unsigned max_count) {
  (void)r;
  unsigned count = 0;
  void *item;
  while (count < max_count && sfu_spsc_ring_pop(from_worker, &item)) {
    uint32_t slot, frame;
    if (!sfu_af_xdp_decode_rx_return((uintptr_t)item, &slot, &frame) || slot >= g_xdp.queue_count || frame >= g_xdp.queues[slot].rx_frame_count) {
      sfu_metric_inc("af_xdp_invalid_rx_return");
      count++;
      continue;
    }
    recycle_rx_frame(&g_xdp.queues[slot], frame);
    refill_rx(&g_xdp.queues[slot]);
    count++;
  }
  return count;
}

static sfu_af_xdp_tx_build_result_t build_tx_frame(sfu_xdp_queue_t *q, uint32_t frame_id, sfu_packet_t *pkt, uint32_t *out_length) {
  struct sockaddr_in *destination = (struct sockaddr_in *)&pkt->peer_addr;
  uint8_t destination_mac[ETH_ALEN];
  if (lookup_neighbor(&destination->sin_addr, destination_mac) != 0) {
    sfu_metric_inc("af_xdp_neighbor_miss");
    return SFU_AF_XDP_TX_KERNEL_FALLBACK;
  }
  uint8_t *frame = xsk_umem__get_data(q->umem_area, (uint64_t)frame_id * q->frame_size);
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
  return sfu_af_xdp_build_frame(frame, q->frame_size, &params, out_length) ? SFU_AF_XDP_TX_FRAME_READY : SFU_AF_XDP_TX_BUILD_ERROR;
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

static unsigned complete_waiting_frames(sfu_xdp_queue_t *q) {
  unsigned count = 0;
  for (uint32_t i = q->rx_frame_count; i < q->frame_count; i++) {
    sfu_xdp_frame_meta_t *meta = &q->frames[i];
    if (meta->state != SFU_XDP_FRAME_TX_COMPLETE_WAIT || !meta->origin || !meta->packet) {
      continue;
    }
    if (!sfu_spsc_ring_push(&meta->origin->tx_completed, meta->packet)) {
      continue;
    }
    meta->packet = NULL;
    meta->origin = NULL;
    meta->state = SFU_XDP_FRAME_TX_FREE;
    q->tx_free[q->tx_free_count++] = i;
    count++;
  }
  return count;
}

static unsigned reap_tx_completions(sfu_xdp_queue_t *q) {
  uint32_t index;
  unsigned count = xsk_ring_cons__peek(&q->completion, SFU_AF_XDP_BATCH, &index);
  for (unsigned i = 0; i < count; i++) {
    uint64_t address = *xsk_ring_cons__comp_addr(&q->completion, index + i);
    uint32_t frame = (uint32_t)(xsk_umem__extract_addr(address) / q->frame_size);
    if (frame >= q->rx_frame_count && frame < q->frame_count && q->frames[frame].state == SFU_XDP_FRAME_TX_KERNEL) {
      q->frames[frame].state = SFU_XDP_FRAME_TX_COMPLETE_WAIT;
    } else if (frame >= q->rx_frame_count && frame < q->frame_count && q->frames[frame].state == SFU_XDP_FRAME_TX_KERNEL_ORPHANED) {
      q->frames[frame].state = SFU_XDP_FRAME_TX_FREE;
      q->tx_free[q->tx_free_count++] = frame;
    } else {
      sfu_metric_inc("af_xdp_invalid_tx_completion");
    }
  }
  if (count) {
    xsk_ring_cons__release(&q->completion, count);
  }
  return count + complete_waiting_frames(q);
}

static sfu_xdp_queue_t *next_tx_queue(void) {
  for (uint32_t i = 0; i < g_xdp.queue_count; i++) {
    uint32_t slot = g_xdp.tx_cursor++ % g_xdp.queue_count;
    if (g_xdp.queues[slot].tx_free_count > 0) {
      return &g_xdp.queues[slot];
    }
  }
  return NULL;
}

unsigned sfu_net_service(sfu_net_t *recv_net, sfu_net_t *send_net, unsigned max_count) {
  (void)recv_net;
  if (!g_xdp.initialized) {
    return 0;
  }
  unsigned work = 0;
  for (uint32_t q = 0; q < g_xdp.queue_count; q++) {
    work += reap_tx_completions(&g_xdp.queues[q]);
  }
  if (!send_net) {
    return work;
  }
  {
    sfu_net_t *ring = send_net;
    for (unsigned n = 0; n < max_count; n++) {
      sfu_packet_t *pkt = ring->tx_retry;
      if (!pkt) {
        void *item;
        if (!sfu_spsc_ring_pop(&ring->tx_pending, &item)) {
          break;
        }
        pkt = item;
        ring->tx_retry = pkt;
      }

      sfu_xdp_queue_t *q = next_tx_queue();
      if (!q) {
        sfu_metric_inc("af_xdp_tx_frame_starvation");
        break;
      }
      uint32_t frame = (uint32_t)q->tx_free[--q->tx_free_count];
      uint32_t length = 0;
      bool delivered_by_fallback = false;
      sfu_af_xdp_tx_build_result_t build_result = build_tx_frame(q, frame, pkt, &length);
      if (build_result == SFU_AF_XDP_TX_KERNEL_FALLBACK) {
        int fallback = kernel_fallback_send(pkt);
        if (fallback == 0) {
          q->tx_free[q->tx_free_count++] = frame;
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
          q->tx_free[q->tx_free_count++] = frame;
        } else {
          q->frames[frame].packet = pkt;
          q->frames[frame].origin = ring;
          q->frames[frame].state = SFU_XDP_FRAME_TX_COMPLETE_WAIT;
        }
        work++;
        continue;
      }

      uint32_t tx_index;
      if (xsk_ring_prod__reserve(&q->tx, 1, &tx_index) != 1) {
        q->tx_free[q->tx_free_count++] = frame;
        sfu_metric_inc("af_xdp_tx_ring_backpressure");
        break;
      }
      struct xdp_desc *desc = xsk_ring_prod__tx_desc(&q->tx, tx_index);
      desc->addr = (uint64_t)frame * q->frame_size;
      desc->len = length;
      q->frames[frame].packet = pkt;
      q->frames[frame].origin = ring;
      q->frames[frame].state = SFU_XDP_FRAME_TX_KERNEL;
      ring->tx_retry = NULL;
      xsk_ring_prod__submit(&q->tx, 1);
      if (xsk_ring_prod__needs_wakeup(&q->tx)) {
        (void)sendto(xsk_socket__fd(q->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
      }
      work++;
    }
  }
  return work;
}

unsigned sfu_net_cancel(sfu_net_t *send_net) {
  if (!g_xdp.initialized || !send_net) {
    return 0;
  }
  unsigned work = 0;
  {
    sfu_net_t *ring = send_net;
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
  for (uint32_t slot = 0; slot < g_xdp.queue_count; slot++) {
    sfu_xdp_queue_t *q = &g_xdp.queues[slot];
    for (uint32_t frame = q->rx_frame_count; frame < q->frame_count; frame++) {
      sfu_xdp_frame_meta_t *meta = &q->frames[frame];
      if (!meta->packet || !meta->origin) {
        continue;
      }
      if (!sfu_spsc_ring_push(&meta->origin->tx_completed, meta->packet)) {
        continue;
      }
      bool kernel_owned = meta->state == SFU_XDP_FRAME_TX_KERNEL;
      meta->packet = NULL;
      meta->origin = NULL;
      if (kernel_owned) {
        meta->state = SFU_XDP_FRAME_TX_KERNEL_ORPHANED;
      } else {
        meta->state = SFU_XDP_FRAME_TX_FREE;
        q->tx_free[q->tx_free_count++] = frame;
      }
      sfu_metric_inc("af_xdp_tx_canceled_shutdown");
      work++;
    }
  }
  return work;
}

bool sfu_net_backend_is_worker_driven(void) { return true; }
uint32_t sfu_net_backend_queue_count(void) { return g_xdp.initialized ? g_xdp.queue_count : 0; }

uint32_t sfu_net_outstanding_sends(const sfu_net_t *r) { return r ? atomic_load_explicit(&r->outstanding_sends, memory_order_relaxed) : 0; }
