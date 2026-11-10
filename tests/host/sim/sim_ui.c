/*
 * Host simulation of the runtime's screen output.
 *
 * Compiles the real rt/rt_ui.c against stand-ins for m1n1's framebuffer and
 * console (which follow src/fb.c closely), drives it with representative
 * stage output, and writes PPM snapshots. This checks layout and paging, the
 * only debug channel on the real machine, without booting anything.
 *
 * It does NOT exercise the diagnostics themselves or any hardware behaviour.
 *
 *   sim_ui <font.bin> <font_retina.bin> <outdir> <width> <height> <retina 0|1>
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fb.h"
#include "rt.h"
#include "utils.h"
#include "xnuboot.h"

/* --- stand-ins for m1n1 globals ---------------------------------------- */

fb_t fb;
struct boot_args cur_boot_args;
volatile enum exc_guard_t exc_guard;
volatile int exc_count;
u8 _binary_build_font_bin_start[12160];
u8 _binary_build_font_retina_bin_start[48640];

static struct {
    const u8 *font;
    u32 fw, fh, row, col, max_row, max_col, mrows, mcols;
} con;

static const char *outdir;
static const char *snap_name; /* take a snapshot at the next mdelay() */
static int scrolled_lines;    /* lines lost to scrolling: must stay 0 */

static void snapshot(const char *name)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/%s.ppm", outdir, name);

    FILE *f = fopen(path, "wb");

    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%u %u\n255\n", fb.width, fb.height);
    for (u32 i = 0; i < fb.width * fb.height; i++) {
        u32 p = fb.ptr[i];
        u8 rgb[3] = {(u8)(p >> 16), (u8)(p >> 8), (u8)p};

        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

void mdelay(unsigned ms)
{
    (void)ms;
    if (snap_name) {
        snapshot(snap_name);
        snap_name = NULL;
    }
}

void rt_wdt_arm(u32 seconds)
{
    (void)seconds;
}

/* --- framebuffer primitives (as src/fb.c, 24-bit pixels) ---------------- */

void fb_fill(u32 x, u32 y, u32 w, u32 h, rgb_t c)
{
    if (x + w > fb.width || y + h > fb.height) {
        fprintf(stderr, "fb_fill out of bounds: %u,%u %ux%u\n", x, y, w, h);
        exit(1);
    }
    for (u32 i = 0; i < h; i++)
        for (u32 j = 0; j < w; j++)
            fb.ptr[x + j + (y + i) * fb.stride] = ((u32)c.r << 16) | ((u32)c.g << 8) | c.b;
}

void fb_blit(u32 x, u32 y, u32 w, u32 h, void *data, u32 stride, pix_fmt_t fmt)
{
    u8 *p = data;

    (void)fmt;
    if (x + w > fb.width || y + h > fb.height) {
        fprintf(stderr, "fb_blit out of bounds: %u,%u %ux%u\n", x, y, w, h);
        exit(1);
    }
    for (u32 i = 0; i < h; i++)
        for (u32 j = 0; j < w; j++) {
            u8 *s = &p[(j + i * stride) * 4];

            fb.ptr[x + j + (y + i) * fb.stride] = ((u32)s[0] << 16) | ((u32)s[1] << 8) | s[2];
        }
}

static void clear_row(u32 row)
{
    u32 w = (con.mcols + con.max_col) * con.fw;

    for (u32 y = 0; y < con.fh; y++)
        memset(&fb.ptr[((con.mrows + row) * con.fh + y) * fb.stride], 0, w * 4);
}

static void move_row(u32 dst, u32 src)
{
    u32 w = (con.mcols + con.max_col) * con.fw;

    for (u32 y = 0; y < con.fh; y++)
        memcpy(&fb.ptr[((con.mrows + dst) * con.fh + y) * fb.stride],
               &fb.ptr[((con.mrows + src) * con.fh + y) * fb.stride], w * 4);
    clear_row(src);
}

void fb_console_scroll(u32 n)
{
    u32 row = 0;

    if (n > con.row)
        n = con.row;
    for (; row < con.max_row - n; row++)
        move_row(row, row + n);
    for (; row < con.max_row; row++)
        clear_row(row);
    con.row -= n;
}

static void con_putchar(u8 c)
{
    if (c == '\r') {
        con.col = 0;
    } else if (c == '\n') {
        con.row++;
        con.col = 0;
    } else {
        if (c < 0x20 || c >= 0x7f)
            c = '?';

        u32 x = (con.mcols + con.col) * con.fw, y = (con.mrows + con.row) * con.fh;
        const u8 *g = con.font + (u32)(c - 0x20) * con.fw * con.fh;

        for (u32 i = 0; i < con.fh; i++)
            for (u32 j = 0; j < con.fw; j++) {
                u32 v = g[i * con.fw + j];

                fb.ptr[x + j + (y + i) * fb.stride] = (v << 16) | (v << 8) | v;
            }
        con.col++;
    }

    if (con.col == con.max_col) {
        con.row++;
        con.col = 0;
    }
    if (con.row == con.max_row) {
        fb_console_scroll(1);
        scrolled_lines++;
    }
}

int sim_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    for (char *p = buf; *p; p++)
        con_putchar((u8)*p);
    return n;
}

static void load(const char *path, u8 *dst, size_t len)
{
    FILE *f = fopen(path, "rb");

    if (!f || fread(dst, 1, len, f) != len) {
        fprintf(stderr, "cannot read %zu bytes from %s\n", len, path);
        exit(1);
    }
    fclose(f);
}

static void sim_fb_init(u32 w, u32 h, bool retina)
{
    u32 logo = retina ? 256 : 128;

    fb.width = w;
    fb.height = h;
    fb.stride = w;
    fb.depth = 30;
    fb.size = w * h * 4;
    fb.ptr = calloc(w * h, 4);
    fb.hwptr = fb.ptr;
    cur_boot_args.video.width = w;
    cur_boot_args.video.height = h;
    cur_boot_args.video.stride = w * 4;
    cur_boot_args.video.depth = 30 | (retina ? FB_DEPTH_FLAG_RETINA : 0);

    con.font = retina ? _binary_build_font_retina_bin_start : _binary_build_font_bin_start;
    con.fw = retina ? 16 : 8;
    con.fh = retina ? 32 : 16;
    con.mrows = 2;
    con.mcols = 4;
    con.max_row = h / con.fh - 2 * con.mrows;
    con.max_col = ((w - logo) / 2) / con.fw - 2 * con.mcols;

    /* m1n1's centred logo, as a grey square */
    fb_fill((w - logo) / 2, (h - logo) / 2, logo, logo, (rgb_t){90, 90, 90});
}

/* --- the scenario ------------------------------------------------------- */

static const char *const tiles[] = {"ident", "cpu", "exceptions", "memory",
                                    "devtree", "gpu", "nvme", "reboot"};
#define NT ((int)ARRAY_SIZE(tiles))

static void stage_begin(int i)
{
    char title[48];

    snprintf(title, sizeof(title), "stage %d of %d: %s", i + 1, NT - 1, tiles[i]);
    ui_page(title);
    ui_tile(i, RT_RUNNING);
}

static void stage_end(int i, enum rt_result r, const char *snap)
{
    ui_tile(i, r);
    rt_printf("\n>> %s: %s\n", tiles[i], rt_result_name(r));
    snap_name = snap;
    ui_hold(3, "next page");
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: sim_ui font.bin font_retina.bin outdir width height retina\n");
        return 2;
    }

    load(argv[1], _binary_build_font_bin_start, sizeof(_binary_build_font_bin_start));
    load(argv[2], _binary_build_font_retina_bin_start, sizeof(_binary_build_font_retina_bin_start));
    outdir = argv[3];
    sim_fb_init((u32)atoi(argv[4]), (u32)atoi(argv[5]), atoi(argv[6]) != 0);

    /* m1n1's own boot chatter, which the first page must clear. */
    for (int i = 0; i < 30; i++)
        sim_printf("m1n1 boot message %d: lorem ipsum dolor sit amet\n", i);
    scrolled_lines = 0;

    ui_init(tiles, NT);

    stage_begin(0);
    rt_printf("model         MacBook Pro (14-inch, M5 Pro, 2026)\n");
    rt_printf("target-type   J714s\n");
    rt_printf("chip-id       0x6050     board-id 0x12\n");
    rt_printf("boot core     MIDR 0x610f0640 part 0x64 (M5 Pro M-core)\n");
    rt_printf("mem_size      0x%lx (%lu MiB usable from phys_base)\n", 0xf7c000000ul, 63424ul);
    rt_printf("\nThis is T6050 (M5 Pro), the SoC this build was written for.\n");
    stage_end(0, RT_PASS, "1-ident");

    stage_begin(1);
    for (int i = 0; i < 8; i++)
        rt_printf("%-18s 0x%016lx\n", "ID_AA64PFR0_EL1", 0x1101000010111112ul + (unsigned)i);
    rt_printf("\nSecondary cores are not started. Upstream m1n1 reports that they\n"
              "power on but never run code on T6050, so this build stays on core 0.\n");
    stage_end(1, RT_WARN, NULL);

    stage_begin(2);
    rt_printf("1. guarded read of valid memory     no exception (correct)\n");
    stage_end(2, RT_PASS, NULL);

    stage_begin(3);
    rt_printf("arena         256 MiB at 0x10024000000 (16384 pages of 16 KiB)\n");
    stage_end(3, RT_PASS, NULL);

    /* The long stage: three groups of device lines force automatic paging. */
    stage_begin(4);
    for (int g = 0; g < 3; g++) {
        rt_printf("group %d:\n", g);
        for (int i = 0; i < 20; i++)
            rt_printf(" %-16.16s %-28.28s 0x%lx +0x%lx\n", "dart-sgx-long-name",
                      "dart,t6050-with-a-long-compat", 0x504000000ul + (unsigned long)i * 0x4000,
                      0x4000ul);
        rt_printf("\n");
        if (g == 0)
            snap_name = "2-devtree-page1";
    }
    stage_end(4, RT_PASS, "3-devtree-last");

    stage_begin(5);
    rt_printf("mode: device tree only (no GPU register is touched)\n\n");
    rt_printf("/arm-io/sgx  gpu,t6050\n");
    rt_printf("   reg[%d]       0x%lx +0x%lx\n", 0, 0x2100000000ul, 0x3fdc000ul);
    rt_printf("   %-12s 0x%x 0x%x\n", "clock-gates", 0x268, 0x267);
    stage_end(5, RT_PASS, "3b-gpu");

    stage_begin(6);
    rt_printf("Not built in. Rebuild with RT_NVME_PROBE=1 once the earlier stages\n"
              "pass on this machine (rung 3 in docs/TESTING.md).\n");
    stage_end(6, RT_SKIP, NULL);

    ui_page("summary");
    for (int i = 0; i < NT - 1; i++)
        rt_printf("  %d  %-12s %s\n", i + 1, tiles[i], "PASS");
    rt_printf("\nMILESTONE 1: REACHED\n");
    ui_tile(NT - 1, RT_RUNNING);
    snap_name = "4-summary";
    ui_hold(3, "reboot");

    /* A fatal exception during stage 4: m1n1 prints a register dump (which
     * bypasses our paging and may scroll), then rt_fatal_hold() runs. */
    for (int i = 0; i < 12; i++)
        sim_printf("x%-2d 0x%016lx  x%-2d 0x%016lx\n", i * 2, 0xdeadbeef00ul + (unsigned)i,
                   i * 2 + 1, 0xcafef00d00ul + (unsigned)i);
    ui_fatal(3);
    sim_printf("\n*** FATAL in m5rt %s build %s ***\n", RT_VERSION, RT_BUILD_ID);
    snapshot("5-fatal");

    fprintf(stdout, "sim: %ux%u, console %ux%u, ui_cols %u, lines lost to scrolling: %d\n",
            fb.width, fb.height, con.max_col, con.max_row, ui_cols(), scrolled_lines);

    /* The register dump after the summary is allowed to scroll; nothing our
     * own paging printed before it may. */
    return 0;
}
