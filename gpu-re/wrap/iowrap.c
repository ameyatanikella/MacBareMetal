/*
 * iowrap: log the IOKit calls a process makes to kernel drivers.
 *
 *   DYLD_INSERT_LIBRARIES=out/iowrap.dylib IOWRAP_LOG=out/trace.txt out/mtlrun ...
 *
 * Metal's GPU driver is a userspace bundle that talks to the kernel half
 * through IOKit user clients. Interposing the public IOKit entry points shows
 * which user-client methods (selectors) a known workload uses, with what
 * argument sizes, and which shared memory it maps. That is observation of
 * our own process's system-call traffic; no Apple code is read or modified.
 *
 * It only works on binaries we build ourselves: dyld ignores
 * DYLD_INSERT_LIBRARIES for protected system binaries, which is as it
 * should be.
 *
 * One line per call:
 *   open    <service class> type=<n> -> conn=<port> kr=<result>
 *   call    conn=<port> sel=<n> in=<scalars>/<struct bytes> out=<scalars>/<struct bytes> kr=<result> [in: hex]
 *   map     conn=<port> type=<n> -> addr=<va> size=<bytes> kr=<result>
 */

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTERPOSE(replacement, replacee)                                                           \
    __attribute__((used)) static const struct {                                                    \
        const void *replacement;                                                                   \
        const void *replacee;                                                                      \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {                      \
        (const void *)(unsigned long)&replacement, (const void *)(unsigned long)&replacee}

#define HEX_BYTES 48

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf;

static FILE *out(void)
{
    if (!logf) {
        const char *path = getenv("IOWRAP_LOG");

        logf = path ? fopen(path, "w") : NULL;
        if (!logf)
            logf = stderr;
    }
    return logf;
}

static void hex(FILE *f, const void *p, size_t n)
{
    const unsigned char *b = p;

    for (size_t i = 0; i < n && i < HEX_BYTES; i++)
        fprintf(f, "%02x", b[i]);
    if (n > HEX_BYTES)
        fprintf(f, "...");
}

static kern_return_t w_IOServiceOpen(io_service_t service, task_port_t task, uint32_t type,
                                     io_connect_t *connect)
{
    io_name_t cls = "?";
    kern_return_t kr = IOServiceOpen(service, task, type, connect);

    IOObjectGetClass(service, cls);
    pthread_mutex_lock(&lock);
    fprintf(out(), "open    %s type=%u -> conn=%#x kr=%#x\n", cls, type, connect ? *connect : 0, kr);
    fflush(out());
    pthread_mutex_unlock(&lock);
    return kr;
}
INTERPOSE(w_IOServiceOpen, IOServiceOpen);

static void log_call(const char *kind, mach_port_t conn, uint32_t sel, uint32_t in_cnt,
                     const void *in_struct, size_t in_size, uint32_t out_cnt, size_t out_size,
                     kern_return_t kr)
{
    pthread_mutex_lock(&lock);
    fprintf(out(), "%-7s conn=%#x sel=%u in=%u/%zu out=%u/%zu kr=%#x", kind, conn, sel, in_cnt,
            in_size, out_cnt, out_size, kr);
    if (in_struct && in_size) {
        fprintf(out(), " in:");
        hex(out(), in_struct, in_size);
    }
    fprintf(out(), "\n");
    fflush(out());
    pthread_mutex_unlock(&lock);
}

static kern_return_t w_IOConnectCallMethod(mach_port_t conn, uint32_t sel, const uint64_t *in,
                                           uint32_t in_cnt, const void *in_struct, size_t in_size,
                                           uint64_t *outp, uint32_t *out_cnt, void *out_struct,
                                           size_t *out_size)
{
    kern_return_t kr = IOConnectCallMethod(conn, sel, in, in_cnt, in_struct, in_size, outp, out_cnt,
                                           out_struct, out_size);

    log_call("call", conn, sel, in_cnt, in_struct, in_size, out_cnt ? *out_cnt : 0,
             out_size ? *out_size : 0, kr);
    return kr;
}
INTERPOSE(w_IOConnectCallMethod, IOConnectCallMethod);

static kern_return_t w_IOConnectCallAsyncMethod(mach_port_t conn, uint32_t sel, mach_port_t wake,
                                                uint64_t *ref, uint32_t ref_cnt, const uint64_t *in,
                                                uint32_t in_cnt, const void *in_struct,
                                                size_t in_size, uint64_t *outp, uint32_t *out_cnt,
                                                void *out_struct, size_t *out_size)
{
    kern_return_t kr = IOConnectCallAsyncMethod(conn, sel, wake, ref, ref_cnt, in, in_cnt, in_struct,
                                                in_size, outp, out_cnt, out_struct, out_size);

    log_call("async", conn, sel, in_cnt, in_struct, in_size, out_cnt ? *out_cnt : 0,
             out_size ? *out_size : 0, kr);
    return kr;
}
INTERPOSE(w_IOConnectCallAsyncMethod, IOConnectCallAsyncMethod);

static kern_return_t w_IOConnectCallStructMethod(mach_port_t conn, uint32_t sel,
                                                 const void *in_struct, size_t in_size,
                                                 void *out_struct, size_t *out_size)
{
    kern_return_t kr = IOConnectCallStructMethod(conn, sel, in_struct, in_size, out_struct, out_size);

    log_call("struct", conn, sel, 0, in_struct, in_size, 0, out_size ? *out_size : 0, kr);
    return kr;
}
INTERPOSE(w_IOConnectCallStructMethod, IOConnectCallStructMethod);

static kern_return_t w_IOConnectCallScalarMethod(mach_port_t conn, uint32_t sel, const uint64_t *in,
                                                 uint32_t in_cnt, uint64_t *outp, uint32_t *out_cnt)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, sel, in, in_cnt, outp, out_cnt);

    log_call("scalar", conn, sel, in_cnt, NULL, 0, out_cnt ? *out_cnt : 0, 0, kr);
    return kr;
}
INTERPOSE(w_IOConnectCallScalarMethod, IOConnectCallScalarMethod);

static kern_return_t w_IOConnectMapMemory64(io_connect_t conn, uint32_t type, task_port_t task,
                                            mach_vm_address_t *addr, mach_vm_size_t *size,
                                            IOOptionBits opts)
{
    kern_return_t kr = IOConnectMapMemory64(conn, type, task, addr, size, opts);

    pthread_mutex_lock(&lock);
    fprintf(out(), "map     conn=%#x type=%u -> addr=%#llx size=%llu kr=%#x\n", conn, type,
            addr ? *addr : 0, size ? *size : 0, kr);
    fflush(out());
    pthread_mutex_unlock(&lock);
    return kr;
}
INTERPOSE(w_IOConnectMapMemory64, IOConnectMapMemory64);

/* Fast traps: user clients can expose a second, cheaper call path. */
#define TRAP_LOG(n, ...)                                                                           \
    do {                                                                                           \
        uintptr_t a[] = {__VA_ARGS__};                                                             \
        pthread_mutex_lock(&lock);                                                                 \
        fprintf(out(), "trap%d   conn=%#x index=%u kr=%#x args:", n, conn, index, kr);              \
        for (size_t i = 0; i < sizeof(a) / sizeof(a[0]); i++)                                      \
            fprintf(out(), " %#lx", a[i]);                                                         \
        fprintf(out(), "\n");                                                                      \
        fflush(out());                                                                             \
        pthread_mutex_unlock(&lock);                                                               \
    } while (0)

static kern_return_t w_IOConnectTrap0(io_connect_t conn, uint32_t index)
{
    kern_return_t kr = IOConnectTrap0(conn, index);
    TRAP_LOG(0, 0);
    return kr;
}
INTERPOSE(w_IOConnectTrap0, IOConnectTrap0);

static kern_return_t w_IOConnectTrap1(io_connect_t conn, uint32_t index, uintptr_t p1)
{
    kern_return_t kr = IOConnectTrap1(conn, index, p1);
    TRAP_LOG(1, p1);
    return kr;
}
INTERPOSE(w_IOConnectTrap1, IOConnectTrap1);

static kern_return_t w_IOConnectTrap2(io_connect_t conn, uint32_t index, uintptr_t p1, uintptr_t p2)
{
    kern_return_t kr = IOConnectTrap2(conn, index, p1, p2);
    TRAP_LOG(2, p1, p2);
    return kr;
}
INTERPOSE(w_IOConnectTrap2, IOConnectTrap2);

static kern_return_t w_IOConnectTrap3(io_connect_t conn, uint32_t index, uintptr_t p1, uintptr_t p2,
                                      uintptr_t p3)
{
    kern_return_t kr = IOConnectTrap3(conn, index, p1, p2, p3);
    TRAP_LOG(3, p1, p2, p3);
    return kr;
}
INTERPOSE(w_IOConnectTrap3, IOConnectTrap3);

static kern_return_t w_IOConnectTrap4(io_connect_t conn, uint32_t index, uintptr_t p1, uintptr_t p2,
                                      uintptr_t p3, uintptr_t p4)
{
    kern_return_t kr = IOConnectTrap4(conn, index, p1, p2, p3, p4);
    TRAP_LOG(4, p1, p2, p3, p4);
    return kr;
}
INTERPOSE(w_IOConnectTrap4, IOConnectTrap4);

static kern_return_t w_IOConnectTrap6(io_connect_t conn, uint32_t index, uintptr_t p1, uintptr_t p2,
                                      uintptr_t p3, uintptr_t p4, uintptr_t p5, uintptr_t p6)
{
    kern_return_t kr = IOConnectTrap6(conn, index, p1, p2, p3, p4, p5, p6);
    TRAP_LOG(6, p1, p2, p3, p4, p5, p6);
    return kr;
}
INTERPOSE(w_IOConnectTrap6, IOConnectTrap6);
