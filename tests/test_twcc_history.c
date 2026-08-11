#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "congestion/twcc_history.h"

static void test_received_consumes_once(void) {
  sfu_twcc_history_t history;
  gcc_packet_info_t pkt = {0};
  bool was_lost = true;
  sfu_twcc_history_init(&history);
  sfu_twcc_history_record(&history, 77, 1000, 1200);

  assert(sfu_twcc_history_consume_received(&history, 77, &pkt, &was_lost));
  assert(pkt.send_time_us == 1000);
  assert(pkt.size_bytes == 1200);
  assert(!was_lost);
  assert(!sfu_twcc_history_consume_received(&history, 77, &pkt, &was_lost));
  assert(sfu_twcc_history_lookup(&history, 77, &pkt));
}

static void test_loss_then_late_receive(void) {
  sfu_twcc_history_t history;
  gcc_packet_info_t pkt = {0};
  bool was_lost = false;
  sfu_twcc_history_init(&history);
  sfu_twcc_history_record(&history, 100, 2000, 900);

  assert(sfu_twcc_history_report_loss_once(&history, 100));
  assert(!sfu_twcc_history_report_loss_once(&history, 100));
  assert(sfu_twcc_history_consume_received(&history, 100, &pkt, &was_lost));
  assert(was_lost);
  assert(!sfu_twcc_history_consume_received(&history, 100, &pkt, &was_lost));
}

static void test_received_blocks_stale_loss(void) {
  sfu_twcc_history_t history;
  gcc_packet_info_t pkt = {0};
  sfu_twcc_history_init(&history);
  sfu_twcc_history_record(&history, 101, 3000, 800);

  assert(sfu_twcc_history_consume_received(&history, 101, &pkt, NULL));
  assert(!sfu_twcc_history_report_loss_once(&history, 101));
}

static void test_reorder_overlap_and_overwrite(void) {
  sfu_twcc_history_t history;
  gcc_packet_info_t pkt = {0};
  sfu_twcc_history_init(&history);
  sfu_twcc_history_record(&history, 200, 4000, 700);
  sfu_twcc_history_record(&history, 201, 5000, 700);
  sfu_twcc_history_record(&history, 202, 6000, 700);

  assert(sfu_twcc_history_consume_received(&history, 201, &pkt, NULL));
  assert(sfu_twcc_history_consume_received(&history, 200, &pkt, NULL));
  assert(!sfu_twcc_history_consume_received(&history, 201, &pkt, NULL));
  assert(sfu_twcc_history_consume_received(&history, 202, &pkt, NULL));

  uint16_t old_seq = 300;
  uint16_t new_seq = (uint16_t)(old_seq + SFU_TWCC_HISTORY_CAPACITY);
  sfu_twcc_history_record(&history, old_seq, 7000, 600);
  sfu_twcc_history_record(&history, new_seq, 8000, 500);
  assert(!sfu_twcc_history_consume_received(&history, old_seq, &pkt, NULL));
  assert(sfu_twcc_history_consume_received(&history, new_seq, &pkt, NULL));
}

static void test_rerecord_and_sequence_wrap(void) {
  sfu_twcc_history_t history;
  gcc_packet_info_t pkt = {0};
  sfu_twcc_history_init(&history);

  sfu_twcc_history_record(&history, 65535, 9000, 400);
  sfu_twcc_history_record(&history, 0, 10000, 300);
  assert(sfu_twcc_history_report_loss_once(&history, 65535));
  assert(sfu_twcc_history_consume_received(&history, 0, &pkt, NULL));

  sfu_twcc_history_record(&history, 0, 11000, 200);
  assert(sfu_twcc_history_report_loss_once(&history, 0));
}

int main(void) {
  test_received_consumes_once();
  test_loss_then_late_receive();
  test_received_blocks_stale_loss();
  test_reorder_overlap_and_overwrite();
  test_rerecord_and_sequence_wrap();
  printf("test_twcc_history: OK\n");
  return 0;
}
