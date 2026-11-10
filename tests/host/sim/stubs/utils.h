/* Host stand-in for m1n1's utils.h (simulation only). */
#ifndef SIM_UTILS_H
#define SIM_UTILS_H
#include <stdio.h>
#include "types.h"
int sim_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf(...) sim_printf(__VA_ARGS__)
void mdelay(unsigned ms);
#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#endif
