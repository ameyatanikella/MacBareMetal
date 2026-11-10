/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Screen output for a machine with no serial console.
 *
 * m1n1's framebuffer console occupies the left part of the screen (left of
 * its centred logo). We print there through m1n1's printf, and use the free
 * right part for a status panel: one coloured tile per stage plus a countdown
 * bar. The tiles are readable in a blurry phone photo, which is the only
 * "log capture" available at this milestone.
 *
 * Two rules keep the output usable:
 *   - nothing scrolls away: rt_printf() counts lines and inserts a timed page
 *     break before the console would scroll;
 *   - every page carries the build id, so a photo identifies what ran.
 *
 * All drawing goes through m1n1's fb.c (fb_fill / fb_blit), so pixel format
 * and the shadow buffer stay m1n1's business.
 */

#include "rt.h"

#include <stdarg.h>

#include "fb.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"
#include "vsprintf.h"
#include "xnuboot.h"

#define UI_MAX_TILES 12

/* m1n1's console fonts: one byte of coverage per pixel, glyphs from 0x20. */
extern u8 _binary_build_font_bin_start[];
extern u8 _binary_build_font_retina_bin_start[];

static const rgb_t COL_BLACK = {0, 0, 0};
static const rgb_t COL_WHITE = {255, 255, 255};
static const rgb_t COL_DIM = {40, 40, 40};

static struct {
    bool ready; /* framebuffer present and geometry computed */
    bool panel; /* room for the status panel */

    const u8 *font;
    u32 fw, fh; /* font cell in pixels */

    u32 rows, cols; /* text area of m1n1's console */
    u32 lines, col; /* our estimate of the cursor on the current page */

    u32 px, pw;       /* panel x and width */
    u32 ty, th, tgap; /* first tile y, tile height, gap */
    u32 hy;           /* y of the hold line under the tiles */

    int ntiles;
    const char *const *names;
    enum rt_result state[UI_MAX_TILES];

    u8 *label; /* RGBX scratch for one line of panel text, pw x fh */
} ui;

const char *rt_result_name(enum rt_result r)
{
    switch (r) {
        case RT_PENDING:
            return "pending";
        case RT_RUNNING:
            return "RUNNING";
        case RT_PASS:
            return "PASS";
        case RT_WARN:
            return "WARN";
        case RT_FAIL:
            return "FAIL";
        case RT_SKIP:
            return "skipped";
    }
    return "?";
}

static rgb_t result_colour(enum rt_result r)
{
    switch (r) {
        case RT_RUNNING:
            return (rgb_t){235, 185, 0};
        case RT_PASS:
            return (rgb_t){0, 150, 60};
        case RT_WARN:
            return (rgb_t){235, 120, 0};
        case RT_FAIL:
            return (rgb_t){210, 30, 30};
        case RT_SKIP:
            return (rgb_t){50, 80, 130};
        case RT_PENDING:
        default:
            return (rgb_t){70, 70, 70};
    }
}

/* Draw one line of text into the panel at (x, y), clipped to w pixels. */
static void panel_text(u32 x, u32 y, u32 w, const char *s, rgb_t fg, rgb_t bg)
{
    if (!ui.panel || !ui.label || w > ui.pw)
        return;

    for (u32 i = 0; i < w * ui.fh; i++) {
        ui.label[i * 4 + 0] = bg.r;
        ui.label[i * 4 + 1] = bg.g;
        ui.label[i * 4 + 2] = bg.b;
        ui.label[i * 4 + 3] = 0xff;
    }

    for (u32 n = 0; s[n] && (n + 1) * ui.fw <= w; n++) {
        u8 c = (u8)s[n];

        if (c < 0x20 || c > 0x7e)
            c = '?';

        const u8 *glyph = ui.font + (u32)(c - 0x20) * ui.fw * ui.fh;

        for (u32 gy = 0; gy < ui.fh; gy++) {
            for (u32 gx = 0; gx < ui.fw; gx++) {
                u32 v = glyph[gy * ui.fw + gx];
                u8 *p = &ui.label[(gy * w + n * ui.fw + gx) * 4];

                p[0] = (u8)((fg.r * v + bg.r * (255 - v)) / 255);
                p[1] = (u8)((fg.g * v + bg.g * (255 - v)) / 255);
                p[2] = (u8)((fg.b * v + bg.b * (255 - v)) / 255);
            }
        }
    }

    fb_blit(x, y, w, ui.fh, ui.label, w, PIX_FMT_XRGB);
}

static void draw_tile(int idx)
{
    if (!ui.panel || idx < 0 || idx >= ui.ntiles)
        return;

    enum rt_result r = ui.state[idx];
    rgb_t bg = result_colour(r);
    rgb_t fg = (r == RT_RUNNING) ? COL_BLACK : COL_WHITE;
    u32 y = ui.ty + (u32)idx * (ui.th + ui.tgap);
    char text[64];

    snprintf(text, sizeof(text), "%d %-12s %s", idx + 1, ui.names[idx], rt_result_name(r));

    fb_fill(ui.px, y, ui.pw, ui.th, bg);
    panel_text(ui.px + ui.fw, y + (ui.th - ui.fh) / 2, ui.pw - 2 * ui.fw, text, fg, bg);
}

void ui_init(const char *const *tile_names, int ntiles)
{
    memset(&ui, 0, sizeof(ui));

    ui.names = tile_names;
    ui.ntiles = ntiles > UI_MAX_TILES ? UI_MAX_TILES : ntiles;

    if (!fb.ptr || !fb.width || !fb.height)
        return; /* no framebuffer: output still reaches m1n1's other consoles */

    /* Same geometry m1n1's fb_init() derives from the boot arguments. */
    bool retina = cur_boot_args.video.depth & FB_DEPTH_FLAG_RETINA;
    u32 logo = retina ? 256 : 128;

    ui.fw = retina ? 16 : 8;
    ui.fh = retina ? 32 : 16;
    ui.font = retina ? _binary_build_font_retina_bin_start : _binary_build_font_bin_start;

    if (fb.height < fb.width) {
        ui.rows = fb.height / ui.fh - 4;
        ui.cols = ((fb.width - logo) / 2) / ui.fw - 8;

        ui.px = fb.width / 2 + logo / 2 + 4 * ui.fw;
        ui.pw = fb.width - ui.px - 4 * ui.fw;
        ui.th = 2 * ui.fh;
        ui.tgap = ui.fh / 2;
        ui.ty = 3 * ui.fh; /* clears a display notch */
        ui.hy = ui.ty + (u32)ui.ntiles * (ui.th + ui.tgap) + ui.fh;

        ui.label = malloc((size_t)ui.pw * ui.fh * 4);
        ui.panel = ui.label && ui.pw >= 24 * ui.fw && ui.hy + 3 * ui.fh < fb.height;
    } else {
        ui.rows = fb.height / ui.fh;
        ui.cols = fb.width / ui.fw;
    }

    ui.ready = true;

    for (int i = 0; i < ui.ntiles; i++)
        draw_tile(i);
}

u32 ui_cols(void)
{
    return ui.ready ? ui.cols : 80;
}

void ui_tile(int idx, enum rt_result r)
{
    if (idx < 0 || idx >= ui.ntiles)
        return;

    ui.state[idx] = r;
    draw_tile(idx);
}

static void console_clear(void)
{
    if (ui.ready)
        fb_console_scroll(ui.rows); /* m1n1 clamps this to the rows in use */

    ui.lines = 0;
    ui.col = 0;
}

static void emit(const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '\n') {
            ui.lines++;
            ui.col = 0;
        } else if (ui.cols && ++ui.col >= ui.cols) {
            ui.lines++;
            ui.col = 0;
        }
    }
    printf("%s", s);
}

void ui_page(const char *title)
{
    char line[160];

    console_clear();
    snprintf(line, sizeof(line), "m5rt %s  build %s\n== %s ==\n\n", RT_VERSION, RT_BUILD_ID, title);
    emit(line);
}

int rt_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return n;

    /* Lines this text will take, counting wraps at the console width. */
    u32 need = 0, col = ui.col;

    for (const char *p = buf; *p; p++) {
        if (*p == '\n') {
            need++;
            col = 0;
        } else if (ui.cols && ++col >= ui.cols) {
            need++;
            col = 0;
        }
    }

    if (ui.ready && ui.lines + need + 2 > ui.rows) {
        ui_hold(RT_HOLD_S, "more output");
        console_clear();
        emit("(continued)\n\n");
    }

    emit(buf);
    return n;
}

void ui_hold(u32 seconds, const char *what)
{
    for (u32 left = seconds; left > 0; left--) {
        if (ui.panel) {
            char text[80];
            u32 bh = ui.fh / 2;
            u32 by = ui.hy + ui.fh + bh;

            snprintf(text, sizeof(text), "%s in %u s   ", what, left);
            panel_text(ui.px, ui.hy, ui.pw, text, COL_WHITE, COL_BLACK);
            fb_fill(ui.px, by, ui.pw, bh, COL_DIM);
            fb_fill(ui.px, by, (u32)((u64)ui.pw * left / seconds), bh, COL_WHITE);
        }

        /* Keep the hang guard from firing while we wait on purpose. */
        rt_wdt_arm(30);
        mdelay(1000);
    }

    if (ui.panel) {
        panel_text(ui.px, ui.hy, ui.pw, "", COL_WHITE, COL_BLACK);
        fb_fill(ui.px, ui.hy + ui.fh + ui.fh / 2, ui.pw, ui.fh / 2, COL_BLACK);
    }
}

void ui_fatal(int tile)
{
    if (!ui.panel)
        return;

    if (tile >= 0 && tile < ui.ntiles) {
        ui.state[tile] = RT_FAIL;
        draw_tile(tile);
    }

    rgb_t red = result_colour(RT_FAIL);

    fb_fill(ui.px, ui.hy, ui.pw, 2 * ui.fh, red);
    panel_text(ui.px + ui.fw, ui.hy + ui.fh / 2, ui.pw - 2 * ui.fw,
               "FATAL - photograph the console now", COL_WHITE, red);
}
