#include "runtime/routing_context.h"
#include <string.h>
#include "util/log.h"

void sfu_routing_table_init(sfu_routing_table_t *table) {
  if (!table) {
    return;
  }

  memset(table->entries, 0, sizeof(table->entries));
  table->count = 0;

  pthread_mutex_init(&table->mutex, NULL);
}

void sfu_routing_table_set_pending_answer(sfu_routing_table_t *table, const char *client_ufrag, uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                          uint8_t video_pt, uint8_t rtx_pt) {
  pthread_mutex_lock(&table->mutex);
  for (int i = 0; i < table->count; i++) {
    if (strcmp(table->entries[i].ufrag, client_ufrag) == 0) {
      table->entries[i].pending_audio_ssrc = audio_ssrc;
      table->entries[i].pending_video_ssrc = video_ssrc;
      table->entries[i].pending_rtx_ssrc = rtx_ssrc;
      table->entries[i].pending_video_pt = video_pt;
      table->entries[i].pending_rtx_pt = rtx_pt;
      table->entries[i].has_pending_answer = true;
      pthread_mutex_unlock(&table->mutex);
      return;
    }
  }
  SFU_LOG_WARN("signaling: no routing entry for ufrag=%s to attach pending answer", client_ufrag);
  pthread_mutex_unlock(&table->mutex);
}

void sfu_routing_table_unregister_fd(sfu_routing_table_t *table, int fd) {
  if (!table) return;
  pthread_mutex_lock(&table->mutex);
  for (int i = 0; i < table->count;) {
    if (table->entries[i].fd == fd) {
      table->entries[i] = table->entries[--table->count];
      memset(&table->entries[table->count], 0, sizeof(table->entries[0]));
    } else i++;
  }
  pthread_mutex_unlock(&table->mutex);
}
