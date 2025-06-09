/* SPDX-License-Identifier: ISC */
/*
 * Authors: Florin-Cristian Cocolas
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <uk/console/driver.h>
#include <uk/prio.h>
#include <uk/boot/earlytab.h>
#include <uk/plat/common/efi.h>
#include <uk/plat/memory.h>
#include <efigopcons/font8x16.h>

#define FB_COLOR_BLACK      0x00000000
#define FB_COLOR_WHITE      0x00FFFFFF

#define FONT_WIDTH          8
#define FONT_HEIGHT         16

static __u32 fg_color = FB_COLOR_WHITE;
static __u32 bg_color = FB_COLOR_BLACK;

static __u32 cursor_x = 0, cursor_y = 0;

static struct uk_efi_graphics_output_protocol *gop;

static volatile __u32 *framebuffer;
static __u64 framebuffer_size;
static __u32 fb_height;
static __u32 fb_width;
static __u32 pixels_per_scanline;


static inline void fb_put_pixel(__u32 x, __u32 y, __u32 color)
{
    if (x >= fb_width || y >= fb_height)
        return;
    framebuffer[y * pixels_per_scanline + x] = color;
}

static void fb_draw_char(char c, __u32 x, __u32 y)
{
    const __u8 *glyph = font8x16[(__u8)c];

    for (__u32 row = 0; row < FONT_HEIGHT; row++) {
        __u8 line = glyph[row];
        for (__u32 col = 0; col < FONT_WIDTH; col++) {
            __u32 color = (line & (1 << (7 - col))) ? fg_color : bg_color;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

static void fb_scroll()
{
    __u32 row_bytes = pixels_per_scanline * FONT_HEIGHT;
    __u32 total_rows = fb_height / FONT_HEIGHT;

    memmove((void *)framebuffer,
            (void *)(framebuffer + row_bytes),
            (total_rows - 1) * row_bytes * sizeof(__u32));

    memset((void *)(framebuffer + (total_rows - 1) * row_bytes),
           0,
           row_bytes * sizeof(__u32));
}

static void fb_putc(char c)
{
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if ((cursor_y + 1) * FONT_HEIGHT >= fb_height)
            fb_scroll();
        return;
    }

    if (c == '\r') {
        cursor_x = 0;  // Carriage return: move cursor to start of line
        return;
    }

    fb_draw_char(c, cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT);
    cursor_x++;

    if ((cursor_x + 1) * FONT_WIDTH >= fb_width) {
        cursor_x = 0;
        cursor_y++;
        if ((cursor_y + 1) * FONT_HEIGHT >= fb_height)
            fb_scroll();
    }
}

static __ssz efi_gop_out(struct uk_console *dev __unused,
			     const char *buf, __sz len)
{
	for (__sz i = 0; i < len; i++)
		fb_putc(buf[i]);
	return len;
}

static struct uk_console_ops efi_gop_ops = { .out = efi_gop_out };

static struct uk_console efi_gop_dev;

static void efi_gop_clear_screen()
{
    for (__u32 y = 0; y < fb_height; y++) {
        for (__u32 x = 0; x < fb_width; x++) {
            fb_put_pixel(x, y, bg_color);
        }
    }
}

static int efi_gop_init(struct ukplat_bootinfo *bi)
{
    gop = (struct uk_efi_graphics_output_protocol *)bi->efi_gop;

    if (!gop || !gop->mode){
        uk_pr_err("Could not initialize the EFI Graphics Output Protocol driver\n");
        return 0;
    }

    framebuffer = (__u32 *)gop->mode->frame_buffer_base;
    framebuffer_size = gop->mode->frame_buffer_size;
    fb_width = gop->mode->info->horizontal_resolution;
    fb_height = gop->mode->info->vertical_resolution;
    pixels_per_scanline = gop->mode->info->pixels_per_scan_line;

    efi_gop_clear_screen();

	uk_console_init(&efi_gop_dev, "efigopcons", &efi_gop_ops, UK_CONSOLE_FLAG_STDOUT);
	uk_console_register(&efi_gop_dev);

	return 0;
}

UK_BOOT_EARLYTAB_ENTRY(efi_gop_init, UK_PRIO_AFTER(UK_PRIO_EARLIEST));
