#include "memory/packet_pool.h"
#include "memory/refcount.h"
#include "peer/session.h"
#include "pipeline/router.h"
#include "rtp/rtp_packet.h"
#include "runtime/fanout.h"
#include "runtime/worker.h"
#include "util/alloc.h"
#include "util/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERATIONS 1000000ull
#define DEFAULT_WARMUP 10000ull
#define DEFAULT_WORKERS 4u
#define DEFAULT_FANOUT 3u
#define DEFAULT_SUBSCRIBERS DEFAULT_FANOUT
#define DEFAULT_PACKET_SIZE 1200u
#define QUICK_ITERATIONS 1000ull
#define QUICK_WARMUP 100ull

typedef enum bench_kind {
  BENCH_ALL = 0,
  BENCH_RTP_PARSE,
  BENCH_PACKET_POOL,
  BENCH_FANOUT_MESH,
  BENCH_MEDIA_FANOUT,
} bench_kind_t;

typedef struct bench_config {
  bench_kind_t kind;
  uint64_t iterations;
  uint64_t warmup;
  uint32_t workers;
  uint32_t fanout;
  uint32_t subscribers;
  uint32_t packet_size;
  bool csv;
} bench_config_t;

typedef struct bench_result {
  const char *name;
  uint64_t iterations;
  uint64_t jobs;
  uint64_t total_ns;
  uint32_t workers;
  uint32_t fanout;
  uint32_t subscribers;
  uint32_t packet_size;
} bench_result_t;

typedef struct fanout_collector {
  sfu_fanout_mesh_t *mesh;
  uint64_t drained;
} fanout_collector_t;

typedef struct media_fanout_collector {
  sfu_fanout_mesh_t *mesh;
  sfu_packet_pool_t *pool;
  uint64_t drained_jobs;
  uint64_t drained_targets;
} media_fanout_collector_t;

static volatile uint64_t g_sink;

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
    perror("clock_gettime");
    exit(1);
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool parse_u64(const char *s, uint64_t *out) {
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return false;
  }
  *out = (uint64_t)v;
  return true;
}

static bool parse_u32(const char *s, uint32_t *out) {
  uint64_t v;
  if (!parse_u64(s, &v) || v > UINT32_MAX) {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "usage: %s [all|rtp_parse|packet_pool|fanout_mesh|media_fanout] [options]\n"
          "\n"
          "options:\n"
          "  --iterations N   timed iterations, default %llu\n"
          "  --warmup N       warmup iterations, default %llu\n"
          "  --workers N      fanout mesh worker count, default %u\n"
          "  --fanout N       fanout mesh targets per packet, default %u\n"
          "  --subscribers N  media fanout subscribers, default %u\n"
          "  --packet-size N  synthetic packet size, default %u\n"
          "  --csv            print CSV rows\n"
          "  --quick          use a small smoke-test workload\n"
          "  --help           show this help\n",
          prog, (unsigned long long)DEFAULT_ITERATIONS, (unsigned long long)DEFAULT_WARMUP, DEFAULT_WORKERS, DEFAULT_FANOUT, DEFAULT_SUBSCRIBERS,
          DEFAULT_PACKET_SIZE);
}

static bool parse_kind(const char *s, bench_kind_t *kind) {
  if (strcmp(s, "all") == 0) {
    *kind = BENCH_ALL;
  } else if (strcmp(s, "rtp_parse") == 0) {
    *kind = BENCH_RTP_PARSE;
  } else if (strcmp(s, "packet_pool") == 0) {
    *kind = BENCH_PACKET_POOL;
  } else if (strcmp(s, "fanout_mesh") == 0) {
    *kind = BENCH_FANOUT_MESH;
  } else if (strcmp(s, "media_fanout") == 0) {
    *kind = BENCH_MEDIA_FANOUT;
  } else {
    return false;
  }
  return true;
}

static bool parse_args(int argc, char **argv, bench_config_t *cfg) {
  cfg->kind = BENCH_ALL;
  cfg->iterations = DEFAULT_ITERATIONS;
  cfg->warmup = DEFAULT_WARMUP;
  cfg->workers = DEFAULT_WORKERS;
  cfg->fanout = DEFAULT_FANOUT;
  cfg->subscribers = DEFAULT_SUBSCRIBERS;
  cfg->packet_size = DEFAULT_PACKET_SIZE;
  cfg->csv = false;

  int i = 1;
  if (i < argc && argv[i][0] != '-') {
    if (!parse_kind(argv[i], &cfg->kind)) {
      fprintf(stderr, "unknown benchmark: %s\n", argv[i]);
      return false;
    }
    i++;
  }

  while (i < argc) {
    const char *arg = argv[i++];
    if (strcmp(arg, "--help") == 0) {
      usage(argv[0]);
      exit(0);
    } else if (strcmp(arg, "--csv") == 0) {
      cfg->csv = true;
    } else if (strcmp(arg, "--quick") == 0) {
      cfg->iterations = QUICK_ITERATIONS;
      cfg->warmup = QUICK_WARMUP;
    } else if (strcmp(arg, "--iterations") == 0) {
      if (i >= argc || !parse_u64(argv[i++], &cfg->iterations)) {
        fprintf(stderr, "invalid --iterations\n");
        return false;
      }
    } else if (strcmp(arg, "--warmup") == 0) {
      if (i >= argc || !parse_u64(argv[i++], &cfg->warmup)) {
        fprintf(stderr, "invalid --warmup\n");
        return false;
      }
    } else if (strcmp(arg, "--workers") == 0) {
      if (i >= argc || !parse_u32(argv[i++], &cfg->workers)) {
        fprintf(stderr, "invalid --workers\n");
        return false;
      }
    } else if (strcmp(arg, "--fanout") == 0) {
      if (i >= argc || !parse_u32(argv[i++], &cfg->fanout)) {
        fprintf(stderr, "invalid --fanout\n");
        return false;
      }
    } else if (strcmp(arg, "--subscribers") == 0) {
      if (i >= argc || !parse_u32(argv[i++], &cfg->subscribers)) {
        fprintf(stderr, "invalid --subscribers\n");
        return false;
      }
    } else if (strcmp(arg, "--packet-size") == 0) {
      if (i >= argc || !parse_u32(argv[i++], &cfg->packet_size)) {
        fprintf(stderr, "invalid --packet-size\n");
        return false;
      }
    } else {
      fprintf(stderr, "unknown option: %s\n", arg);
      return false;
    }
  }

  if (cfg->iterations == 0 || cfg->packet_size < 12 || cfg->workers < 2 || cfg->fanout == 0 || cfg->fanout > SFU_FANOUT_BATCH_CAP ||
      cfg->fanout >= cfg->workers || cfg->subscribers == 0 || cfg->subscribers > SFU_FANOUT_BATCH_CAP) {
    fprintf(stderr, "invalid benchmark configuration\n");
    return false;
  }
  return true;
}

static void print_csv_header(void) { printf("benchmark,iterations,jobs,total_ns,ns_per_op,ops_per_sec,workers,fanout,packet_size\n"); }

static void print_result(const bench_result_t *r, bool csv) {
  double denom = r->jobs ? (double)r->jobs : (double)r->iterations;
  double ns_per_op = (double)r->total_ns / denom;
  double ops_per_sec = denom * 1000000000.0 / (double)r->total_ns;
  if (csv) {
    printf("%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.2f,%.2f,%u,%u,%u\n", r->name, r->iterations, r->jobs, r->total_ns, ns_per_op, ops_per_sec, r->workers,
           r->fanout, r->packet_size);
    return;
  }
  if (strcmp(r->name, "fanout_mesh") == 0) {
    printf("benchmark=%s iterations=%" PRIu64 " jobs=%" PRIu64 " workers=%u fanout=%u packet_size=%u total_ns=%" PRIu64 " ns_per_job=%.2f jobs_per_sec=%.2f\n",
           r->name, r->iterations, r->jobs, r->workers, r->fanout, r->packet_size, r->total_ns, ns_per_op, ops_per_sec);
  } else if (strcmp(r->name, "media_fanout") == 0) {
    printf("benchmark=%s iterations=%" PRIu64 " jobs=%" PRIu64 " workers=%u subscribers=%u packet_size=%u total_ns=%" PRIu64
           " ns_per_target=%.2f targets_per_sec=%.2f\n",
           r->name, r->iterations, r->jobs, r->workers, r->fanout, r->packet_size, r->total_ns, ns_per_op, ops_per_sec);
  } else {
    printf("benchmark=%s iterations=%" PRIu64 " packet_size=%u total_ns=%" PRIu64 " ns_per_op=%.2f ops_per_sec=%.2f\n", r->name, r->iterations, r->packet_size,
           r->total_ns, ns_per_op, ops_per_sec);
  }
}

static void make_rtp_packet(uint8_t *data, uint32_t len) {
  memset(data, 0xab, len);
  data[0] = 0x80;
  data[1] = 0xe0;
  data[2] = 0x12;
  data[3] = 0x34;
  data[4] = 0x01;
  data[5] = 0x23;
  data[6] = 0x45;
  data[7] = 0x67;
  data[8] = 0x89;
  data[9] = 0xab;
  data[10] = 0xcd;
  data[11] = 0xef;
}

static bench_result_t bench_rtp_parse(const bench_config_t *cfg) {
  uint8_t *data = malloc(cfg->packet_size);
  if (!data) {
    perror("malloc");
    exit(1);
  }
  make_rtp_packet(data, cfg->packet_size);

  sfu_rtp_packet_t packet;
  for (uint64_t i = 0; i < cfg->warmup; i++) {
    if (!sfu_rtp_packet_parse(data, cfg->packet_size, &packet)) {
      fprintf(stderr, "rtp_parse warmup failed\n");
      exit(1);
    }
    g_sink += packet.sequence_number;
  }

  uint64_t start = now_ns();
  for (uint64_t i = 0; i < cfg->iterations; i++) {
    if (!sfu_rtp_packet_parse(data, cfg->packet_size, &packet)) {
      fprintf(stderr, "rtp_parse failed\n");
      exit(1);
    }
    g_sink += packet.sequence_number + packet.timestamp + packet.ssrc + packet.payload_len;
  }
  uint64_t total = now_ns() - start;
  free(data);

  return (bench_result_t){.name = "rtp_parse", .iterations = cfg->iterations, .jobs = cfg->iterations, .total_ns = total, .packet_size = cfg->packet_size};
}

static bench_result_t bench_packet_pool(const bench_config_t *cfg) {
  sfu_packet_pool_t pool;
  uint32_t capacity = 1024;
  if (sfu_packet_pool_init(&pool, capacity, cfg->packet_size) != 0) {
    fprintf(stderr, "sfu_packet_pool_init failed\n");
    exit(1);
  }

  for (uint64_t i = 0; i < cfg->warmup; i++) {
    sfu_packet_t *pkt = sfu_packet_pool_alloc(&pool);
    if (!pkt) {
      fprintf(stderr, "packet_pool warmup alloc failed\n");
      exit(1);
    }
    pkt->len = cfg->packet_size;
    pkt->data[0] = (uint8_t)i;
    if (!sfu_packet_release(pkt)) {
      fprintf(stderr, "packet_pool warmup release failed\n");
      exit(1);
    }
    sfu_packet_pool_free(&pool, pkt);
  }

  uint64_t start = now_ns();
  for (uint64_t i = 0; i < cfg->iterations; i++) {
    sfu_packet_t *pkt = sfu_packet_pool_alloc(&pool);
    if (!pkt) {
      fprintf(stderr, "packet_pool alloc failed\n");
      exit(1);
    }
    pkt->len = cfg->packet_size;
    pkt->data[0] = (uint8_t)i;
    sfu_packet_retain(pkt, 2);
    g_sink += pkt->data[0] + pkt->len;
    if (sfu_packet_release(pkt) || sfu_packet_release(pkt) || !sfu_packet_release(pkt)) {
      fprintf(stderr, "packet_pool release failed\n");
      exit(1);
    }
    sfu_packet_pool_free(&pool, pkt);
  }
  uint64_t total = now_ns() - start;

  sfu_packet_pool_destroy(&pool);
  return (bench_result_t){.name = "packet_pool", .iterations = cfg->iterations, .jobs = cfg->iterations, .total_ns = total, .packet_size = cfg->packet_size};
}

static struct sockaddr_storage make_addr(uint16_t port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  struct sockaddr_storage s;
  memset(&s, 0, sizeof(s));
  memcpy(&s, &a, sizeof(a));
  return s;
}

static void collect_fanout(void *user_data, sfu_fanout_job_t *job) {
  fanout_collector_t *c = (fanout_collector_t *)user_data;
  c->drained++;
  if (job->kind == SFU_FANOUT_JOB_BATCH) {
    for (uint8_t i = 0; i < job->target_count; i++) {
      sfu_session_release(job->targets[i].subscriber);
    }
    if (!sfu_packet_release(job->pkt)) {
      fprintf(stderr, "fanout packet release failed\n");
      exit(1);
    }
  }
  sfu_fanout_mesh_free_job(c->mesh, job);
}

static bench_result_t bench_fanout_mesh(const bench_config_t *cfg) {
  sfu_fanout_mesh_t mesh;
  uint32_t ring_capacity = 4096;
  uint32_t job_pool_capacity = cfg->workers * ring_capacity;
  if (sfu_fanout_mesh_init(&mesh, cfg->workers, ring_capacity, job_pool_capacity) != 0) {
    fprintf(stderr, "sfu_fanout_mesh_init failed\n");
    exit(1);
  }

  sfu_packet_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.len = cfg->packet_size;

  sfu_peer_session_t subscribers[SFU_FANOUT_BATCH_CAP];
  sfu_fanout_target_t targets[SFU_FANOUT_BATCH_CAP];
  memset(subscribers, 0, sizeof(subscribers));
  memset(targets, 0, sizeof(targets));
  for (uint32_t i = 0; i < cfg->fanout; i++) {
    atomic_store(&subscribers[i].refcount, 1);
    targets[i].subscriber = &subscribers[i];
    targets[i].dst = make_addr((uint16_t)(10000 + i));
    targets[i].dst_len = sizeof(struct sockaddr_in);
  }

  fanout_collector_t collector = {.mesh = &mesh, .drained = 0};
  for (uint64_t i = 0; i < cfg->warmup; i++) {
    atomic_store(&pkt.refcount, 1);
    if (!sfu_fanout_mesh_enqueue_forward_batch(&mesh, 0, 1, &pkt, NULL, targets, (uint8_t)cfg->fanout, true, NULL, false, false)) {
      fprintf(stderr, "fanout warmup enqueue failed\n");
      exit(1);
    }
    if (sfu_fanout_mesh_drain(&mesh, 1, 1, collect_fanout, &collector) != 1) {
      fprintf(stderr, "fanout warmup drain failed\n");
      exit(1);
    }
  }

  collector.drained = 0;
  uint64_t start = now_ns();
  for (uint64_t i = 0; i < cfg->iterations; i++) {
    uint32_t dst_worker = (uint32_t)(1 + (i % (cfg->workers - 1)));
    atomic_store(&pkt.refcount, 1);
    if (!sfu_fanout_mesh_enqueue_forward_batch(&mesh, 0, dst_worker, &pkt, NULL, targets, (uint8_t)cfg->fanout, true, NULL, false, false)) {
      fprintf(stderr, "fanout enqueue failed\n");
      exit(1);
    }
    if (sfu_fanout_mesh_drain(&mesh, dst_worker, 1, collect_fanout, &collector) != 1) {
      fprintf(stderr, "fanout drain failed\n");
      exit(1);
    }
  }
  uint64_t total = now_ns() - start;

  if (collector.drained != cfg->iterations) {
    fprintf(stderr, "fanout drained mismatch\n");
    exit(1);
  }
  g_sink += collector.drained;
  sfu_fanout_mesh_destroy(&mesh);
  return (bench_result_t){.name = "fanout_mesh",
                          .iterations = cfg->iterations,
                          .jobs = cfg->iterations * cfg->fanout,
                          .total_ns = total,
                          .workers = cfg->workers,
                          .fanout = cfg->fanout,
                          .packet_size = cfg->packet_size};
}


static sfu_peer_session_t *mock_media_session(const char *ufrag, uint16_t owner_worker, uint16_t port) {
  sfu_peer_session_t *s = SFU_CALLOC(1, sizeof(*s));
  if (!s) {
    perror("SFU_CALLOC");
    exit(1);
  }
  s->cold = SFU_CALLOC(1, sizeof(*s->cold));
  if (!s->cold) {
    perror("SFU_CALLOC");
    exit(1);
  }

  s->room_slot = UINT32_MAX;
  snprintf(s->cold->ufrag, sizeof(s->cold->ufrag), "%s", ufrag);
  s->cold->addr = make_addr(port);
  s->cold->addr_len = sizeof(struct sockaddr_in);
  s->active = true;
  s->state = SFU_SESSION_ESTABLISHED;
  pthread_mutex_init(&s->answer_lock, NULL);
  pthread_mutex_init(&s->negotiation.lock, NULL);
  pthread_mutex_init(&s->media.lock, NULL);
  pthread_mutex_init(&s->graph.lock, NULL);
  pthread_mutex_init(&s->ingress_lock, NULL);
  atomic_store(&s->refcount, 1);
  atomic_store(&s->lifecycle, SFU_SESSION_LIFECYCLE_OPEN);
  atomic_store(&s->accepts_work, true);
  atomic_store(&s->media.visible, true);
  sfu_session_set_owner_worker(s, owner_worker);
  s->media.uplink_audio.owner = s;
  s->media.uplink_video.owner = s;
  s->media.uplink_video.active = true;
  return s;
}

static void free_mock_media_session(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }
  pthread_mutex_destroy(&s->ingress_lock);
  pthread_mutex_destroy(&s->graph.lock);
  pthread_mutex_destroy(&s->media.lock);
  pthread_mutex_destroy(&s->negotiation.lock);
  pthread_mutex_destroy(&s->answer_lock);
  SFU_FREE(s->cold);
  SFU_FREE(s);
}

static sfu_fanout_bundle_t *make_video_fanout_bundle(sfu_peer_session_t **subs, uint32_t count) {
  sfu_fanout_bundle_t *bundle = sfu_fanout_bundle_alloc();
  if (!bundle) {
    perror("SFU_CALLOC");
    exit(1);
  }
  bundle->generation = 1;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t remote_slot = 0;
    const uint64_t assignment_generation = (uint64_t)i + 1;
    atomic_store_explicit(&subs[i]->graph.remote_slots.applied_assignment_generations[remote_slot], assignment_generation, memory_order_release);
    sfu_fanout_route_t route = {.subscriber = subs[i],
                                .video_ssrc = 0x11110000u + i,
                                .video_rtx_ssrc = 0x22220000u + i,
                                .remote_slot = remote_slot,
                                .assignment_generation = assignment_generation,
                                .video_pt = 96,
                                .video_rtx_pt = 97};
    if (!sfu_fanout_bundle_set(bundle, i, &route, SFU_FANOUT_VIDEO)) {
      sfu_fanout_bundle_release(bundle);
      fprintf(stderr, "fanout bundle allocation failed\n");
      exit(1);
    }
  }
  return bundle;
}

static void init_bench_worker(sfu_worker_t *w, sfu_packet_pool_t *pool, sfu_fanout_mesh_t *mesh) {
  memset(w, 0, sizeof(*w));
  w->worker_index = 0;
  w->pp = pool;
  w->mesh = mesh;
  w->fd = -1;
  if (sfu_spsc_ring_init(&w->release_to_dispatcher, 1024) != 0) {
    fprintf(stderr, "release_to_dispatcher init failed\n");
    exit(1);
  }
}

static void collect_media_fanout(void *user_data, sfu_fanout_job_t *job) {
  media_fanout_collector_t *c = (media_fanout_collector_t *)user_data;
  c->drained_jobs++;
  if (job->kind == SFU_FANOUT_JOB_BATCH) {
    c->drained_targets += job->target_count;
    for (uint8_t i = 0; i < job->target_count; i++) {
      sfu_session_release(job->targets[i].subscriber);
    }
    if (job->publisher) {
      sfu_session_release(job->publisher);
    }
    if (sfu_packet_release(job->pkt)) {
      sfu_packet_pool_free(c->pool, job->pkt);
    }
  }
  sfu_fanout_mesh_free_job(c->mesh, job);
}

static void run_media_fanout_iteration(sfu_worker_t *w, sfu_packet_pool_t *pool, sfu_fanout_mesh_t *mesh, sfu_peer_session_t *publisher,
                                       const uint8_t *template_data, const sfu_rtp_packet_t *rtp, const bench_config_t *cfg,
                                       media_fanout_collector_t *collector) {
  sfu_packet_t *pkt = sfu_packet_pool_alloc(pool);
  if (!pkt) {
    fprintf(stderr, "media_fanout packet alloc failed\n");
    exit(1);
  }
  memcpy(pkt->data, template_data, cfg->packet_size);
  pkt->len = cfg->packet_size;

  sfu_ingress_media_t media = {
      .pkt = pkt,
      .rtp = *rtp,
      .source = SFU_MEDIA_VIDEO,
      .is_audio = false,
      .has_svc = false,
      .is_keyframe = false,
  };

  uint64_t before_targets = collector->drained_targets;
  sfu_router_forward(w, publisher, &media);
  for (uint32_t dst = 1; dst < cfg->workers; dst++) {
    while (sfu_fanout_mesh_drain(mesh, dst, SFU_FANOUT_BATCH_CAP, collect_media_fanout, collector) > 0) {
    }
  }
  if (collector->drained_targets - before_targets != cfg->subscribers) {
    fprintf(stderr, "media_fanout target mismatch\n");
    exit(1);
  }
}

static bench_result_t bench_media_fanout(const bench_config_t *cfg) {
  sfu_packet_pool_t pool;
  sfu_fanout_mesh_t mesh;
  sfu_worker_t worker;
  uint32_t ring_capacity = 4096;
  uint32_t job_pool_capacity = cfg->workers * ring_capacity;

  if (sfu_packet_pool_init(&pool, 4096, cfg->packet_size) != 0) {
    fprintf(stderr, "media_fanout packet pool init failed\n");
    exit(1);
  }
  if (sfu_fanout_mesh_init(&mesh, cfg->workers, ring_capacity, job_pool_capacity) != 0) {
    fprintf(stderr, "media_fanout mesh init failed\n");
    exit(1);
  }
  init_bench_worker(&worker, &pool, &mesh);

  sfu_peer_session_t *publisher = mock_media_session("pub", 0, 9000);
  sfu_peer_session_t *subscribers[SFU_FANOUT_BATCH_CAP];
  for (uint32_t i = 0; i < cfg->subscribers; i++) {
    char ufrag[32];
    snprintf(ufrag, sizeof(ufrag), "sub%u", i);
    uint16_t owner = (uint16_t)(1 + (i % (cfg->workers - 1)));
    subscribers[i] = mock_media_session(ufrag, owner, (uint16_t)(10000 + i));
  }

  sfu_fanout_bundle_t *bundle = make_video_fanout_bundle(subscribers, cfg->subscribers);
  sfu_session_publish_fanout(publisher, bundle);

  uint8_t *template_data = malloc(cfg->packet_size);
  if (!template_data) {
    perror("malloc");
    exit(1);
  }
  make_rtp_packet(template_data, cfg->packet_size);
  sfu_rtp_packet_t rtp;
  if (!sfu_rtp_packet_parse(template_data, cfg->packet_size, &rtp)) {
    fprintf(stderr, "media_fanout RTP template parse failed\n");
    exit(1);
  }

  media_fanout_collector_t collector = {.mesh = &mesh, .pool = &pool};
  for (uint64_t i = 0; i < cfg->warmup; i++) {
    run_media_fanout_iteration(&worker, &pool, &mesh, publisher, template_data, &rtp, cfg, &collector);
  }

  collector.drained_jobs = 0;
  collector.drained_targets = 0;
  uint64_t start = now_ns();
  for (uint64_t i = 0; i < cfg->iterations; i++) {
    run_media_fanout_iteration(&worker, &pool, &mesh, publisher, template_data, &rtp, cfg, &collector);
  }
  uint64_t total = now_ns() - start;

  if (collector.drained_targets != cfg->iterations * cfg->subscribers) {
    fprintf(stderr, "media_fanout drained target mismatch\n");
    exit(1);
  }
  g_sink += collector.drained_jobs + collector.drained_targets;

  sfu_fanout_bundle_t *old = sfu_session_publish_fanout_swap(publisher, NULL);
  sfu_fanout_bundle_release(old);
  free(template_data);
  for (uint32_t i = 0; i < cfg->subscribers; i++) {
    free_mock_media_session(subscribers[i]);
  }
  free_mock_media_session(publisher);
  sfu_spsc_ring_destroy(&worker.release_to_dispatcher);
  sfu_fanout_mesh_destroy(&mesh);
  sfu_packet_pool_destroy(&pool);

  return (bench_result_t){.name = "media_fanout",
                          .iterations = cfg->iterations,
                          .jobs = cfg->iterations * cfg->subscribers,
                          .total_ns = total,
                          .workers = cfg->workers,
                          .fanout = cfg->subscribers,
                          .packet_size = cfg->packet_size};
}

static void run_one(bench_kind_t kind, const bench_config_t *cfg) {
  bench_result_t result;
  switch (kind) {
    case BENCH_RTP_PARSE:
      result = bench_rtp_parse(cfg);
      break;
    case BENCH_PACKET_POOL:
      result = bench_packet_pool(cfg);
      break;
    case BENCH_FANOUT_MESH:
      result = bench_fanout_mesh(cfg);
      break;
    case BENCH_MEDIA_FANOUT:
      result = bench_media_fanout(cfg);
      break;
    case BENCH_ALL:
    default:
      return;
  }
  print_result(&result, cfg->csv);
}

int main(int argc, char **argv) {
  sfu_log_set_level(SFU_LOG_LEVEL_WARN);

  bench_config_t cfg;
  if (!parse_args(argc, argv, &cfg)) {
    usage(argv[0]);
    return 2;
  }

  if (cfg.csv) {
    print_csv_header();
  }

  if (cfg.kind == BENCH_ALL) {
    run_one(BENCH_RTP_PARSE, &cfg);
    run_one(BENCH_PACKET_POOL, &cfg);
    run_one(BENCH_FANOUT_MESH, &cfg);
    run_one(BENCH_MEDIA_FANOUT, &cfg);
  } else {
    run_one(cfg.kind, &cfg);
  }

  return g_sink == UINT64_MAX ? 1 : 0;
}
