/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Exception-guarded access, built on m1n1's exc_guard mechanism.
 *
 * m1n1's handlers (src/exception.c) consult exc_guard when an exception
 * arrives. With a guard set they count the exception, step over the faulting
 * instruction and resume, instead of dumping registers and rebooting. That
 * turns "does touching this address or register fault?" into a question we can
 * ask and survive, which matters a great deal with no debugger attached.
 */

#include "rt.h"
#include "utils.h"

void rt_guard_begin(struct rt_guard *g, enum exc_guard_t mode)
{
    g->saved = exc_guard;
    g->count0 = exc_count;
    exc_guard = mode | GUARD_SILENT;
}

int rt_guard_end(struct rt_guard *g)
{
    /* Give an asynchronous abort (SError) raised by the access a chance to be
     * delivered while the guard is still in place. */
    sysop("dsb sy");
    sysop("isb");

    int taken = exc_count - g->count0;

    exc_guard = g->saved;
    return taken;
}

bool rt_probe_read32(u64 addr, u32 *val)
{
    struct rt_guard g;

    rt_guard_begin(&g, GUARD_MARK);
    u32 v = read32(addr);
    bool ok = rt_guard_end(&g) == 0;

    *val = ok ? v : 0;
    return ok;
}

bool rt_probe_read64(u64 addr, u64 *val)
{
    struct rt_guard g;

    rt_guard_begin(&g, GUARD_MARK);
    u64 v = read64(addr);
    bool ok = rt_guard_end(&g) == 0;

    *val = ok ? v : 0;
    return ok;
}
