/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * m5-baremetal runtime: internal interfaces.
 *
 * The runtime is linked into m1n1 and entered from m1n1_main() once m1n1 has
 * finished its own bring-up (MMU and caches on, exception vectors installed,
 * framebuffer console active, heap ready). Everything here runs on the boot
 * CPU with interrupts masked; there is no OS underneath.
 */
#ifndef RT_H
#define RT_H

#include "exception.h"
#include "rt_arena.h"
#include "types.h"

#define RT_VERSION "0.1"

enum rt_result {
    RT_PENDING = 0,
    RT_RUNNING,
    RT_PASS,
    RT_WARN,
    RT_FAIL,
    RT_SKIP,
};

const char *rt_result_name(enum rt_result r);

/* --- rt_main.c --------------------------------------------------------- */

void rt_main(void);
void rt_fatal_hold(void);

/* Arena for device-visible buffers. Valid after the "memory" stage passes. */
extern struct rt_arena rt_dev_arena;
extern bool rt_dev_arena_ready;

/* --- rt_ui.c: screen output without a serial console ------------------- */

void ui_init(const char *const *tile_names, int ntiles);
void ui_tile(int idx, enum rt_result r);
void ui_page(const char *title); /* hold is the caller's job; this clears and titles */
void ui_hold(u32 seconds, const char *what);
void ui_fatal(int tile);
u32 ui_cols(void);

/* printf to every m1n1 console, with automatic page breaks on the screen so
 * nothing scrolls away before it can be photographed. */
int rt_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- rt_probe.c: exception-guarded access ------------------------------ */

struct rt_guard {
    enum exc_guard_t saved;
    int count0;
};

void rt_guard_begin(struct rt_guard *g, enum exc_guard_t mode);
int rt_guard_end(struct rt_guard *g); /* number of exceptions taken inside */

/* Read memory or MMIO; false if the access faulted. */
bool rt_probe_read32(u64 addr, u32 *val);
bool rt_probe_read64(u64 addr, u64 *val);

/* Read a system register; evaluates to false if the read trapped. */
#define RT_PROBE_MRS(reg, valp)                                                                    \
    ({                                                                                             \
        struct rt_guard _g;                                                                        \
        rt_guard_begin(&_g, GUARD_SKIP);                                                           \
        u64 _v = mrs(reg);                                                                         \
        bool _ok = rt_guard_end(&_g) == 0;                                                         \
        *(valp) = _ok ? _v : 0;                                                                    \
        _ok;                                                                                       \
    })

/* --- rt_wdt.c: watchdog as a hang guard -------------------------------- */

int rt_wdt_init(void); /* 0 if the watchdog registers were found */
u64 rt_wdt_base(void);
bool rt_wdt_guard_enabled(void);
void rt_wdt_arm(u32 seconds); /* no-op unless built with RT_WDT_GUARD=1 */
void rt_wdt_disarm(void);

/* --- rt_diag.c: the diagnostic stages ---------------------------------- */

enum rt_result diag_ident(void);
enum rt_result diag_cpu(void);
enum rt_result diag_exceptions(void);
enum rt_result diag_memory(void);
enum rt_result diag_devtree(void);
enum rt_result diag_gpu(void);
enum rt_result diag_nvme(void);

#endif
