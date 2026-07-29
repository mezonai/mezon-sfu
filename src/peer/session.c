#include "peer/session.h"
#include <string.h>
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "util/alloc.h"
#include "util/log.h"

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx) {
  memset(t, 0, sizeof(*t));
  t->capacity = SFU_SESSION_TABLE_MAX;
  t->dtls_ctx = dtls_ctx;

  if (pthread_mutex_init(&t->lock, NULL) != 0) {
    return -1;
  }

  return 0;
}

void sfu_session_table_destroy(sfu_session_table_t *t) {
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i]->active) {
      if (t->sessions[i]->state == SFU_SESSION_ESTABLISHED) {
        sfu_srtp_ctx_destroy(&t->sessions[i]->srtp);
      }
      sfu_dtls_conn_destroy(&t->sessions[i]->dtls);
    }
  }

  for (uint32_t i = 0; i < t->count; i++) {
    SFU_FREE(t->sessions[i]);
    t->sessions[i] = NULL;
  }

  t->count = 0;

  pthread_mutex_unlock(&t->lock);
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

  int free_index = -1;

  for (uint32_t i = 0; i < t->count; i++) {
    sfu_peer_session_t *session = t->sessions[i];

    if (!session) {
      if (free_index < 0) {
        free_index = (int)i;
      }
      continue;
    }

    if (session->active && addr_equal(&session->addr, session->addr_len, addr, addr_len)) {
      pthread_mutex_unlock(&t->lock);
      return session;
    }

    if (!session->active && free_index < 0) {
      free_index = (int)i;
    }
  }

  uint32_t index;

  if (free_index >= 0) {
    index = (uint32_t)free_index;

    if (t->sessions[index] == NULL) {
      t->sessions[index] = SFU_CALLOC(1, sizeof(sfu_peer_session_t));
      if (!t->sessions[index]) {
        pthread_mutex_unlock(&t->lock);
        return NULL;
      }
    }
  } else if (t->count < t->capacity) {
    index = t->count++;

    t->sessions[index] = SFU_CALLOC(1, sizeof(sfu_peer_session_t));
    if (!t->sessions[index]) {
      t->count--;
      pthread_mutex_unlock(&t->lock);
      return NULL;
    }
  } else {
    SFU_LOG_WARN("session table full (%u), rejecting new peer", t->capacity);
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  sfu_peer_session_t *s = t->sessions[index];

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

    SFU_FREE(s);
    t->sessions[index] = NULL;

    if (index + 1 == t->count) {
      t->count--;
    }

    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  pthread_mutex_unlock(&t->lock);
  return s;
}

sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  pthread_mutex_lock(&t->lock);
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i]->active && addr_equal(&t->sessions[i]->addr, t->sessions[i]->addr_len, addr, addr_len)) {
      pthread_mutex_unlock(&t->lock);
      return t->sessions[i];
    }
  }
  pthread_mutex_unlock(&t->lock);
  return NULL;
}

sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag) {
  if (!ufrag || ufrag[0] == '\0') {
    return NULL;
  }

  pthread_mutex_lock(&t->lock);
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i]->active && t->sessions[i]->ufrag[0] != '\0' && strcmp(t->sessions[i]->ufrag, ufrag) == 0) {
      pthread_mutex_unlock(&t->lock);
      return t->sessions[i];
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

void sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len) {
  pthread_mutex_lock(&t->lock);
  memcpy(&s->addr, addr, addr_len);
  s->addr_len = addr_len;
  pthread_mutex_unlock(&t->lock);
}
