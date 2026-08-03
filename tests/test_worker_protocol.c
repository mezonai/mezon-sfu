/*
 * Phase 2 worker protocol integration tests.
 *
 * Exercises sfu_room_forward_packet's compound-RTCP dispatch end to end:
 * plaintext feedback is SRTCP-protected through a real SRTP context, fed
 * through the worker ingress path, and the resulting NACK/PLI effects are
 * observed via counters, cache state and throttling. The io_uring send ring
 * is left zeroed so queued sends fail harmlessly; packet ownership is
 * verified through the pool release path instead.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "util/alloc.h"
#include "rtp/rtx.h"
#include "runtime/worker.h"
#include "sfu/datadef.h"
#include "transport/srtp/srtp.h"
#include "util/metrics.h"
#include "util/netbytes.h"

#define POOL_CAPACITY 64
#define RTP_PT 96
#define RTX_PT 97
#define MEDIA_SSRC 0x0a0b0c0du
#define RTX_SSRC 0x01020304u
#define SFU_KF_SENDER_SSRC 1u /* hardcoded in sfu_session_request_keyframe */

typedef struct {
  sfu_worker_t w;
  sfu_packet_pool_t pp;
  sfu_session_table_t sessions;
  sfu_dtls_ctx_t dtls_ctx; /* session creation needs an SSL_CTX; never used */
  sfu_srtp_ctx_t srtp;     /* single ctx: loopback protect/unprotect */
  sfu_rtx_cache_t *cache;
  sfu_peer_session_t *session;
  int send_fds[2]; /* pipe standing in for the io_uring send fd */
} fixture_t;

static void build_rtp_video(uint8_t *buf, uint16_t seq, size_t payload_len, size_t *out_len) {
  buf[0] = 0x80;
  buf[1] = RTP_PT;
  sfu_write_be16(buf + 2, seq);
  sfu_write_be32(buf + 4, 0x11223344u);
  sfu_write_be32(buf + 8, MEDIA_SSRC);
  memset(buf + 12, 0xab, payload_len);
  *out_len = 12 + payload_len;
}

static size_t rtcp_member_header(uint8_t *buf, uint8_t fmt, uint8_t pt, uint32_t sender_ssrc, size_t body_len) {
  size_t total = 8 + body_len;
  assert(total % 4 == 0);
  buf[0] = 0x80 | (fmt & 0x1F);
  buf[1] = pt;
  sfu_write_be16(buf + 2, (uint16_t)(total / 4 - 1));
  sfu_write_be32(buf + 4, sender_ssrc);
  return total;
}

static size_t build_nack(uint8_t *buf, const uint16_t *fci, size_t fci_halfwords) {
  size_t total = rtcp_member_header(buf, 1, 205, MEDIA_SSRC, 4 + fci_halfwords * 2);
  sfu_write_be32(buf + 8, MEDIA_SSRC);
  for (size_t i = 0; i < fci_halfwords; i++) {
    sfu_write_be16(buf + 12 + i * 2, fci[i]);
  }
  return total;
}

static size_t build_pli(uint8_t *buf) {
  size_t total = rtcp_member_header(buf, 1, 206, MEDIA_SSRC, 4);
  sfu_write_be32(buf + 8, MEDIA_SSRC);
  return total;
}

static size_t build_fir(uint8_t *buf) {
  size_t total = rtcp_member_header(buf, 4, 206, MEDIA_SSRC, 12);
  sfu_write_be32(buf + 8, MEDIA_SSRC);
  sfu_write_be32(buf + 12, MEDIA_SSRC); /* FCI entry: target SSRC */
  buf[16] = 7;                          /* seq nr */
  buf[17] = buf[18] = buf[19] = 0;      /* reserved */
  return total;
}

/* Feed one plaintext RTCP compound through the ingress path. */
static void feed_rtcp(fixture_t *f, const uint8_t *plain, size_t plain_len) {
  uint8_t wire[2048];
  memcpy(wire, plain, plain_len);
  int wire_len = (int)plain_len;
  bool ok = sfu_srtp_protect_rtcp(&f->srtp, wire, &wire_len, sizeof(wire));
  assert(ok);

  sfu_packet_t *pkt = sfu_packet_pool_alloc(&f->pp);
  assert(pkt != NULL);
  assert((size_t)wire_len <= pkt->cap);
  memcpy(pkt->data, wire, (size_t)wire_len);
  pkt->len = (uint32_t)wire_len;
  pkt->peer_addr = f->session->cold->addr;
  pkt->peer_addr_len = f->session->cold->addr_len;

  sfu_room_forward_packet(&f->w, pkt);
}

/* Count packets currently held by the pool (capacity minus free slots). */
static uint32_t pool_free_count(sfu_packet_pool_t *pp) {
  uint32_t n = 0;
  sfu_packet_t *tmp[POOL_CAPACITY];
  for (;;) {
    sfu_packet_t *p = sfu_packet_pool_alloc(pp);
    if (!p || n >= POOL_CAPACITY) {
      break;
    }
    tmp[n++] = p;
  }
  for (uint32_t i = 0; i < n; i++) {
    sfu_worker_release_packet(pp, NULL, tmp[i]);
  }
  return n;
}

static void fixture_init(fixture_t *f) {
  memset(f, 0, sizeof(*f));
  assert(sfu_srtp_global_init() == 0);
  assert(sfu_packet_pool_init(&f->pp, POOL_CAPACITY, 2048) == 0);
  f->dtls_ctx.ssl_ctx = SSL_CTX_new(TLS_method());
  assert(f->dtls_ctx.ssl_ctx != NULL);
  assert(sfu_session_table_init(&f->sessions, &f->dtls_ctx) == 0);
  sfu_metrics_init();

  uint8_t key_material[SFU_SRTP_KEY_MATERIAL_LEN];
  for (size_t i = 0; i < sizeof(key_material); i++) {
    key_material[i] = (uint8_t)(i * 7 + 1);
  }
  /* Default profile (AES128_CM_SHA1_80). Loopback: mirror the client half of
   * the keying material into the server half so outbound (client, since
   * is_server=false) and inbound (server) derive identical session keys. */
  memcpy(key_material + 16, key_material, 16);
  memcpy(key_material + 46, key_material + 32, 14);
  assert(sfu_srtp_ctx_init_from_dtls(&f->srtp, key_material, 0x0001, false) == 0);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(5000);
  addr.sin_addr.s_addr = htonl(0x7f000001u);

  f->session = sfu_session_table_get_or_create(&f->sessions, (const struct sockaddr_storage *)&addr, sizeof(addr));
  assert(f->session != NULL);
  memcpy(&f->session->srtp, &f->srtp, sizeof(f->srtp));
  f->session->state = SFU_SESSION_ESTABLISHED;
  f->session->worker_id = 0;
  /* The worker forwards RTX to a zeroed io_uring send ring; keeps the test
   * single-threaded and out of the kernel. The session's gcc/twcc/scheduler
   * sub-allocations stay unused (TWCC members are not exercised here; the
   * TWCC parser already has dedicated protocol tests). */
  SFU_FREE(f->session->gcc_ctx);
  f->session->gcc_ctx = NULL;
  SFU_FREE(f->session->twcc_history);
  f->session->twcc_history = NULL;
  SFU_FREE(f->session->scheduler);
  f->session->scheduler = NULL;

  f->cache = (sfu_rtx_cache_t *)SFU_CALLOC(1, sizeof(sfu_rtx_cache_t));
  assert(f->cache != NULL);
  assert(sfu_rtx_cache_init(f->cache) == 0);
  f->session->rtx_cache = f->cache;

  f->w.pp = &f->pp;
  f->w.sessions = &f->sessions;
  f->w.worker_index = 0;
  /* Small real send ring over a pipe fd: queueing RTX/keyframe sends stays
   * in-process and never submits, so nothing reaches the kernel. */
  assert(pipe(f->send_fds) == 0);
  assert(sfu_ring_init(&f->w.send_ring, f->send_fds[1], 8, 16, 0, 0, -1, false) == 0);
}

static void fixture_destroy(fixture_t *f) {
  sfu_ring_destroy(&f->w.send_ring);
  close(f->send_fds[0]);
  close(f->send_fds[1]);
  sfu_rtx_cache_destroy(f->cache);
  SFU_FREE(f->cache);
  f->session->rtx_cache = NULL; /* table teardown must not double-free */
  sfu_session_table_destroy(&f->sessions); /* destroys copied SRTP handles */
  f->srtp.inbound = NULL;
  f->srtp.outbound = NULL;
  sfu_packet_pool_destroy(&f->pp);
  sfu_srtp_global_deinit();
  SSL_CTX_free(f->dtls_ctx.ssl_ctx);
}

/* A valid NACK for a cached packet produces an RTX retransmission attempt,
 * which consumes the cache's RTX sequence space. */
static void test_compound_nack_rtx_dispatch(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t pkt_buf[512];
  size_t pkt_len;
  build_rtp_video(pkt_buf, 42, 100, &pkt_len);
  sfu_rtx_cache_put(f.cache, 42, pkt_buf, (uint32_t)pkt_len, RTX_SSRC, RTX_PT);
  assert(f.cache->next_rtx_seq == 0);

  uint8_t nack[64];
  size_t nack_len = build_nack(nack, (uint16_t[]){42, 0x0000}, 2);
  feed_rtcp(&f, nack, nack_len);

  assert(f.cache->next_rtx_seq == 1);
  assert(sfu_metric_get("rtcp_compound_malformed") == 0);
  assert(sfu_metric_get("rtcp_nack_bad") == 0);
  fixture_destroy(&f);
}

/* NACK + PLI in one compound: both members are dispatched (the old
 * first-header-only path would have dropped the PLI on the floor). */
static void test_compound_nack_then_pli(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t compound[128];
  size_t nack_len = build_nack(compound, (uint16_t[]){4242, 0x0000}, 2); /* not cached */
  size_t pli_len = build_pli(compound + nack_len);
  feed_rtcp(&f, compound, nack_len + pli_len);

  /* NACK cache miss marks unrecoverable loss -> throttled keyframe request;
   * the PLI member then finds last_pli_time already set and is a no-op. Both
   * members parsed cleanly, so nothing was flagged bad or malformed. */
  assert(f.session->last_pli_time != 0);
  assert(sfu_metric_get("rtcp_compound_malformed") == 0);
  assert(sfu_metric_get("rtcp_nack_bad") == 0);
  assert(sfu_metric_get("rtcp_pli_bad") == 0);
  fixture_destroy(&f);
}

/* PLI then NACK: the reverse order exercises PLI first. */
static void test_compound_pli_then_nack(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t pkt_buf[512];
  size_t pkt_len;
  build_rtp_video(pkt_buf, 7, 60, &pkt_len);
  sfu_rtx_cache_put(f.cache, 7, pkt_buf, (uint32_t)pkt_len, RTX_SSRC, RTX_PT);

  uint8_t compound[128];
  size_t pli_len = build_pli(compound);
  size_t nack_len = build_nack(compound + pli_len, (uint16_t[]){7, 0x0000}, 2);
  feed_rtcp(&f, compound, pli_len + nack_len);

  assert(f.session->last_pli_time != 0); /* PLI dispatched */
  assert(f.cache->next_rtx_seq == 1);    /* NACK dispatched and serviced */
  fixture_destroy(&f);
}

/* A malformed trailing member drops the remainder but keeps the members
 * parsed before it, and bumps rtcp_compound_malformed. */
static void test_malformed_tail_drops_remainder(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t pkt_buf[512];
  size_t pkt_len;
  build_rtp_video(pkt_buf, 9, 60, &pkt_len);
  sfu_rtx_cache_put(f.cache, 9, pkt_buf, (uint32_t)pkt_len, RTX_SSRC, RTX_PT);

  uint8_t compound[128];
  size_t nack_len = build_nack(compound, (uint16_t[]){9, 0x0000}, 2);
  /* Bogus member: version 3 -> iterator flags malformed and stops. */
  compound[nack_len] = 0xC1;
  compound[nack_len + 1] = 206;
  sfu_write_be16(compound + nack_len + 2, 2);
  memset(compound + nack_len + 4, 0, 8);
  feed_rtcp(&f, compound, nack_len + 12);

  assert(f.cache->next_rtx_seq == 1); /* NACK before the bad member ran */
  assert(sfu_metric_get("rtcp_compound_malformed") == 1);
  fixture_destroy(&f);
}

/* FCI whose length is not a multiple of 4 is rejected before iteration. */
static void test_bad_nack_fci(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t nack[64];
  uint16_t fci[] = {42, 0x0000};
  size_t nack_len = build_nack(nack, fci, 2);
  nack_len -= 2; /* lie: member ends mid-FCI */
  /* Fix the length word to match the odd logical size. */
  sfu_write_be16(nack + 2, 3); /* 16 bytes total -> 14 logical here; parser rejects */
  feed_rtcp(&f, nack, 14);

  /* The compound iterator rejects the member boundary before dispatching the
   * NACK parser; the parser's direct odd-FCI rejection is covered separately. */
  assert(sfu_metric_get("rtcp_compound_malformed") == 1);
  assert(sfu_metric_get("rtcp_nack_bad") == 0);
  assert(f.cache->next_rtx_seq == 0);
  fixture_destroy(&f);
}

/* Malformed PLI member (wrong body length) bumps rtcp_pli_bad. */
static void test_bad_pli(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t pli[64];
  size_t pli_len = rtcp_member_header(pli, 1, 206, MEDIA_SSRC, 8); /* body too long */
  sfu_write_be32(pli + 8, MEDIA_SSRC);
  sfu_write_be32(pli + 12, 0);
  feed_rtcp(&f, pli, pli_len);

  assert(sfu_metric_get("rtcp_pli_bad") == 1);
  assert(f.session->last_pli_time == 0);
  fixture_destroy(&f);
}

/* FIR member is parsed and ignored (no keyframe side effects, no metrics). */
static void test_fir_ignored(void) {
  fixture_t f;
  fixture_init(&f);

  uint8_t fir[64];
  size_t fir_len = build_fir(fir);
  feed_rtcp(&f, fir, fir_len);

  assert(sfu_metric_get("rtcp_compound_malformed") == 0);
  assert(sfu_metric_get("rtcp_pli_bad") == 0);
  assert(f.session->last_pli_time == 0);
  fixture_destroy(&f);
}

/* Every packet fed through the worker must be released exactly once; only
 * send-completed packets return to the pool (queued ZC sends retain a ref
 * until their CQE is reaped, and this test never submits). */
static void test_packet_release_ownership(void) {
  fixture_t f;
  fixture_init(&f);
  assert(pool_free_count(&f.pp) == POOL_CAPACITY);

  /* Malformed compound: dropped without any keyframe/RTX side effects. */
  uint8_t garbage[16] = {0xC1, 206, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  feed_rtcp(&f, garbage, sizeof(garbage));

  /* PLI triggers one throttled keyframe request: the worker's own PLI build
   * allocs one packet, queues it (retained ref), and drops its own ref. */
  uint8_t pli[64];
  size_t pli_len = build_pli(pli);
  feed_rtcp(&f, pli, pli_len);
  feed_rtcp(&f, pli, pli_len); /* throttled: no second keyframe packet */

  assert(pool_free_count(&f.pp) == POOL_CAPACITY - 1);
  fixture_destroy(&f);
}

int main(void) {
  test_compound_nack_rtx_dispatch();
  test_compound_nack_then_pli();
  test_compound_pli_then_nack();
  test_malformed_tail_drops_remainder();
  test_bad_nack_fci();
  test_bad_pli();
  test_fir_ignored();
  test_packet_release_ownership();
  printf("test_worker_protocol: OK\n");
  return 0;
}
