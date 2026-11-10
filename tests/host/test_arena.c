/* Host tests for rt/rt_arena.c. Built with ASan + UBSan by `make test`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_arena.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

struct fixture {
    struct rt_arena a;
    void *mem;
    void *meta;
    size_t npages;
};

static void setup(struct fixture *f, size_t npages)
{
    f->npages = npages;
    f->mem = aligned_alloc(RT_PAGE_SIZE, npages * RT_PAGE_SIZE);
    f->meta = malloc(rt_arena_meta_bytes(npages));
    CHECK(f->mem && f->meta);
    CHECK(rt_arena_init(&f->a, f->mem, npages * RT_PAGE_SIZE, f->meta,
                        rt_arena_meta_bytes(npages)) == 0);
}

static void teardown(struct fixture *f)
{
    free(f->mem);
    free(f->meta);
}

static void test_init_rejects_bad_arguments(void)
{
    struct rt_arena a;
    void *mem = aligned_alloc(RT_PAGE_SIZE, 4 * RT_PAGE_SIZE);
    void *meta = malloc(rt_arena_meta_bytes(4));

    CHECK(rt_arena_init(&a, NULL, 4 * RT_PAGE_SIZE, meta, rt_arena_meta_bytes(4)) == -1);
    CHECK(rt_arena_init(&a, mem, 4 * RT_PAGE_SIZE, NULL, rt_arena_meta_bytes(4)) == -1);
    CHECK(rt_arena_init(&a, (char *)mem + 8, 3 * RT_PAGE_SIZE, meta, rt_arena_meta_bytes(4)) == -1);
    CHECK(rt_arena_init(&a, mem, RT_PAGE_SIZE - 1, meta, rt_arena_meta_bytes(4)) == -1);
    CHECK(rt_arena_init(&a, mem, 4 * RT_PAGE_SIZE, meta, rt_arena_meta_bytes(4) - 1) == -1);

    /* A trailing partial page is ignored, not used. */
    CHECK(rt_arena_init(&a, mem, 3 * RT_PAGE_SIZE + 100, meta, rt_arena_meta_bytes(4)) == 0);
    CHECK(a.npages == 3);

    free(mem);
    free(meta);
}

static void test_alloc_free_basics(void)
{
    struct fixture f;

    setup(&f, 16);

    CHECK(rt_arena_alloc(&f.a, 0, 0) == NULL);

    void *p1 = rt_arena_alloc(&f.a, 1, 0); /* rounds up to one page */
    void *p2 = rt_arena_alloc(&f.a, RT_PAGE_SIZE + 1, 0);

    CHECK(p1 == f.mem);
    CHECK(p2 == (char *)f.mem + RT_PAGE_SIZE);
    CHECK(f.a.used_pages == 3);
    CHECK(rt_arena_free_pages(&f.a) == 13);
    CHECK(rt_arena_check(&f.a) == 0);

    /* Freeing must be by the start address of a live allocation. */
    CHECK(rt_arena_free(&f.a, (char *)p2 + RT_PAGE_SIZE) == -1);
    CHECK(rt_arena_free(&f.a, (char *)p2 + 1) == -1);
    CHECK(rt_arena_free(&f.a, (char *)f.mem - RT_PAGE_SIZE) == -1);
    CHECK(rt_arena_free(&f.a, (char *)f.mem + 16 * RT_PAGE_SIZE) == -1);

    CHECK(rt_arena_free(&f.a, p1) == 0);
    CHECK(rt_arena_free(&f.a, p1) == -1); /* double free */
    CHECK(rt_arena_free(&f.a, p2) == 0);
    CHECK(f.a.used_pages == 0);
    CHECK(f.a.peak_pages == 3);
    CHECK(rt_arena_largest_free(&f.a) == 16);

    teardown(&f);
}

static void test_exhaustion_and_reuse(void)
{
    struct fixture f;
    void *p[8];

    setup(&f, 8);

    CHECK(rt_arena_alloc(&f.a, 9 * RT_PAGE_SIZE, 0) == NULL);

    for (int i = 0; i < 8; i++) {
        p[i] = rt_arena_alloc(&f.a, RT_PAGE_SIZE, 0);
        CHECK(p[i] != NULL);
    }
    CHECK(rt_arena_alloc(&f.a, RT_PAGE_SIZE, 0) == NULL);

    /* Free pages 2,3 and 5: the longest run is 2, so 3 pages must not fit. */
    CHECK(rt_arena_free(&f.a, p[2]) == 0);
    CHECK(rt_arena_free(&f.a, p[3]) == 0);
    CHECK(rt_arena_free(&f.a, p[5]) == 0);
    CHECK(rt_arena_largest_free(&f.a) == 2);
    CHECK(rt_arena_alloc(&f.a, 3 * RT_PAGE_SIZE, 0) == NULL);

    void *q = rt_arena_alloc(&f.a, 2 * RT_PAGE_SIZE, 0);

    CHECK(q == p[2]); /* first fit lands in the freed hole */
    CHECK(rt_arena_check(&f.a) == 0);

    /* Freeing page 4 joins it with page 5 into a run of two. */
    CHECK(rt_arena_free(&f.a, p[4]) == 0);
    CHECK(rt_arena_largest_free(&f.a) == 2);
    CHECK(rt_arena_alloc(&f.a, 2 * RT_PAGE_SIZE, 0) == p[4]);

    teardown(&f);
}

static void test_alignment(void)
{
    struct fixture f;

    setup(&f, 64);

    void *one = rt_arena_alloc(&f.a, RT_PAGE_SIZE, 0);
    void *big = rt_arena_alloc(&f.a, RT_PAGE_SIZE, 16 * RT_PAGE_SIZE);

    CHECK(one != NULL && big != NULL);
    CHECK(((uintptr_t)big & (16 * RT_PAGE_SIZE - 1)) == 0);

    CHECK(rt_arena_alloc(&f.a, RT_PAGE_SIZE, 3 * RT_PAGE_SIZE) == NULL); /* not a power of two */
    CHECK(rt_arena_alloc(&f.a, RT_PAGE_SIZE, 4096) != NULL);            /* below a page: page */
    CHECK(rt_arena_alloc(&f.a, RT_PAGE_SIZE, (size_t)1 << 40) == NULL); /* cannot be met */
    CHECK(rt_arena_check(&f.a) == 0);

    teardown(&f);
}

static void test_selftest(void)
{
    static const size_t sizes[] = {1, 7, 64, 1000, 4096};

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        struct fixture f;

        setup(&f, sizes[s]);
        for (uint32_t seed = 1; seed <= 5; seed++) {
            int rc = rt_arena_selftest(&f.a, seed * 2654435761u, 5000);

            if (rc) {
                printf("FAIL selftest pages=%zu seed=%u rc=%d\n", sizes[s], seed, rc);
                failures++;
            }
        }
        CHECK(f.a.used_pages == 0);
        teardown(&f);
    }

    /* The self-test must refuse to run on an arena that is in use. */
    struct fixture f;

    setup(&f, 8);
    CHECK(rt_arena_alloc(&f.a, RT_PAGE_SIZE, 0) != NULL);
    CHECK(rt_arena_selftest(&f.a, 1, 10) == -10);
    teardown(&f);
}

static void test_check_detects_corruption(void)
{
    struct fixture f;

    setup(&f, 16);

    void *p = rt_arena_alloc(&f.a, 4 * RT_PAGE_SIZE, 0);

    CHECK(p != NULL);
    CHECK(rt_arena_check(&f.a) == 0);

    f.a.used_pages++;
    CHECK(rt_arena_check(&f.a) != 0);
    f.a.used_pages--;

    f.a.runlen[2] = 1; /* a second allocation claiming a page inside the first */
    CHECK(rt_arena_check(&f.a) != 0);
    f.a.runlen[2] = 0;

    f.a.bitmap[0] &= ~(uint64_t)2; /* page 1 marked free while still allocated */
    CHECK(rt_arena_check(&f.a) != 0);

    teardown(&f);
}

int main(void)
{
    test_init_rejects_bad_arguments();
    test_alloc_free_basics();
    test_exhaustion_and_reuse();
    test_alignment();
    test_selftest();
    test_check_detects_corruption();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("rt_arena: all host tests passed\n");
    return 0;
}
