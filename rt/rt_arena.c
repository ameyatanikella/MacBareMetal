/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "rt_arena.h"

#include <string.h>

#define BITS_PER_WORD 64

static inline bool page_used(const struct rt_arena *a, size_t page)
{
    return (a->bitmap[page / BITS_PER_WORD] >> (page % BITS_PER_WORD)) & 1;
}

static inline void page_set(struct rt_arena *a, size_t page, bool used)
{
    uint64_t bit = (uint64_t)1 << (page % BITS_PER_WORD);

    if (used)
        a->bitmap[page / BITS_PER_WORD] |= bit;
    else
        a->bitmap[page / BITS_PER_WORD] &= ~bit;
}

static size_t bitmap_words(size_t npages)
{
    return (npages + BITS_PER_WORD - 1) / BITS_PER_WORD;
}

size_t rt_arena_meta_bytes(size_t npages)
{
    return bitmap_words(npages) * sizeof(uint64_t) + npages * sizeof(uint32_t);
}

int rt_arena_init(struct rt_arena *a, void *base, size_t bytes, void *meta, size_t meta_bytes)
{
    size_t npages = bytes >> RT_PAGE_SHIFT;

    if (!a || !base || !meta || !npages)
        return -1;
    if ((uintptr_t)base & (RT_PAGE_SIZE - 1))
        return -1;
    if ((uintptr_t)meta & (sizeof(uint64_t) - 1))
        return -1;
    if (npages > UINT32_MAX)
        return -1;
    if (meta_bytes < rt_arena_meta_bytes(npages))
        return -1;

    memset(meta, 0, rt_arena_meta_bytes(npages));

    a->base = (uintptr_t)base;
    a->npages = npages;
    a->used_pages = 0;
    a->peak_pages = 0;
    a->bitmap = meta;
    a->runlen = (uint32_t *)((uint8_t *)meta + bitmap_words(npages) * sizeof(uint64_t));
    return 0;
}

void *rt_arena_alloc(struct rt_arena *a, size_t bytes, size_t align)
{
    if (!bytes || bytes > SIZE_MAX - RT_PAGE_SIZE)
        return NULL;

    size_t need = (bytes + RT_PAGE_SIZE - 1) >> RT_PAGE_SHIFT;

    if (need > a->npages - a->used_pages)
        return NULL;

    if (align < RT_PAGE_SIZE)
        align = RT_PAGE_SIZE;
    if (align & (align - 1))
        return NULL;

    size_t start = 0;

    while (1) {
        /* Round the candidate up so that its absolute address is aligned. */
        uintptr_t addr = a->base + (start << RT_PAGE_SHIFT);
        uintptr_t aligned = (addr + align - 1) & ~(uintptr_t)(align - 1);

        if (aligned < addr)
            return NULL; /* wrapped */
        start += (aligned - addr) >> RT_PAGE_SHIFT;

        if (start >= a->npages || need > a->npages - start)
            return NULL;

        size_t i;
        for (i = 0; i < need; i++)
            if (page_used(a, start + i))
                break;

        if (i == need)
            break;

        /* Page start + i is taken: no run starting at or before it can fit. */
        start += i + 1;
    }

    for (size_t i = 0; i < need; i++)
        page_set(a, start + i, true);

    a->runlen[start] = (uint32_t)need;
    a->used_pages += need;
    if (a->used_pages > a->peak_pages)
        a->peak_pages = a->used_pages;

    return (void *)(a->base + (start << RT_PAGE_SHIFT));
}

int rt_arena_free(struct rt_arena *a, void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;

    if (addr < a->base || (addr & (RT_PAGE_SIZE - 1)))
        return -1;

    size_t start = (addr - a->base) >> RT_PAGE_SHIFT;

    if (start >= a->npages)
        return -1;

    size_t n = a->runlen[start];

    if (!n || n > a->npages - start)
        return -1;

    for (size_t i = 0; i < n; i++)
        page_set(a, start + i, false);

    a->runlen[start] = 0;
    a->used_pages -= n;
    return 0;
}

size_t rt_arena_free_pages(const struct rt_arena *a)
{
    return a->npages - a->used_pages;
}

size_t rt_arena_largest_free(const struct rt_arena *a)
{
    size_t best = 0, run = 0;

    for (size_t i = 0; i < a->npages; i++) {
        if (page_used(a, i)) {
            run = 0;
        } else if (++run > best) {
            best = run;
        }
    }
    return best;
}

int rt_arena_check(const struct rt_arena *a)
{
    size_t used_bits = 0, run_total = 0;

    for (size_t i = 0; i < a->npages; i++)
        used_bits += page_used(a, i);

    if (used_bits != a->used_pages)
        return -1;

    for (size_t i = 0; i < a->npages; i++) {
        size_t n = a->runlen[i];

        if (!n)
            continue;
        if (n > a->npages - i)
            return -2;
        for (size_t j = 0; j < n; j++) {
            if (!page_used(a, i + j))
                return -3;
            if (j && a->runlen[i + j])
                return -4; /* allocations overlap */
        }
        run_total += n;
        i += n - 1;
    }

    if (run_total != a->used_pages)
        return -5;

    return 0;
}

/* --- self-test --------------------------------------------------------- */

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

#define SELFTEST_SLOTS 32

struct selftest_slot {
    uint64_t *ptr;
    size_t pages;
    uint64_t tag;
};

/* Each page carries its owner's tag and its index at both ends, so an overlap
 * between two allocations or a page that does not hold data shows up. */
static void stamp(const struct selftest_slot *s)
{
    size_t words = RT_PAGE_SIZE / sizeof(uint64_t);

    for (size_t p = 0; p < s->pages; p++) {
        uint64_t *page = s->ptr + p * words;

        page[0] = s->tag ^ p;
        page[words - 1] = ~(s->tag ^ p);
    }
}

static bool verify(const struct selftest_slot *s)
{
    size_t words = RT_PAGE_SIZE / sizeof(uint64_t);

    for (size_t p = 0; p < s->pages; p++) {
        const uint64_t *page = s->ptr + p * words;

        if (page[0] != (s->tag ^ p) || page[words - 1] != ~(s->tag ^ p))
            return false;
    }
    return true;
}

int rt_arena_selftest(struct rt_arena *a, uint32_t seed, unsigned rounds)
{
    static const size_t aligns[] = {1, 1, 2, 4, 16};
    struct selftest_slot slots[SELFTEST_SLOTS];
    uint32_t rng = seed ? seed : 1;
    size_t max_pages = a->npages / 8;

    if (a->used_pages)
        return -10;
    if (!max_pages)
        max_pages = 1;
    if (max_pages > 64)
        max_pages = 64;

    memset(slots, 0, sizeof(slots));

    for (unsigned r = 0; r < rounds; r++) {
        struct selftest_slot *s = &slots[xorshift32(&rng) % SELFTEST_SLOTS];

        if (s->ptr) {
            if (!verify(s))
                return -11;
            if (rt_arena_free(a, s->ptr))
                return -12;
            if (!rt_arena_free(a, s->ptr))
                return -13; /* a double free must be refused */
            s->ptr = NULL;
            continue;
        }

        size_t pages = 1 + xorshift32(&rng) % max_pages;
        size_t align = aligns[xorshift32(&rng) % (sizeof(aligns) / sizeof(aligns[0]))];
        void *p = rt_arena_alloc(a, pages << RT_PAGE_SHIFT, align << RT_PAGE_SHIFT);

        if (!p)
            continue; /* legitimately full or fragmented */
        if ((uintptr_t)p & ((align << RT_PAGE_SHIFT) - 1))
            return -14;
        if ((uintptr_t)p < a->base ||
            (uintptr_t)p + (pages << RT_PAGE_SHIFT) > a->base + (a->npages << RT_PAGE_SHIFT))
            return -15;

        s->ptr = p;
        s->pages = pages;
        s->tag = ((uint64_t)xorshift32(&rng) << 32) | r;
        stamp(s);

        if ((r & 63) == 0 && rt_arena_check(a))
            return -16;
    }

    for (size_t i = 0; i < SELFTEST_SLOTS; i++) {
        if (!slots[i].ptr)
            continue;
        if (!verify(&slots[i]))
            return -17;
        if (rt_arena_free(a, slots[i].ptr))
            return -18;
    }

    if (a->used_pages)
        return -19;
    if (rt_arena_largest_free(a) != a->npages)
        return -20;
    if (rt_arena_check(a))
        return -21;

    return 0;
}
