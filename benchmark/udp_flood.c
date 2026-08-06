/*
 * udp_flood -- sends fixed-size UDP datagrams as fast as possible to a
 * target host:port and reports achieved packets/sec. Use this against
 * mezon-sfu's dispatcher to benchmark raw ingestion (multishot recvmsg +
 * provided buffers) before any RTP parsing is in the picture.
 *
 * Usage: udp_flood <host> <port> <duration_sec> [packet_size]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <mimalloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <host> <port> <duration_sec> [packet_size=200]\n", argv[0]);
    return 1;
  }

  const char *host = argv[1];
  uint16_t port = (uint16_t)atoi(argv[2]);
  int duration_sec = atoi(argv[3]);
  int pkt_size = argc >= 5 ? atoi(argv[4]) : 200;

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
    fprintf(stderr, "invalid host: %s\n", host);
    return 1;
  }

  char *buf = mi_malloc(pkt_size);
  memset(buf, 0x42, pkt_size);

  uint64_t start = now_ns();
  uint64_t deadline = start + (uint64_t)duration_sec * 1000000000ULL;
  uint64_t sent = 0;

  while (now_ns() < deadline) {
    ssize_t rc = sendto(fd, buf, pkt_size, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (rc < 0) {
      continue;
    }
    sent++;
  }

  uint64_t elapsed_ns = now_ns() - start;
  double secs = (double)elapsed_ns / 1e9;
  double pps = (double)sent / secs;

  printf("sent=%lu duration=%.2fs pps=%.0f throughput=%.2f Mbps\n", (unsigned long)sent, secs, pps, (pps * pkt_size * 8) / 1e6);

  mi_free(buf);
  close(fd);
  return 0;
}
