/* Host stand-in for m1n1's types.h (simulation only). */
#ifndef SIM_TYPES_H
#define SIM_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
#define BIT(x) (1UL << (x))
#endif
