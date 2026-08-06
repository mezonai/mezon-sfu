#include "pipeline/egress.h"

#include "congestion/twcc_history.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtx.h"
#include "runtime/fanout.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"
#include "util/metrics.h"
#include "util/netbytes.h"

/* Local egress: runs on the worker that owns `sub_session` (its SRTP context,
 * pacer and TWCC sequence are not thread-safe across workers). */
static void sfu_egress_process_local(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst,
                                     socklen_t dst_len, uint32_t video_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, uint32_t video_rtx_ssrc,
                                     bool has_video, bool is_audio, sfu_pacer_class_t video_class) {
  int enc_len = (int)pkt->len;

  uint8_t incoming_pt = pkt->data[1] & 0x7F;
  uint8_t expected_pt = sfu_session_get_mapped_pt(sub_session, incoming_pt);
  if (incoming_pt != expected_pt) {
    pkt->data[1] = (pkt->data[1] & 0x80) | (expected_pt & 0x7F);
  }

  int64_t send_time_us = (int64_t)sfu_now_us();
  sfu_pacer_class_t cls = is_audio ? SFU_PACER_CLASS_AUDIO : (has_video ? video_class : SFU_PACER_CLASS_VIDEO_BASE);
  if (!sfu_pacer_should_send(&sub_session->pacer, cls, (uint32_t)enc_len + 10 /* SRTP auth tag */, &send_time_us)) {
    sfu_metric_inc("pacer_dropped_enh");
    return;
  }

  uint16_t subscriber_seq = sfu_read_be16(pkt->data + 2);
  if (has_video && incoming_pt == video_pt && sub_session->rtx_cache) {
    sfu_rtx_cache_put_stream(sub_session->rtx_cache, subscriber_seq, pkt->data, (uint32_t)enc_len, video_rtx_ssrc, video_rtx_pt, video_ssrc,
                             atomic_load_explicit(&sub_session->egress_generation, memory_order_acquire));
  }

  if (sub_session->twcc_extmap_id != 0) {
    uint16_t twcc_seq = __atomic_fetch_add(&sub_session->next_twcc_seq, 1, __ATOMIC_RELAXED);
    size_t new_len = (size_t)enc_len;
    if (sfu_rtp_ext_write_twcc(pkt->data, (size_t)enc_len, pkt->cap, sub_session->twcc_extmap_id, twcc_seq, &new_len)) {
      enc_len = (int)new_len;
      if (sub_session->twcc_history) {
        sfu_twcc_history_record(sub_session->twcc_history, twcc_seq, send_time_us, (uint32_t)enc_len);
      }
    } else {
      sfu_metric_inc("twcc_write_fail");
    }
  }

  if (!sfu_srtp_protect_rtp(&sub_session->srtp, pkt->data, &enc_len, pkt->cap)) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] SRTP protect FAILED", w->worker_index);
    sfu_metric_inc("egress_protect_fail");
    return;
  }
  pkt->len = (uint32_t)enc_len;

  if (sfu_ring_queue_send_zc(&w->send_ring, pkt, (const struct sockaddr *)dst, dst_len) != 0) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] send SQ full", w->worker_index);
    sfu_metric_inc("egress_send_full");
  }
}

void sfu_egress_process(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                        uint32_t video_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, uint32_t video_rtx_ssrc, bool has_video, bool is_audio,
                        sfu_pacer_class_t video_class) {
  if (sub_session->worker_id == w->worker_index) {
    sfu_egress_process_local(w, sub_session, pkt, dst, dst_len, video_ssrc, video_pt, video_rtx_pt, video_rtx_ssrc, has_video, is_audio,
                             video_class);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  atomic_fetch_add_explicit(&sub_session->refcount, 1, memory_order_relaxed);
  if (!sfu_fanout_mesh_enqueue_forward(w->mesh, w->worker_index, sub_session->worker_id, pkt, sub_session, dst, dst_len, video_ssrc, video_rtx_ssrc,
                                       video_pt, video_rtx_pt, has_video, is_audio, (uint8_t)video_class)) {
    SFU_LOG_WARN("worker %u: fanout queue full", w->worker_index);
    sfu_session_release(sub_session);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
  }
}
