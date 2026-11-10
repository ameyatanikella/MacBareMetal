/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * m5-baremetal runtime entry.
 *
 * Called from m1n1_main() after m1n1's own initialisation. Runs the stages in
 * order, shows each result on screen long enough to photograph, then reboots
 * so the machine returns to its default boot volume (macOS).
 */

#include "rt.h"

#include "utils.h"

struct rt_arena rt_dev_arena;
bool rt_dev_arena_ready;

struct rt_stage {
    const char *name;
    enum rt_result (*fn)(void);
    u32 budget_s;  /* hang-guard budget; only used with RT_WDT_GUARD=1 */
    bool required; /* counts towards the milestone verdict */
};

static const struct rt_stage stages[] = {
    {"ident", diag_ident, 60, true},
    {"cpu", diag_cpu, 60, true},
    {"exceptions", diag_exceptions, 60, true},
    {"memory", diag_memory, 120, true},
    {"devtree", diag_devtree, 120, true},
    {"gpu", diag_gpu, 120, false},
    {"nvme", diag_nvme, 120, false},
};

#define NSTAGES     ((int)ARRAY_SIZE(stages))
#define TILE_REBOOT NSTAGES

static const char *tile_names[NSTAGES + 1];
static enum rt_result results[NSTAGES];
static int current_tile = -1;
static bool running;

void rt_main(void)
{
    for (int i = 0; i < NSTAGES; i++)
        tile_names[i] = stages[i].name;
    tile_names[TILE_REBOOT] = "reboot";

    rt_wdt_init();
    ui_init(tile_names, NSTAGES + 1);
    running = true;

    for (int i = 0; i < NSTAGES; i++) {
        char title[48];

        current_tile = i;
        snprintf(title, sizeof(title), "stage %d of %d: %s", i + 1, NSTAGES, stages[i].name);
        ui_page(title);
        ui_tile(i, RT_RUNNING);

        rt_wdt_arm(stages[i].budget_s);
        results[i] = stages[i].fn();
        rt_wdt_arm(60);

        ui_tile(i, results[i]);
        rt_printf("\n>> %s: %s\n", stages[i].name, rt_result_name(results[i]));
        ui_hold(RT_HOLD_S, "next page");
    }

    current_tile = TILE_REBOOT;
    ui_page("summary");

    bool milestone = true;

    for (int i = 0; i < NSTAGES; i++) {
        rt_printf("  %d  %-12s %s%s\n", i + 1, stages[i].name, rt_result_name(results[i]),
                  stages[i].required ? "" : "  (optional)");
        if (stages[i].required && results[i] != RT_PASS && results[i] != RT_WARN)
            milestone = false;
    }

    rt_printf("\nMILESTONE 1: %s\n", milestone ? "REACHED" : "NOT REACHED");
    rt_printf("(required stages must be PASS or WARN; read each WARN)\n\n");

    rt_wdt_disarm();
    ui_tile(TILE_REBOOT, RT_RUNNING);

#ifdef RT_EXIT_TO_PROXY
    rt_printf("Returning to m1n1 (USB proxy). Hold the power button to turn off.\n");
    ui_hold(RT_HOLD_S, "m1n1 proxy");
    ui_tile(TILE_REBOOT, RT_SKIP);
    running = false;
    return;
#else
    rt_printf("Rebooting to the default startup disk after the countdown.\n");
    rt_printf("If the screen is still here 10 s after it ends, the reboot did\n"
              "not work: hold the power button until the Mac turns off.\n");
    ui_hold(RT_FINAL_HOLD_S, "reboot");

    running = false;
    reboot();
#endif
}

/*
 * Called from m1n1's flush_and_reboot(), i.e. after any panic or unhandled
 * exception has printed its message and register dump. Holds the screen, then
 * returns so m1n1 resets the machine. Also runs for m1n1's own early panics,
 * before rt_main() and the UI exist; then it is just a delay.
 */
void rt_fatal_hold(void)
{
    static bool entered;

    if (entered)
        return;
    entered = true;

    rt_wdt_disarm();

    if (running)
        ui_fatal(current_tile);

    printf("\n*** FATAL in m5rt %s build %s ***\n", RT_VERSION, RT_BUILD_ID);
    printf("*** holding %d s, then rebooting; photograph the screen ***\n", RT_FATAL_HOLD_S);

    for (int i = 0; i < RT_FATAL_HOLD_S; i++)
        mdelay(1000);
}
