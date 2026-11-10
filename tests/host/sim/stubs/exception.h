/* Host stand-in for m1n1's exception.h (simulation only). */
#ifndef SIM_EXCEPTION_H
#define SIM_EXCEPTION_H
enum exc_guard_t { GUARD_OFF = 0, GUARD_SKIP, GUARD_MARK, GUARD_RETURN, GUARD_SILENT = 0x100 };
extern volatile enum exc_guard_t exc_guard;
extern volatile int exc_count;
#endif
