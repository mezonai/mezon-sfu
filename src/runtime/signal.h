#ifndef SFU_RUNTIME_SIGNAL_H
#define SFU_RUNTIME_SIGNAL_H

#include <stdbool.h>

void sfu_install_shutdown_handler(void);
bool sfu_shutdown_requested(void);
void sfu_request_shutdown(void);

#endif /* SFU_RUNTIME_SIGNAL_H */
