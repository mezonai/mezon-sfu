#include "runtime/signal.h"
#include "util/log.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown = 0;
static const char *g_crash_log_path = "mezon-sfu-crash.log";

static void crash_write(int fd, const char *s) {
  if (fd < 0 || !s) {
    return;
  }
  size_t n = strlen(s);
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) {
      break;
    }
    s += (size_t)w;
    n -= (size_t)w;
  }
}

static void crash_write_hex(int fd, unsigned long v) {
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 15; i >= 0; i--) {
    unsigned nibble = (unsigned)((v >> (unsigned)(i * 4)) & 0xfu);
    buf[2 + (15 - i)] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
  }
  buf[18] = '\0';
  crash_write(fd, buf);
}

static void crash_write_dec(int fd, long v) {
  char buf[32];
  int i = (int)sizeof(buf);
  buf[--i] = '\0';
  unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
  if (u == 0) {
    buf[--i] = '0';
  }
  while (u > 0 && i > 1) {
    buf[--i] = (char)('0' + (u % 10));
    u /= 10;
  }
  if (v < 0) {
    buf[--i] = '-';
  }
  crash_write(fd, buf + i);
}

static const char *crash_signame(int signum) {
  switch (signum) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGABRT:
      return "SIGABRT";
    case SIGFPE:
      return "SIGFPE";
    case SIGILL:
      return "SIGILL";
    case SIGBUS:
      return "SIGBUS";
    default:
      return "SIGNAL";
  }
}

static void dump_crash(int fd, int signum, siginfo_t *info) {
  crash_write(fd, "=== mezon-sfu crash ===\n");
  crash_write(fd, "signal=");
  crash_write(fd, crash_signame(signum));
  crash_write(fd, " (");
  crash_write_dec(fd, signum);
  crash_write(fd, ")\n");
  crash_write(fd, "pid=");
  crash_write_dec(fd, (long)getpid());
  crash_write(fd, " tid=");
  crash_write_dec(fd, (long)gettid());
  crash_write(fd, "\n");
  if (info) {
    crash_write(fd, "si_code=");
    crash_write_dec(fd, info->si_code);
    crash_write(fd, " si_addr=");
    crash_write_hex(fd, (unsigned long)info->si_addr);
    crash_write(fd, "\n");
  }
  crash_write(fd, "backtrace:\n");
  void *frames[64];
  int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, fd);
  crash_write(fd, "=== end crash ===\n");
}

static void on_crash(int signum, siginfo_t *info, void *ucontext) {
  (void)ucontext;
  int fd = open(g_crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    dump_crash(fd, signum, info);
    close(fd);
  }
  dump_crash(STDERR_FILENO, signum, info);
  /* SA_RESETHAND already restored the default handler; re-raise for coredump. */
  raise(signum);
}

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

  struct sigaction ca;
  memset(&ca, 0, sizeof(ca));
  ca.sa_sigaction = on_crash;
  sigemptyset(&ca.sa_mask);
  ca.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGSEGV, &ca, NULL);
  sigaction(SIGABRT, &ca, NULL);
  sigaction(SIGFPE, &ca, NULL);
  sigaction(SIGILL, &ca, NULL);
  sigaction(SIGBUS, &ca, NULL);

  SFU_LOG_INFO("shutdown handler installed (SIGINT/SIGTERM); crash log=%s", g_crash_log_path);
}

bool sfu_shutdown_requested(void) { return g_shutdown != 0; }

void sfu_request_shutdown(void) { g_shutdown = 1; }
