#ifndef MEZON_PRODUCER_H
#define MEZON_PRODUCER_H

#include <nats/nats.h>
#include <uv.h>

natsStatus init_nats_connection(const char *nats_url, const char *client_name);
void cleanup_nats_connection();
bool dispatch_hook_event(const void *msg, int len);

#endif
