#ifndef SFU_UTIL_ALLOC_H
#define SFU_UTIL_ALLOC_H

#include <mimalloc.h>

/*
 * All heap allocation in this codebase goes through these macros
 * rather than calling mi_* directly, so the allocator is swappable
 * from one place. mimalloc chosen for the same reason it's used
 * elsewhere in the mezon stack: better multi-threaded allocation
 * behavior than glibc malloc under the kind of concurrent alloc/free
 * churn the packet pools and fan-out mesh produce.
 *
 * Note the argument order difference from libc's aligned_alloc(align,
 * size): mimalloc's mi_malloc_aligned takes (size, alignment).
 */
#define SFU_MALLOC(size)                 mi_malloc(size)
#define SFU_CALLOC(count, size)          mi_calloc(count, size)
#define SFU_ALIGNED_ALLOC(align, size)   mi_malloc_aligned(size, align)
#define SFU_FREE(ptr)                    mi_free(ptr)

#endif /* SFU_UTIL_ALLOC_H */
