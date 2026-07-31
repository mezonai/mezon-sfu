#include "runtime/signal.h"
#include "util/log.h"

#include <signal.h>
#include <stdatomic.h>
#include <string.h>

static volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int signum) {
  (void)signum;
  /* async-signal-safe: only sets a flag, does no logging/allocation */
  g_shutdown = 1;
}

void sfu_install_shutdown_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  SFU_LOG_INFO("shutdown handler installed (SIGINT/SIGTERM)");
}

bool sfu_shutdown_requested(void) { return g_shutdown != 0; }

void sfu_request_shutdown(void) { g_shutdown = 1; }
