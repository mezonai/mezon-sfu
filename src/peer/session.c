#include "peer/session.h"
#include "util/log.h"

#include <string.h>

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

static bool addr_equal(const struct sockaddr_storage *a, socklen_t a_len,
                        const struct sockaddr_storage *b, socklen_t b_len) {
    if (a_len != b_len) return false;
    return memcmp(a, b, a_len) == 0;
}

sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t,
                                                     const struct sockaddr_storage *addr,
                                                     socklen_t addr_len) {
    pthread_mutex_lock(&t->lock);

    for (uint32_t i = 0; i < t->count; i++) {
        if (t->sessions[i].active &&
            addr_equal(&t->sessions[i].addr, t->sessions[i].addr_len, addr, addr_len)) {
            pthread_mutex_unlock(&t->lock);
            return &t->sessions[i];
        }
    }

    if (t->count >= SFU_SESSION_TABLE_MAX) {
        SFU_LOG_WARN("session table full (%u), rejecting new peer", SFU_SESSION_TABLE_MAX);
        pthread_mutex_unlock(&t->lock);
        return NULL;
    }

    sfu_peer_session_t *s = &t->sessions[t->count++];
    memset(s, 0, sizeof(*s));
    memcpy(&s->addr, addr, addr_len);
    s->addr_len = addr_len;
    s->active    = true;
    s->state     = SFU_SESSION_NEW;

    if (sfu_dtls_conn_init(&s->dtls, t->dtls_ctx) != 0) {
        SFU_LOG_ERROR("failed to init DTLS connection for new peer session");
        s->active = false;
        t->count--;
        pthread_mutex_unlock(&t->lock);
        return NULL;
    }

    pthread_mutex_unlock(&t->lock);
    return s;
}

sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t,
                                            const struct sockaddr_storage *addr,
                                            socklen_t addr_len) {
    pthread_mutex_lock(&t->lock);
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->sessions[i].active &&
            addr_equal(&t->sessions[i].addr, t->sessions[i].addr_len, addr, addr_len)) {
            pthread_mutex_unlock(&t->lock);
            return &t->sessions[i];
        }
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}
