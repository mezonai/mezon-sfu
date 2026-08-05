#ifndef SFU_CONFIG_H
#define SFU_CONFIG_H

#define SFU_MAX_WORKERS 16
#define SFU_CACHELINE_SIZE 64

#define SFU_LIKELY(x) __builtin_expect(!!(x), 1)
#define SFU_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif /* SFU_CONFIG_H */
