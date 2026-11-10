/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Diagnostic stages for milestone 1.
 *
 * Risk ladder (see docs/TESTING.md):
 *   rung 1  ident, cpu, exceptions, memory, devtree
 *           Read RAM, CPU registers and the device tree iBoot left in RAM.
 *           The only writes are to our own memory and the framebuffer.
 *   rung 2  watchdog hang guard (RT_WDT_GUARD=1) - writes the watchdog block.
 *   rung 3  nvme (RT_NVME_PROBE=1) - runs m1n1's NVMe driver, read commands
 *           only, on a storage controller m1n1 has not been reported to
 *           support on this SoC.
 *   rung 4  gpu reads (RT_GPU_PROBE=1) - guarded reads of GPU register ranges.
 *   rung 5  gpu power (RT_GPU_POWER=1) - switches the GPU power gates on and
 *           off through m1n1's power manager.
 *
 * Nothing here hard-codes an M5 Pro address. Every address is read from the
 * device tree at run time and printed, so the first boot tells us what is
 * true instead of confirming what we assumed from M1/M2.
 */

#include "rt.h"

#include "adt.h"
#include "heapblock.h"
#include "malloc.h"
#include "midr.h"
#include "nvme.h"
#include "pmgr.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

extern const char *const m1n1_version;

static const char *str_or(const char *s, const char *fallback)
{
    return s ? s : fallback;
}

/* --- stage: ident ------------------------------------------------------ */

static const char *core_name(u32 part)
{
    switch (part) {
        case MIDR_PART_T6050_SOTRA_MCORE:
            return "M5 Pro M-core";
        case MIDR_PART_T6050_SOTRA_PCORE:
            return "M5 Pro P-core";
        case MIDR_PART_T6051_SOTRAC_MCORE:
            return "M5 Max M-core";
        case MIDR_PART_T6051_SOTRAC_PCORE:
            return "M5 Max P-core";
        case MIDR_PART_T8142_HIDRA_ECORE:
            return "M5 E-core";
        case MIDR_PART_T8142_HIDRA_PCORE:
            return "M5 P-core";
        default:
            return "not an M5-family core known to this build";
    }
}

enum rt_result diag_ident(void)
{
    int chosen = adt_path_offset(adt, "/chosen");
    const char *model = adt_getprop(adt, 0, "model", NULL);
    const char *target = adt_getprop(adt, 0, "target-type", NULL);
    const char *iboot = chosen > 0 ? adt_getprop(adt, chosen, "firmware-version", NULL) : NULL;
    u64 midr = mrs(MIDR_EL1);
    u32 part = (midr >> 4) & 0xfff;
    const struct boot_args *ba = &cur_boot_args;

    rt_printf("model         %s\n", str_or(model, "?"));
    rt_printf("target-type   %s\n", str_or(target, "?"));
    rt_printf("chip-id       0x%x     board-id 0x%x\n", chip_id, board_id);
    rt_printf("iBoot         %s\n", str_or(iboot, "?"));
    rt_printf("m1n1          %s\n", m1n1_version);
    rt_printf("boot core     MIDR 0x%lx part 0x%x (%s)\n", midr, part, core_name(part));
    rt_printf("              MPIDR 0x%lx\n", mrs(MPIDR_EL1));
    rt_printf("\n");
    rt_printf("boot args     revision %d version %d\n", ba->revision, ba->version);
    rt_printf("phys_base     0x%lx\n", ba->phys_base);
    rt_printf("mem_size      0x%lx (%lu MiB usable from phys_base)\n", ba->mem_size,
              ba->mem_size >> 20);
    rt_printf("mem_actual    0x%lx (%lu GiB installed)\n", mem_size_actual, mem_size_actual >> 30);
    rt_printf("top_of_kdata  0x%lx\n", ba->top_of_kernel_data);
    rt_printf("devtree       %p, 0x%x bytes\n", adt, ba->devtree_size);
    rt_printf("video         %lux%lu stride %lu depth 0x%lx\n", ba->video.width, ba->video.height,
              ba->video.stride, ba->video.depth);
    rt_printf("              base 0x%lx\n", ba->video.base);
    rt_printf("\n");

    if (chip_id == T6050) {
        rt_printf("This is T6050 (M5 Pro), the SoC this build was written for.\n");
        return RT_PASS;
    }

    rt_printf("NOT T6050. Nothing in this build has been considered for this SoC.\n");
    return RT_WARN;
}

/* --- stage: cpu -------------------------------------------------------- */

static void show_idreg(const char *name, bool ok, u64 v)
{
    if (ok)
        rt_printf("%-18s 0x%016lx\n", name, v);
    else
        rt_printf("%-18s (read trapped)\n", name);
}

enum rt_result diag_cpu(void)
{
    static const u8 pa_bits[] = {32, 36, 40, 42, 44, 48, 52};
    enum rt_result result = RT_PASS;
    u64 el = mrs(CurrentEL) >> 2;
    u64 sctlr = mrs(SCTLR_EL1);
    u64 frq = mrs(CNTFRQ_EL0);

    rt_printf("exception level   EL%lu\n", el);
    rt_printf("SCTLR             0x%lx  MMU %s, D-cache %s, I-cache %s\n", sctlr,
              (sctlr & BIT(0)) ? "on" : "OFF", (sctlr & BIT(2)) ? "on" : "OFF",
              (sctlr & BIT(12)) ? "on" : "OFF");

    if (el != 1 && el != 2)
        result = RT_FAIL;
    if (!(sctlr & BIT(0)))
        result = RT_FAIL;
    else if (!(sctlr & BIT(2)) && result == RT_PASS)
        result = RT_WARN;

    /* udelay() and get_ticks() share the counter, so this only shows that the
     * counter runs and that CNTFRQ is sane; it cannot detect a wrong rate. */
    u64 t0 = get_ticks();
    udelay(100000);
    u64 dt = get_ticks() - t0;

    rt_printf("timer             CNTFRQ %lu Hz, 100 ms = %lu ticks\n", frq, dt);
    if (!dt || !frq)
        result = RT_FAIL;
    else if (frq != 24000000 && result == RT_PASS)
        result = RT_WARN;

    u64 pfr0, pfr1, isar0, isar1, mmfr0, smfr0, zfr0, cpacr;
    bool ok_pfr0 = RT_PROBE_MRS(ID_AA64PFR0_EL1, &pfr0);
    bool ok_pfr1 = RT_PROBE_MRS(ID_AA64PFR1_EL1, &pfr1);
    bool ok_isar0 = RT_PROBE_MRS(ID_AA64ISAR0_EL1, &isar0);
    bool ok_isar1 = RT_PROBE_MRS(ID_AA64ISAR1_EL1, &isar1);
    bool ok_mmfr0 = RT_PROBE_MRS(ID_AA64MMFR0_EL1, &mmfr0);
    /* By encoding: the assembler only accepts these names with SME/SVE enabled. */
    bool ok_smfr0 = RT_PROBE_MRS(s3_0_c0_c4_5, &smfr0); /* ID_AA64SMFR0_EL1 */
    bool ok_zfr0 = RT_PROBE_MRS(s3_0_c0_c4_4, &zfr0);   /* ID_AA64ZFR0_EL1 */
    bool ok_cpacr = RT_PROBE_MRS(CPACR_EL1, &cpacr);

    rt_printf("\n");
    show_idreg("ID_AA64PFR0_EL1", ok_pfr0, pfr0);
    show_idreg("ID_AA64PFR1_EL1", ok_pfr1, pfr1);
    show_idreg("ID_AA64ISAR0_EL1", ok_isar0, isar0);
    show_idreg("ID_AA64ISAR1_EL1", ok_isar1, isar1);
    show_idreg("ID_AA64MMFR0_EL1", ok_mmfr0, mmfr0);
    show_idreg("ID_AA64SMFR0_EL1", ok_smfr0, smfr0);
    show_idreg("ID_AA64ZFR0_EL1", ok_zfr0, zfr0);
    show_idreg("CPACR_EL1", ok_cpacr, cpacr);
    rt_printf("\n");

    /* Decoded fields that matter for a compute runtime. */
    if (ok_pfr0) {
        rt_printf("FP %s, AdvSIMD %s, SVE %s\n", ((pfr0 >> 16) & 0xf) == 0xf ? "absent" : "present",
                  ((pfr0 >> 20) & 0xf) == 0xf ? "absent" : "present",
                  ((pfr0 >> 32) & 0xf) ? "present" : "absent");
    }
    if (ok_pfr1)
        rt_printf("SME field %lu (0 = absent, 1 = SME, 2 = SME2)\n", (pfr1 >> 24) & 0xf);
    if (ok_isar1)
        rt_printf("BF16 %s, I8MM %s\n", ((isar1 >> 44) & 0xf) ? "present" : "absent",
                  ((isar1 >> 52) & 0xf) ? "present" : "absent");
    if (ok_mmfr0) {
        u32 pa = mmfr0 & 0xf;

        if (pa < ARRAY_SIZE(pa_bits))
            rt_printf("physical address bits %u, 16K granule field %lu\n", pa_bits[pa],
                      (mmfr0 >> 20) & 0xf);
        else
            rt_printf("physical address range field %u (unknown encoding)\n", pa);
    }
    if (ok_cpacr)
        rt_printf("FP/SIMD use at this level: %s (m1n1 itself is built integer-only)\n",
                  ((cpacr >> 20) & 3) == 3 ? "not trapped" : "TRAPPED");

    rt_printf("\nSecondary cores are not started. Upstream m1n1 reports that they\n"
              "power on but never run code on T6050, so this build stays on core 0.\n");

    return result;
}

/* --- stage: exceptions -------------------------------------------------- */

/* A canonical-low address above anything m1n1 maps (RAM, its EL0 aliases and
 * the MMIO windows all sit below 2^40). Touching it raises a translation
 * fault inside the CPU and reaches no hardware. */
#define RT_UNMAPPED_VA 0x0000ffffffffc000ul

enum rt_result diag_exceptions(void)
{
    volatile u64 cell = 0x1122334455667788ul;
    enum exc_guard_t guard_before = exc_guard;
    int count_before = exc_count;
    struct rt_guard g;
    u64 v = 0;

    rt_printf("m1n1's vectors are live; this checks we can take an exception on\n"
              "purpose, survive it, and carry on.\n\n");

    bool good_ok = rt_probe_read64((u64)&cell, &v) && v == 0x1122334455667788ul;

    rt_printf("1. guarded read of valid memory     %s\n",
              good_ok ? "no exception (correct)" : "WRONG");

    bool bad_faulted = !rt_probe_read64(RT_UNMAPPED_VA, &v);

    rt_printf("2. guarded read of unmapped address %s\n",
              bad_faulted ? "faulted and recovered (correct)" : "DID NOT FAULT");

    rt_guard_begin(&g, GUARD_SKIP);
    __asm__ volatile(".inst 0x00000000"); /* permanently undefined instruction */
    int undef_taken = rt_guard_end(&g);

    rt_printf("3. undefined instruction            %d exception(s) taken, expected 1\n",
              undef_taken);

    bool restored = exc_guard == guard_before;

    rt_printf("4. guard state restored             %s\n", restored ? "yes" : "NO");
    rt_printf("\nexceptions handled so far: %d\n", exc_count - count_before);
    rt_printf("\nAn unguarded exception or panic prints registers, then holds the\n"
              "screen for %d s before rebooting (rt_fatal_hold).\n",
              RT_FATAL_HOLD_S);

    return (good_ok && bad_faulted && undef_taken == 1 && restored) ? RT_PASS : RT_FAIL;
}

/* --- stage: memory ------------------------------------------------------ */

#define PATTERN_MIB 64

static u64 pattern_pass(u64 *p, size_t words, u64 salt)
{
    u64 bad = 0;

    for (size_t i = 0; i < words; i++)
        p[i] = (u64)(uintptr_t)&p[i] ^ salt;
    for (size_t i = 0; i < words; i++)
        if (p[i] != ((u64)(uintptr_t)&p[i] ^ salt))
            bad++;

    return bad;
}

enum rt_result diag_memory(void)
{
    size_t bytes = (size_t)RT_ARENA_MB << 20;
    size_t npages = bytes >> RT_PAGE_SHIFT;
    size_t meta_bytes = rt_arena_meta_bytes(npages);

    /* m1n1's heapblock hands out the RAM above its own image and never takes
     * it back, so this region is ours for the rest of the run. */
    void *base = heapblock_alloc_aligned(bytes, RT_PAGE_SIZE);
    void *meta = malloc(meta_bytes);

    rt_printf("arena         %u MiB at %p (%lu pages of 16 KiB)\n", RT_ARENA_MB, base, npages);
    rt_printf("bookkeeping   %lu bytes at %p\n", meta_bytes, meta);

    if (!base || !meta || rt_arena_init(&rt_dev_arena, base, bytes, meta, meta_bytes)) {
        rt_printf("arena setup FAILED\n");
        return RT_FAIL;
    }

    int rc = rt_arena_selftest(&rt_dev_arena, 0x5eed1234, 4000);

    rt_printf("allocator self-test (4000 rounds)   %s (code %d)\n", rc ? "FAILED" : "ok", rc);
    if (rc)
        return RT_FAIL;

    size_t test_bytes = (size_t)PATTERN_MIB << 20;

    if (test_bytes > bytes / 2)
        test_bytes = bytes / 2;

    u64 *p = rt_arena_alloc(&rt_dev_arena, test_bytes, 0);

    if (!p) {
        rt_printf("could not allocate the pattern-test buffer\n");
        return RT_FAIL;
    }

    u64 t0 = get_ticks();
    u64 bad = pattern_pass(p, test_bytes / sizeof(u64), 0x5555aaaa5555aaaaul);

    bad += pattern_pass(p, test_bytes / sizeof(u64), 0xaaaa5555aaaa5555ul);

    u64 ms = (get_ticks() - t0) * 1000 / mrs(CNTFRQ_EL0);

    rt_printf("pattern test  %lu MiB written and verified twice in %lu ms\n", test_bytes >> 20, ms);
    rt_printf("              %lu mismatching words\n", bad);

    if (rt_arena_free(&rt_dev_arena, p) || rt_arena_check(&rt_dev_arena)) {
        rt_printf("arena inconsistent after the pattern test\n");
        return RT_FAIL;
    }

    rt_printf("arena free    %lu of %lu pages, peak use %lu pages\n",
              rt_arena_free_pages(&rt_dev_arena), rt_dev_arena.npages, rt_dev_arena.peak_pages);

    if (bad)
        return RT_FAIL;

    rt_dev_arena_ready = true;
    rt_printf("\nrt_dev_arena is ready for later stages (DART, firmware buffers).\n");
    return RT_PASS;
}

/* --- stage: devtree ----------------------------------------------------- */

#define GROUP_MAX_LINES 20

static bool matches(const char *s, const char *const *keys, bool exact)
{
    if (!s)
        return false;
    for (; *keys; keys++)
        if (exact ? !strcmp(s, *keys) : strstr(s, *keys) != NULL)
            return true;
    return false;
}

/* List /arm-io children by name: an exact match against keys, or (exact =
 * false) any name or first compatible string containing a key. Returns how
 * many matched. */
static int list_group(const char *title, const char *const *keys, bool exact, int armio, int *path,
                      int depth)
{
    int found = 0, node = armio;

    rt_printf("%s\n", title);

    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        u32 clen = 0;
        const char *compat = adt_getprop(adt, node, "compatible", &clen);

        if (!clen)
            compat = NULL;
        if (!matches(name, keys, exact) && (exact || !matches(compat, keys, false)))
            continue;

        if (++found > GROUP_MAX_LINES)
            continue;

        u64 addr = 0, size = 0;

        path[depth] = node;
        path[depth + 1] = 0;

        if (adt_get_reg(adt, path, "reg", 0, &addr, &size) == 0)
            rt_printf(" %-16.16s %-28.28s 0x%lx +0x%lx\n", str_or(name, "?"), str_or(compat, "-"),
                      addr, size);
        else
            rt_printf(" %-16.16s %-28.28s (no reg)\n", str_or(name, "?"), str_or(compat, "-"));
    }

    if (found > GROUP_MAX_LINES)
        rt_printf(" ... and %d more\n", found - GROUP_MAX_LINES);
    if (!found)
        rt_printf(" (none found)\n");
    rt_printf("\n");

    return found;
}

enum rt_result diag_devtree(void)
{
    /* These names were read from this machine's device tree under macOS
     * 26.6.2 (ioreg -p IODeviceTree, 2026-11-09), so they are expected to be
     * found. The addresses are not hard-coded: they are read and printed. */
    static const char *const core_keys[] = {"aic", "pmgr", "wdt", "mcc", "uart0", "smc", NULL};
    static const char *const gpu_keys[] = {"sgx", "gfx", "agx", "gpu", NULL};
    static const char *const coproc_keys[] = {"ans", "sart-ans", "sep", "aop", "dcp",
                                              "disp0", "ane0", NULL};
    int path[8] = {0};

    int cpus = adt_path_offset(adt, "/cpus");

    if (cpus >= 0) {
        int node = cpus, total = 0;
        char types[40] = "";
        size_t used = 0;

        ADT_FOREACH_CHILD(adt, node)
        {
            const char *type = adt_getprop(adt, node, "cluster-type", NULL);
            const char *state = adt_getprop(adt, node, "state", NULL);

            total++;
            if (used + 2 < sizeof(types)) {
                types[used++] = type ? type[0] : '?';
                types[used] = 0;
            }
            if (state && !strcmp(state, "running"))
                rt_printf("running core  %s (cluster-type %s)\n", str_or(adt_get_name(adt, node), "?"),
                          str_or(type, "?"));
        }
        rt_printf("cpus          %d, cluster types in order: %s\n\n", total, types);
    } else {
        rt_printf("/cpus not found\n\n");
    }

    int armio = adt_path_offset_trace(adt, "/arm-io", path);

    if (armio < 0) {
        rt_printf("/arm-io not found: cannot enumerate devices\n");
        return RT_FAIL;
    }

    int depth = 0;

    while (depth < 6 && path[depth])
        depth++;

    int total = adt_get_child_count(adt, armio);
    int darts = 0, node = armio;

    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);

        if (name && !strncmp(name, "dart", 4))
            darts++;
    }

    rt_printf("/arm-io       %d devices, %d of them DARTs (IOMMUs)\n", total, darts);
    rt_printf("watchdog      0x%lx, hang guard %s\n\n", rt_wdt_base(),
              rt_wdt_guard_enabled() ? "ARMED" : "off");

    list_group("core blocks:", core_keys, true, armio, path, depth);

    int gpu = list_group("GPU and its coprocessors:", gpu_keys, false, armio, path, depth);

    list_group("coprocessors and storage:", coproc_keys, true, armio, path, depth);

    if (!gpu) {
        rt_printf("No GPU node found, although macOS shows sgx (gpu,t6050) on this\n"
                  "machine. The device tree m1n1 received differs from the one macOS sees.\n");
        return RT_WARN;
    }

    return RT_PASS;
}

/* --- stage: gpu --------------------------------------------------------- */

/*
 * First contact with the GPU. Three levels:
 *
 *   always        read the GPU's device-tree nodes (RAM only) and print what
 *                 the next milestone needs: register ranges, power and clock
 *                 gate ids, and the two firmware coprocessors.
 *   RT_GPU_PROBE  one guarded 32-bit read at the start of each range. A
 *                 powered-down block normally answers with an abort, which
 *                 the guard absorbs; the result says whether iBoot left the
 *                 GPU on.
 *   RT_GPU_POWER  ask m1n1's power manager to switch the GPU's gates on,
 *                 repeat the reads, and switch them off again.
 *
 * Node and property names were read from this machine under macOS (ioreg,
 * 2026-11-09): sgx is "gpu,t6050"; gfx-asc and gfx1-asc are "iop,ascwrap-v6"
 * with roles GFX and GFX1. Nothing has been tried on the hardware, and no GPU
 * register is written by this file.
 */

static void show_gates(int node, const char *prop)
{
    u32 len = 0;
    const u32 *ids = adt_getprop(adt, node, prop, &len);

    if (!ids || !len) {
        rt_printf("   %-12s (absent)\n", prop);
        return;
    }

    rt_printf("   %-12s", prop);
    for (u32 i = 0; i < len / 4 && i < 8; i++)
        rt_printf(" 0x%x", ids[i]);
    rt_printf("\n");
}

static void probe_word(u64 addr)
{
#ifdef RT_GPU_PROBE
    u32 v;

    if (rt_probe_read32(addr, &v))
        rt_printf("      read 0x%lx = 0x%08x\n", addr, v);
    else
        rt_printf("      read 0x%lx faulted (block not accessible)\n", addr);
#else
    UNUSED(addr);
#endif
}

/* Print one node's identity and register ranges. Returns the node offset. */
static int show_gpu_node(const char *path_str)
{
    int path[8] = {0};
    int node = adt_path_offset_trace(adt, path_str, path);

    if (node < 0) {
        rt_printf("%s: not found\n", path_str);
        return -1;
    }

    u32 clen = 0;
    const char *compat = adt_getprop(adt, node, "compatible", &clen);
    const char *role = adt_getprop(adt, node, "role", NULL);

    rt_printf("%s  %s%s%s\n", path_str, clen ? compat : "-", role ? "  role " : "", role ? role : "");

    for (int i = 0; i < 6; i++) {
        u64 addr = 0, size = 0;

        if (adt_get_reg(adt, path, "reg", i, &addr, &size))
            break;
        rt_printf("   reg[%d]       0x%lx +0x%lx\n", i, addr, size);
        if (size >= 4)
            probe_word(addr);
    }

    show_gates(node, "clock-gates");
    show_gates(node, "power-gates");
    return node;
}

enum rt_result diag_gpu(void)
{
#if defined(RT_GPU_POWER)
    rt_printf("mode: device tree, guarded reads, and power-gate cycling\n\n");
#elif defined(RT_GPU_PROBE)
    rt_printf("mode: device tree and guarded reads (no power changes)\n\n");
#else
    rt_printf("mode: device tree only (no GPU register is touched)\n\n");
#endif

    int sgx = show_gpu_node("/arm-io/sgx");

    if (sgx >= 0) {
        u32 perf = 0;

        if (ADT_GETPROP(adt, sgx, "perf-state-count", &perf) > 0)
            rt_printf("   perf states  %u\n", perf);
    }
    rt_printf("\n");

    int asc0 = show_gpu_node("/arm-io/gfx-asc");

    rt_printf("\n");

    int asc1 = show_gpu_node("/arm-io/gfx1-asc");

    rt_printf("\n");
    show_gpu_node("/arm-io/mapper-gfx-asc");

#ifdef RT_GPU_POWER
    rt_printf("\npowering GPU gates on through m1n1's pmgr ...\n");

    int on = pmgr_adt_power_enable("/arm-io/sgx");

    rt_printf("pmgr_adt_power_enable(/arm-io/sgx) = %d\n", on);
    if (on >= 0) {
        rt_printf("register ranges with the gates on:\n");
        show_gpu_node("/arm-io/sgx");

        int off = pmgr_adt_power_disable("/arm-io/sgx");

        rt_printf("pmgr_adt_power_disable(/arm-io/sgx) = %d\n", off);
        if (off < 0)
            return RT_WARN;
    } else {
        rt_printf("m1n1's power manager could not switch the GPU on for this SoC\n");
        return RT_WARN;
    }
#endif

    if (sgx < 0 || asc0 < 0) {
        rt_printf("\nThe GPU nodes macOS shows are missing from this device tree.\n");
        return RT_WARN;
    }
    if (asc1 < 0)
        rt_printf("\nNote: only one GPU coprocessor node was found.\n");

    return RT_PASS;
}

/* --- stage: nvme -------------------------------------------------------- */

enum rt_result diag_nvme(void)
{
#ifdef RT_NVME_PROBE
    rt_printf("Runs m1n1's NVMe driver with read commands only. If this works,\n"
              "m1n1 can chainload a build from disk and each test no longer\n"
              "needs a trip through recovery.\n\n");

    if (!rt_dev_arena_ready) {
        rt_printf("skipped: the memory stage did not pass\n");
        return RT_SKIP;
    }

    u8 *buf = rt_arena_alloc(&rt_dev_arena, RT_PAGE_SIZE, 0);

    if (!buf)
        return RT_FAIL;

    rt_printf("nvme_init() ...\n");
    if (!nvme_init()) {
        rt_printf("nvme_init() failed: m1n1 cannot drive this SSD controller yet\n");
        rt_arena_free(&rt_dev_arena, buf);
        return RT_FAIL;
    }

    memset(buf, 0, RT_PAGE_SIZE);
    bool r0 = nvme_read(1, 0, buf);
    bool mbr = r0 && buf[510] == 0x55 && buf[511] == 0xaa;

    memset(buf, 0, RT_PAGE_SIZE);
    bool r1 = nvme_read(1, 1, buf);
    bool gpt = r1 && !memcmp(buf, "EFI PART", 8);

    nvme_shutdown();
    rt_arena_free(&rt_dev_arena, buf);

    rt_printf("\nLBA 0 read %s, protective MBR signature %s\n", r0 ? "ok" : "FAILED",
              mbr ? "found" : "missing");
    rt_printf("LBA 1 read %s, GPT header %s\n", r1 ? "ok" : "FAILED", gpt ? "found" : "missing");

    return (mbr && gpt) ? RT_PASS : RT_FAIL;
#else
    rt_printf("Not built in. Rebuild with RT_NVME_PROBE=1 once the earlier stages\n"
              "pass on this machine (rung 3 in docs/TESTING.md).\n");
    return RT_SKIP;
#endif
}
