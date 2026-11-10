/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Watchdog as a hang guard.
 *
 * Without a serial console, a hang is indistinguishable from a slow step. With
 * the watchdog armed before each stage, a hang becomes a reboot after a known
 * delay, and the last line on screen names the step that never finished.
 *
 * UNVERIFIED ON T6050: the register layout (COUNT 0x10, ALARM 0x14, CTL 0x1c)
 * and the 24 MHz tick are the ones m1n1's wdt.c uses for its reboot path on
 * earlier SoCs. Nobody has reported them for M5 Pro. That is why arming is
 * opt-in (RT_WDT_GUARD=1) and off for the first boot. The first boot's final
 * reboot goes through m1n1's wdt_reboot(), which uses the same registers, so
 * a clean reboot there is the evidence for switching this on.
 */

#include "rt.h"
#include "adt.h"
#include "utils.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

#define WDT_CTL_RESET_EN 4
#define WDT_TICK_HZ      24000000u /* assumed, see above */
#define WDT_MAX_SECONDS  170u      /* 32-bit alarm at 24 MHz wraps at ~178 s */

static u64 wdt_base;

int rt_wdt_init(void)
{
    int path[8];

    if (adt_path_offset_trace(adt, "/arm-io/wdt", path) < 0)
        return -1;
    if (adt_get_reg(adt, path, "reg", 0, &wdt_base, NULL)) {
        wdt_base = 0;
        return -1;
    }
    return 0;
}

u64 rt_wdt_base(void)
{
    return wdt_base;
}

bool rt_wdt_guard_enabled(void)
{
#ifdef RT_WDT_GUARD
    return wdt_base != 0;
#else
    return false;
#endif
}

void rt_wdt_arm(u32 seconds)
{
    if (!rt_wdt_guard_enabled())
        return;

    if (seconds > WDT_MAX_SECONDS)
        seconds = WDT_MAX_SECONDS;

    write32(wdt_base + WDT_CTL, 0);
    write32(wdt_base + WDT_ALARM, seconds * WDT_TICK_HZ);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, WDT_CTL_RESET_EN);
}

void rt_wdt_disarm(void)
{
    if (!rt_wdt_guard_enabled())
        return;

    write32(wdt_base + WDT_CTL, 0);
}
