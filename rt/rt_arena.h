/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Page-granular arena allocator.
 *
 * m1n1 already provides malloc() for small objects. This allocator exists for
 * a different job: buffers that hardware will see (DART mappings, firmware
 * shared memory, GPU buffers). Those need 16 KiB page alignment, a stable
 * physical address, and an owner that can say exactly which pages are in use.
 *
 * The code is freestanding C with no m1n1 dependencies, so the same file is
 * compiled natively for the host tests in tests/host/.
 */
#ifndef RT_ARENA_H
#define RT_ARENA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RT_PAGE_SHIFT 14 /* 16 KiB: the Apple Silicon CPU and DART page size */
#define RT_PAGE_SIZE  ((size_t)1 << RT_PAGE_SHIFT)

struct rt_arena {
    uintptr_t base;    /* first byte managed, page aligned */
    size_t npages;     /* pages managed */
    size_t used_pages; /* pages currently allocated */
    size_t peak_pages; /* high-water mark of used_pages */
    uint64_t *bitmap;  /* bit set = page in use */
    uint32_t *runlen;  /* runlen[first page of an allocation] = its page count */
};

/* Bytes of bookkeeping memory needed to manage npages pages. */
size_t rt_arena_meta_bytes(size_t npages);

/*
 * Manage [base, base + bytes). base must be page aligned; a trailing partial
 * page is ignored. meta must point to at least rt_arena_meta_bytes() bytes
 * that stay valid for the arena's lifetime. Returns 0, or -1 on bad arguments.
 */
int rt_arena_init(struct rt_arena *a, void *base, size_t bytes, void *meta, size_t meta_bytes);

/*
 * Allocate bytes rounded up to whole pages. align is a power of two in bytes
 * applied to the absolute address; 0 means one page. Returns NULL when no run
 * fits. The memory is not cleared.
 */
void *rt_arena_alloc(struct rt_arena *a, size_t bytes, size_t align);

/* Free an allocation by its start address. Returns 0, or -1 if ptr is not the
 * start of a live allocation (including a double free). */
int rt_arena_free(struct rt_arena *a, void *ptr);

size_t rt_arena_free_pages(const struct rt_arena *a);
size_t rt_arena_largest_free(const struct rt_arena *a); /* longest free run, in pages */

/* Consistency check of the bookkeeping. Returns 0 if sound, negative otherwise. */
int rt_arena_check(const struct rt_arena *a);

/*
 * Deterministic stress test: random allocate / verify / free cycles that also
 * write to and read back every allocated page. The arena must be empty on
 * entry and is empty again on success. Returns 0, or a negative code naming
 * the check that failed.
 */
int rt_arena_selftest(struct rt_arena *a, uint32_t seed, unsigned rounds);

#endif
