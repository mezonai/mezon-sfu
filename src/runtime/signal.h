#ifndef SFU_RUNTIME_SIGNAL_H
#define SFU_RUNTIME_SIGNAL_H

#include <stdbool.h>

/* Installs SIGINT/SIGTERM handlers that flip an atomic flag rather than
 * doing any real work in signal context. Every worker/dispatcher loop
 * polls sfu_shutdown_requested() once per batch and exits cleanly. */
void sfu_install_shutdown_handler(void);
bool sfu_shutdown_requested(void);

#endif /* SFU_RUNTIME_SIGNAL_H */
