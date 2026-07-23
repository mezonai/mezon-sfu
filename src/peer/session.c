#include "peer/session.h"
#include <string.h>
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx) {
  memset(t, 0, sizeof(*t));
  t->dtls_ctx = dtls_ctx;
  if (pthread_mutex_init(&t->lock, NULL) != 0) {
    return -1;
  }
  return 0;
}

void sfu_session_table_destroy(sfu_session_table_t *t) {
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i].active) {
      if (t->sessions[i].state == SFU_SESSION_ESTABLISHED) {
        sfu_srtp_ctx_destroy(&t->sessions[i].srtp);
      }
      sfu_dtls_conn_destroy(&t->sessions[i].dtls);
    }
  }
  pthread_mutex_destroy(&t->lock);
}

static bool addr_equal(const struct sockaddr_storage *a, socklen_t a_len, const struct sockaddr_storage *b, socklen_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  return memcmp(a, b, a_len) == 0;
}

sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  pthread_mutex_lock(&t->lock);

  sfu_peer_session_t *free_slot = NULL;
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i].active && addr_equal(&t->sessions[i].addr, t->sessions[i].addr_len, addr, addr_len)) {
      pthread_mutex_unlock(&t->lock);
      return &t->sessions[i];
    }
    if (!t->sessions[i].active && !free_slot) {
      free_slot = &t->sessions[i];
    }
  }

  sfu_peer_session_t *s;
  if (free_slot) {
    s = free_slot;
  } else if (t->count < SFU_SESSION_TABLE_MAX) {
    s = &t->sessions[t->count++];
  } else {
    SFU_LOG_WARN("session table full (%u), rejecting new peer", SFU_SESSION_TABLE_MAX);
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  memset(s, 0, sizeof(*s));
  memcpy(&s->addr, addr, addr_len);
  s->addr_len = addr_len;
  s->active = true;
  s->state = SFU_SESSION_NEW;
  s->worker_id = UINT16_MAX;
  for (int i = 0; i < 128; i++) {
    s->pt_map[i] = (uint8_t)i;
  }
  s->uplink_audio.owner = s;
  s->uplink_video.owner = s;
  s->screen.owner = s;

  if (sfu_dtls_conn_init(&s->dtls, t->dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS connection for new peer session");
    s->active = false;
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  pthread_mutex_unlock(&t->lock);
  return s;
}

sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  pthread_mutex_lock(&t->lock);
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i].active && addr_equal(&t->sessions[i].addr, t->sessions[i].addr_len, addr, addr_len)) {
      pthread_mutex_unlock(&t->lock);
      return &t->sessions[i];
    }
  }
  pthread_mutex_unlock(&t->lock);
  return NULL;
}


void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s) {
  pthread_mutex_lock(&t->lock);
  if (s->active) {
    if (s->state == SFU_SESSION_ESTABLISHED) {
      sfu_srtp_ctx_destroy(&s->srtp);
    }
    sfu_dtls_conn_destroy(&s->dtls);
    s->active = false;
    s->state = SFU_SESSION_FAILED;
  }
  pthread_mutex_unlock(&t->lock);
}
