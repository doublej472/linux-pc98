/* SPDX-License-Identifier: MIT */
/*
 * libpc98.h - Unified Game Development, Emulation & master.lib SDK for PC-9821 Linux
 *
 * Supports Dual Graphics Backends:
 *   1. Direct Linux Framebuffer (/dev/fb0 or /dev/fb1) with hardware V-Sync
 *   2. Native X11 Windowed / Fullscreen Mode (auto-detected when DISPLAY is set)
 *
 * Full PC-98 Hardware Architecture & master.lib Drop-in Compatibility:
 *   - Automatic Monitor Refresh Rate Detection & Target Rate Mismatch Warning
 *   - Auto Video Mode Configuration (pc98_gfx_auto_mode)
 *   - PC-98 Dual GDC Planar VRAM (Planes B, R, G, E @ 640x400 / 640x200 / 640x480 / 1280x480)
 *   - Full GRCG (Graphic Read/Write Controller) Emulation (TCR, RMW, TDW)
 *   - Geometric Drawing Primitives (grcg_line, grcg_circle, grcg_boxfill, grcg_polygon)
 *   - Palette Fading & Color Transitions (palette_black_in, palette_black_out, palette_white_in)
 *   - Built-in 8x16 PC-98 Bitmap Font & Dialogue Text Box Engine
 *   - EGC (Enhanced Graphic Charger) Hardware BitBLT & Screen Shifting
 *   - PC-98 Native Game Asset Loaders (.PI images, .CDG sprites, .DAT packfiles)
 *   - master.lib C API Drop-in Wrappers (for porting ReC98 / Touhou 1-5 / Falcom DOS games):
 *       * graph_start(), graph_end(), graph_showpage(), graph_accesspage(), graph_clear()
 *       * palette_init(), palette_show(), palette_set(), palette_black_in(), palette_black_out()
 *       * grcg_setcolor(), grcg_off(), grcg_boxfill(), grcg_line(), grcg_circle(), grcg_pset()
 *       * egc_shift_left(), egc_shift_right(), egc_shift_down(), egc_shift_up()
 *       * vsync_wait()
 *       * js_stat()
 *   - GDC Hardware Page Flipping (Page 0 vs Page 1 double buffering)
 *   - Hardware & Software V-Blank Synchronization (FBIO_WAITFORVSYNC)
 *   - OPNA YM2608 Sound & Music (PMD .M music & hardware SFX)
 *   - Multi-Device Input (DE-9 Gamepads, PC-98 Bus Mouse, Keyboard, X11 Events)
 *   - High-Resolution Microsecond Timing
 */

#ifndef LIBPC98_H
#define LIBPC98_H

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "opna_io.h"
#include "pmd_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Gamepad / Joystick Definitions (compatible with master.lib js_stat)*/
/* ------------------------------------------------------------------ */
#define PC98_BTN_UP       0x01
#define PC98_BTN_DOWN     0x02
#define PC98_BTN_LEFT     0x04
#define PC98_BTN_RIGHT    0x08
#define PC98_BTN_A        0x10
#define PC98_BTN_B        0x20
#define PC98_BTN_SLOW     0x40

#define JS_UP             PC98_BTN_UP
#define JS_DOWN           PC98_BTN_DOWN
#define JS_LEFT           PC98_BTN_LEFT
#define JS_RIGHT          PC98_BTN_RIGHT
#define JS_TRIG1          PC98_BTN_A
#define JS_TRIG2          PC98_BTN_B
#define JS_SLOW           PC98_BTN_SLOW

/* ------------------------------------------------------------------ */
/* GRCG (Graphic Read Controller & Generator) Constants               */
/* ------------------------------------------------------------------ */
#define GRCG_OFF          0x00
#define GRCG_TDW          0x40  /* Tile Direct Write */
#define GRCG_TCR          0x80  /* Tile Clear / Read */
#define GRCG_RMW          0xC0  /* Read-Modify-Write (Sprite Blit) */

#define GRCG_SETCODE      GRCG_TDW
#define GRCG_CLEAR        GRCG_TCR

#define PC98_PLANE_SIZE   0x8000 /* 32 KB per plane */
#define PC98_PITCH        80     /* 640 / 8 = 80 bytes per row */

/* ------------------------------------------------------------------ */
/* X11 Minimal Type Definitions (for dynamic dlopen loading)           */
/* ------------------------------------------------------------------ */
typedef void *XDisplayPtr;
typedef unsigned long XWindow;
typedef unsigned long XColormap;
typedef unsigned long XAtom;
typedef void *XVisualPtr;
typedef void *XGCPtr;
typedef void *XImagePtr;

typedef struct {
	int type;
	unsigned long serial;
	int send_event;
	XDisplayPtr display;
	XWindow window;
	XWindow root;
	XWindow subwindow;
	unsigned long time;
	int x, y;
	int x_root, y_root;
	unsigned int state;
	unsigned int keycode;
	int same_screen;
} XKeyEvent;

typedef union {
	int type;
	XKeyEvent xkey;
	long pad[24];
} XEvent;

/* ------------------------------------------------------------------ */
/* Graphics Context Structure                                         */
/* ------------------------------------------------------------------ */
typedef enum {
	PC98_BACKEND_FB = 0,
	PC98_BACKEND_X11 = 1
} pc98_backend_t;

typedef enum {
	PC98_MODE_NATIVE_640x400 = 0,
	PC98_MODE_DOUBLEWIDE_31K = 1,
	PC98_MODE_VGA_640x480 = 2
} pc98_mode_pref_t;

typedef struct {
	pc98_backend_t backend;
	pc98_mode_pref_t mode_pref;
	int fullscreen;
	int fb_fd;
	uint8_t *buffer;      /* Chunky 8bpp active buffer */
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	size_t buffer_size;
	uint32_t palette[256];
	uint8_t raw_palette[256][3]; /* R, G, B in 0..255 */
	struct fb_var_screeninfo var;
	struct fb_var_screeninfo orig_var;
	struct fb_fix_screeninfo fix;

	/* Refresh Rate Parameters */
	float monitor_hz;
	float target_hz;

	/* PC-98 Planar VRAM Pages (Page 0 and Page 1 for double buffering) */
	uint8_t *vram_page0[4]; /* Planes B, R, G, E */
	uint8_t *vram_page1[4];
	uint8_t current_draw_page;
	uint8_t current_disp_page;

	/* GRCG Hardware/Virtual State */
	uint8_t grcg_mode;
	uint8_t grcg_tile[4];   /* Tile registers for B, R, G, E */

	/* Dirty tracking */
	int dirty_min_y;
	int dirty_max_y;

	/* X11 backend dynamic context */
	void *x11_lib;
	XDisplayPtr x_display;
	XWindow x_window;
	XGCPtr x_gc;
	XImagePtr x_image;
	uint32_t *x_pixels;
	uint8_t x_key_state;
} pc98_gfx_t;

static pc98_gfx_t g_pc98_gfx;

static inline void pc98_gfx_flip(void);
static inline void pc98_gfx_wait_vsync(void);
static inline void pc98_gfx_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
static inline void grcg_boxfill(int x1, int y1, int x2, int y2);
static inline void grcg_set_mode(uint8_t mode);
static inline void grcg_off(void);
static inline void grcg_set_color(uint8_t color);
static inline void grcg_setcolor(uint8_t mode, uint8_t color);
static inline void grcg_blit_sprite(int x, int y, const uint8_t *mask_data, int w_bytes, int h);

/* Forward declarations of dynamic Xlib function pointers */
static XDisplayPtr (*_XOpenDisplay)(const char *);
static int (*_XCloseDisplay)(XDisplayPtr);
static XWindow (*_XCreateSimpleWindow)(XDisplayPtr, XWindow, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
static int (*_XMapWindow)(XDisplayPtr, XWindow);
static int (*_XSelectInput)(XDisplayPtr, XWindow, long);
static XGCPtr (*_XCreateGC)(XDisplayPtr, XWindow, unsigned long, void *);
static int (*_XFreeGC)(XDisplayPtr, XGCPtr);
static int (*_XDestroyWindow)(XDisplayPtr, XWindow);
static XImagePtr (*_XCreateImage)(XDisplayPtr, XVisualPtr, unsigned int, int, int, char *, unsigned int, unsigned int, int, int);
static int (*_XPutImage)(XDisplayPtr, XWindow, XGCPtr, XImagePtr, int, int, int, int, unsigned int, unsigned int);
static int (*_XPending)(XDisplayPtr);
static int (*_XNextEvent)(XDisplayPtr, XEvent *);
static int (*_XStoreName)(XDisplayPtr, XWindow, const char *);
static unsigned long (*_XDefaultRootWindow)(XDisplayPtr);
static int (*_XDefaultScreen)(XDisplayPtr);
static XVisualPtr (*_XDefaultVisual)(XDisplayPtr, int);
static int (*_XDefaultDepth)(XDisplayPtr, int);
static XAtom (*_XInternAtom)(XDisplayPtr, const char *, int);
static int (*_XChangeProperty)(XDisplayPtr, XWindow, XAtom, XAtom, int, int, const unsigned char *, int);

/* ------------------------------------------------------------------ */
/* Command-Line Argument Parser                                       */
/* ------------------------------------------------------------------ */
static inline void pc98_parse_args(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
			g_pc98_gfx.fullscreen = 1;
		} else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--windowed") == 0) {
			g_pc98_gfx.fullscreen = 0;
		} else if (strcmp(argv[i], "--doublewide") == 0 || strcmp(argv[i], "--compat") == 0) {
			g_pc98_gfx.mode_pref = PC98_MODE_DOUBLEWIDE_31K;
		} else if (strcmp(argv[i], "--native") == 0) {
			g_pc98_gfx.mode_pref = PC98_MODE_NATIVE_640x400;
		} else if (strcmp(argv[i], "--640x480") == 0) {
			g_pc98_gfx.mode_pref = PC98_MODE_VGA_640x480;
		} else if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) {
			g_pc98_gfx.target_hz = (float)atof(argv[++i]);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Refresh Rate & Video Mode Control                                  */
/* ------------------------------------------------------------------ */
static inline void pc98_gfx_set_target_hz(float hz)
{
	g_pc98_gfx.target_hz = hz;
}

static inline float pc98_gfx_get_monitor_hz(void)
{
	return g_pc98_gfx.monitor_hz;
}

/* ------------------------------------------------------------------ */
/* GRCG (Graphic Read/Write Controller) API                           */
/* ------------------------------------------------------------------ */
static inline void grcg_set_mode(uint8_t mode)
{
	g_pc98_gfx.grcg_mode = mode;
}

static inline void grcg_off(void)
{
	g_pc98_gfx.grcg_mode = GRCG_OFF;
}

static inline void grcg_set_color(uint8_t color)
{
	g_pc98_gfx.grcg_tile[0] = (color & 1) ? 0xff : 0x00; /* Plane B */
	g_pc98_gfx.grcg_tile[1] = (color & 2) ? 0xff : 0x00; /* Plane R */
	g_pc98_gfx.grcg_tile[2] = (color & 4) ? 0xff : 0x00; /* Plane G */
	g_pc98_gfx.grcg_tile[3] = (color & 8) ? 0xff : 0x00; /* Plane E */
}

static inline void grcg_setcolor(uint8_t mode, uint8_t color)
{
	grcg_set_mode(mode);
	grcg_set_color(color);
}

static inline void grcg_set_tile(uint8_t tb, uint8_t tr, uint8_t tg, uint8_t te)
{
	g_pc98_gfx.grcg_tile[0] = tb;
	g_pc98_gfx.grcg_tile[1] = tr;
	g_pc98_gfx.grcg_tile[2] = tg;
	g_pc98_gfx.grcg_tile[3] = te;
}

static inline void grcg_write_byte(uint32_t offset, uint8_t mask)
{
	uint8_t **planes = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;

	if (offset >= PC98_PLANE_SIZE || !planes[0])
		return;

	switch (g_pc98_gfx.grcg_mode) {
	case GRCG_RMW:
		planes[0][offset] = (planes[0][offset] & ~mask) | (g_pc98_gfx.grcg_tile[0] & mask);
		planes[1][offset] = (planes[1][offset] & ~mask) | (g_pc98_gfx.grcg_tile[1] & mask);
		planes[2][offset] = (planes[2][offset] & ~mask) | (g_pc98_gfx.grcg_tile[2] & mask);
		planes[3][offset] = (planes[3][offset] & ~mask) | (g_pc98_gfx.grcg_tile[3] & mask);
		break;
	case GRCG_TCR:
		planes[0][offset] = g_pc98_gfx.grcg_tile[0] & mask;
		planes[1][offset] = g_pc98_gfx.grcg_tile[1] & mask;
		planes[2][offset] = g_pc98_gfx.grcg_tile[2] & mask;
		planes[3][offset] = g_pc98_gfx.grcg_tile[3] & mask;
		break;
	case GRCG_TDW:
		planes[0][offset] = g_pc98_gfx.grcg_tile[0];
		planes[1][offset] = g_pc98_gfx.grcg_tile[1];
		planes[2][offset] = g_pc98_gfx.grcg_tile[2];
		planes[3][offset] = g_pc98_gfx.grcg_tile[3];
		break;
	default:
		planes[0][offset] = mask;
		break;
	}
}

static inline void grcg_blit_sprite(int x, int y, const uint8_t *mask_data, int w_bytes, int h)
{
	int row, col;
	for (row = 0; row < h; row++) {
		int py = y + row;
		if (py < 0 || py >= (int)g_pc98_gfx.height) continue;
		for (col = 0; col < w_bytes; col++) {
			int px_byte = (x / 8) + col;
			if (px_byte < 0 || px_byte >= PC98_PITCH) continue;
			uint32_t offset = py * PC98_PITCH + px_byte;
			uint8_t mask = mask_data[row * w_bytes + col];
			grcg_write_byte(offset, mask);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Geometric Drawing Primitives (master.lib compatible)               */
/* ------------------------------------------------------------------ */
static inline void grcg_pset(int x, int y)
{
	if ((unsigned)x < g_pc98_gfx.width && (unsigned)y < g_pc98_gfx.height) {
		uint32_t offset = y * PC98_PITCH + (x / 8);
		uint8_t mask = 1 << (7 - (x % 8));
		grcg_write_byte(offset, mask);
	}
}

static inline void grcg_line(int x0, int y0, int x1, int y1)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;

	while (1) {
		grcg_pset(x0, y0);
		if (x0 == x1 && y0 == y1) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

static inline void grcg_circle(int xc, int yc, int r)
{
	int x = 0, y = r;
	int d = 3 - 2 * r;

	while (y >= x) {
		grcg_pset(xc + x, yc + y);
		grcg_pset(xc - x, yc + y);
		grcg_pset(xc + x, yc - y);
		grcg_pset(xc - x, yc - y);
		grcg_pset(xc + y, yc + x);
		grcg_pset(xc - y, yc + x);
		grcg_pset(xc + y, yc - x);
		grcg_pset(xc - y, yc - x);
		x++;
		if (d > 0) {
			y--;
			d = d + 4 * (x - y) + 10;
		} else {
			d = d + 4 * x + 6;
		}
	}
}

/* ------------------------------------------------------------------ */
/* EGC (Enhanced Graphic Charger) Screen Shifting & BitBLT           */
/* ------------------------------------------------------------------ */
static inline void egc_shift_down(int lines)
{
	uint8_t **planes = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;
	if (!planes[0] || lines <= 0 || lines >= (int)g_pc98_gfx.height) return;

	size_t shift_bytes = lines * PC98_PITCH;
	size_t move_size = PC98_PLANE_SIZE - shift_bytes;

	for (int i = 0; i < 4; i++) {
		memmove(planes[i] + shift_bytes, planes[i], move_size);
		memset(planes[i], 0, shift_bytes);
	}
}

static inline void egc_shift_up(int lines)
{
	uint8_t **planes = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;
	if (!planes[0] || lines <= 0 || lines >= (int)g_pc98_gfx.height) return;

	size_t shift_bytes = lines * PC98_PITCH;
	size_t move_size = PC98_PLANE_SIZE - shift_bytes;

	for (int i = 0; i < 4; i++) {
		memmove(planes[i], planes[i] + shift_bytes, move_size);
		memset(planes[i] + move_size, 0, shift_bytes);
	}
}

static inline void egc_shift_left_all(int dots)
{
	uint8_t **planes = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;
	if (!planes[0] || dots <= 0) return;

	int bytes = dots / 8;
	for (int i = 0; i < 4; i++) {
		for (int y = 0; y < 400; y++) {
			uint8_t *row = planes[i] + y * PC98_PITCH;
			memmove(row, row + bytes, PC98_PITCH - bytes);
			memset(row + (PC98_PITCH - bytes), 0, bytes);
		}
	}
}

/* ------------------------------------------------------------------ */
/* GDC Page Flipping (Double Buffering)                               */
/* ------------------------------------------------------------------ */
static inline void gdc_set_draw_page(uint8_t page)
{
	g_pc98_gfx.current_draw_page = page & 1;
}

static inline void gdc_set_disp_page(uint8_t page)
{
	g_pc98_gfx.current_disp_page = page & 1;
}

static inline void gdc_flip_page(void)
{
	g_pc98_gfx.current_disp_page ^= 1;
	g_pc98_gfx.current_draw_page ^= 1;
}

/* ------------------------------------------------------------------ */
/* Ultra-Fast Chunky / Planar Conversions                             */
/* ------------------------------------------------------------------ */
static inline void pc98_planar_to_chunky(uint8_t *dst_chunky, uint8_t **src_planes, int w, int h)
{
	int y, x_byte;
	for (y = 0; y < h; y++) {
		uint8_t *dst_row = dst_chunky + y * w;
		uint32_t row_offset = y * PC98_PITCH;
		for (x_byte = 0; x_byte < PC98_PITCH && x_byte * 8 < w; x_byte++) {
			uint32_t b = src_planes[0][row_offset + x_byte];
			uint32_t r = src_planes[1][row_offset + x_byte];
			uint32_t g = src_planes[2][row_offset + x_byte];
			uint32_t e = src_planes[3][row_offset + x_byte];

			dst_row[x_byte * 8 + 0] = ((b >> 7) & 1) | (((r >> 7) & 1) << 1) | (((g >> 7) & 1) << 2) | (((e >> 7) & 1) << 3);
			dst_row[x_byte * 8 + 1] = ((b >> 6) & 1) | (((r >> 6) & 1) << 1) | (((g >> 6) & 1) << 2) | (((e >> 6) & 1) << 3);
			dst_row[x_byte * 8 + 2] = ((b >> 5) & 1) | (((r >> 5) & 1) << 1) | (((g >> 5) & 1) << 2) | (((e >> 5) & 1) << 3);
			dst_row[x_byte * 8 + 3] = ((b >> 4) & 1) | (((r >> 4) & 1) << 1) | (((g >> 4) & 1) << 2) | (((e >> 4) & 1) << 3);
			dst_row[x_byte * 8 + 4] = ((b >> 3) & 1) | (((r >> 3) & 1) << 1) | (((g >> 3) & 1) << 2) | (((e >> 3) & 1) << 3);
			dst_row[x_byte * 8 + 5] = ((b >> 2) & 1) | (((r >> 2) & 1) << 1) | (((g >> 2) & 1) << 2) | (((e >> 2) & 1) << 3);
			dst_row[x_byte * 8 + 6] = ((b >> 1) & 1) | (((r >> 1) & 1) << 1) | (((g >> 1) & 1) << 2) | (((e >> 1) & 1) << 3);
			dst_row[x_byte * 8 + 7] = (b & 1) | ((r & 1) << 1) | ((g & 1) << 2) | ((e & 1) << 3);
		}
	}
}

/* ------------------------------------------------------------------ */
/* PC-98 Master.lib / PAR (.DAT) Asset Archive Parser                 */
/* ------------------------------------------------------------------ */
#define PC98_DAT_MAX_FILES 64
#define PC98_DAT_FN_LEN    13

typedef struct {
	uint8_t  type[2];     /* "\x95\x95" if RLE-compressed */
	int8_t   aux;
	char     name[PC98_DAT_FN_LEN];
	uint32_t packsize;
	uint32_t orgsize;
	uint32_t offset;
	uint32_t reserved;
} pc98_dat_entry_t;

typedef struct {
	FILE *fp;
	char  path[256];
	int   entry_count;
	uint8_t key;
	pc98_dat_entry_t entries[PC98_DAT_MAX_FILES];
} pc98_dat_t;

static inline pc98_dat_t *pc98_dat_open(const char *game_subpath, const char *dat_name)
{
	const char *sub = game_subpath ? game_subpath : "";
	static const char *search_dirs[] = {
		".",
		"./assets",
		"./assets/%s",
		"/usr/share/touhou/%s",
		"/usr/share/touhou",
		"/usr/share/games/touhou/%s",
		"/mnt/touhou/%s",
		"/mnt/%s",
		"/mnt",
		NULL
	};

	char full_path[512];
	FILE *fp = NULL;

	for (int i = 0; search_dirs[i] != NULL; i++) {
		char dir[256];
		snprintf(dir, sizeof(dir), search_dirs[i], sub);
		snprintf(full_path, sizeof(full_path), "%s/%s", dir, dat_name);

		fp = fopen(full_path, "rb");
		if (!fp) {
			/* Try lowercase variant */
			char lower_name[64];
			for (size_t c = 0; c < sizeof(lower_name) - 1 && dat_name[c]; c++) {
				lower_name[c] = (char)tolower((unsigned char)dat_name[c]);
				lower_name[c + 1] = '\0';
			}
			snprintf(full_path, sizeof(full_path), "%s/%s", dir, lower_name);
			fp = fopen(full_path, "rb");
		}
		if (fp) break;
	}

	if (!fp) return NULL;

	pc98_dat_t *dat = (pc98_dat_t *)calloc(1, sizeof(pc98_dat_t));
	if (!dat) {
		fclose(fp);
		return NULL;
	}

	dat->fp = fp;
	strncpy(dat->path, full_path, sizeof(dat->path) - 1);

	/* Read file headers (64 entries * 32 bytes) */
	for (int i = 0; i < PC98_DAT_MAX_FILES; i++) {
		uint8_t raw[32];
		if (fread(raw, 1, 32, fp) != 32) break;
		if (raw[0] == 0 && raw[1] == 0) break;

		dat->entries[i].type[0] = raw[0];
		dat->entries[i].type[1] = raw[1];
		dat->entries[i].aux     = (int8_t)raw[2];

		/* Inverted filename */
		for (int c = 0; c < PC98_DAT_FN_LEN; c++) {
			uint8_t ch = raw[3 + c];
			if (ch == 0) {
				dat->entries[i].name[c] = '\0';
				break;
			}
			dat->entries[i].name[c] = (char)(~ch);
		}
		dat->entries[i].name[PC98_DAT_FN_LEN - 1] = '\0';

		dat->entries[i].packsize = raw[16] | (raw[17] << 8) | (raw[18] << 16) | (raw[19] << 24);
		dat->entries[i].orgsize  = raw[20] | (raw[21] << 8) | (raw[22] << 16) | (raw[23] << 24);
		dat->entries[i].offset   = raw[24] | (raw[25] << 8) | (raw[26] << 16) | (raw[27] << 24);
		dat->entry_count++;
	}

	return dat;
}

static inline void pc98_dat_close(pc98_dat_t *dat)
{
	if (!dat) return;
	if (dat->fp) fclose(dat->fp);
	free(dat);
}

static inline uint8_t *pc98_dat_read_file(pc98_dat_t *dat, const char *entry_name, size_t *out_size)
{
	if (!dat || !dat->fp || !entry_name) return NULL;

	int found_idx = -1;
	for (int i = 0; i < dat->entry_count; i++) {
		if (strcasecmp(dat->entries[i].name, entry_name) == 0) {
			found_idx = i;
			break;
		}
	}

	if (found_idx < 0) return NULL;

	pc98_dat_entry_t *entry = &dat->entries[found_idx];
	if (entry->packsize == 0 || entry->orgsize == 0) return NULL;

	fseek(dat->fp, (long)entry->offset, SEEK_SET);

	uint8_t *packed = (uint8_t *)malloc(entry->packsize + 64);
	if (!packed) return NULL;

	if (fread(packed, 1, entry->packsize, dat->fp) != entry->packsize) {
		free(packed);
		return NULL;
	}

	/* Check if RLE compressed (type == 0x9595 / 封) */
	if (entry->type[0] == 0x95 && entry->type[1] == 0x95) {
		uint8_t *unpacked = (uint8_t *)malloc(entry->orgsize + 64);
		if (!unpacked) {
			free(packed);
			return NULL;
		}

		size_t r_pos = 0;
		size_t w_pos = 0;
		uint8_t lit2 = packed[r_pos++];

		while (r_pos < entry->packsize && w_pos < entry->orgsize) {
			uint8_t lit1;
			do {
				lit1 = unpacked[w_pos++] = lit2;
				if (r_pos >= entry->packsize || w_pos >= entry->orgsize) break;
				lit2 = packed[r_pos++];
			} while (lit1 != lit2 && r_pos < entry->packsize && w_pos < entry->orgsize);

			if (w_pos < entry->orgsize) unpacked[w_pos++] = lit2;

			while (r_pos < entry->packsize && w_pos < entry->orgsize) {
				uint8_t runs = packed[r_pos++];
				while (runs > 0 && w_pos < entry->orgsize) {
					unpacked[w_pos++] = lit1;
					runs--;
				}
				if (r_pos >= entry->packsize) break;
				lit2 = packed[r_pos++];
				if (lit2 != lit1) break;
				if (w_pos < entry->orgsize) unpacked[w_pos++] = lit1;
			}
		}

		free(packed);
		if (out_size) *out_size = entry->orgsize;
		return unpacked;
	}

	/* Plain encrypted file: XOR with dat->key (0) */
	if (out_size) *out_size = entry->packsize;
	return packed;
}

/* ------------------------------------------------------------------ */
/* PC-98 CDG Animated Sprite Sheet Decoder                            */
/* ------------------------------------------------------------------ */
typedef struct {
	uint16_t bitplane_size;
	int16_t  pixel_w;
	int16_t  pixel_h;
	int16_t  offset_at_bottom_left;
	uint16_t vram_dword_w;
	uint8_t  image_count;
	int8_t   plane_layout; /* 0 = Colors, 1 = Colors + Alpha, 2 = Alpha */
	uint8_t *alpha_plane;  /* Alpha transparency mask */
	uint8_t *planes[4];    /* Planes B, R, G, E */
} pc98_cdg_t;

static inline int pc98_cdg_load_from_memory(const uint8_t *buf, size_t size, pc98_cdg_t *cdg)
{
	if (!buf || size < 16) return -1;

	cdg->bitplane_size = buf[0] | (buf[1] << 8);
	cdg->pixel_w       = buf[2] | (buf[3] << 8);
	cdg->pixel_h       = buf[4] | (buf[5] << 8);
	cdg->offset_at_bottom_left = buf[6] | (buf[7] << 8);
	cdg->vram_dword_w   = buf[8] | (buf[9] << 8);
	cdg->image_count    = buf[10];
	cdg->plane_layout   = (int8_t)buf[11];

	if (cdg->bitplane_size == 0) {
		cdg->bitplane_size = (cdg->pixel_w / 8) * cdg->pixel_h;
	}

	size_t plane_bytes = cdg->bitplane_size;
	const uint8_t *src = buf + 16;

	cdg->alpha_plane = (uint8_t *)malloc(plane_bytes);
	for (int i = 0; i < 4; i++) {
		cdg->planes[i] = (uint8_t *)malloc(plane_bytes);
	}

	if (cdg->plane_layout == 1 && (src + plane_bytes * 5 <= buf + size)) {
		/* Colors + Alpha */
		memcpy(cdg->alpha_plane, src, plane_bytes); src += plane_bytes;
		for (int i = 0; i < 4; i++) {
			memcpy(cdg->planes[i], src, plane_bytes); src += plane_bytes;
		}
	} else if (src + plane_bytes * 4 <= buf + size) {
		/* Colors Only (Alpha is 0xFF solid) */
		memset(cdg->alpha_plane, 0xFF, plane_bytes);
		for (int i = 0; i < 4; i++) {
			memcpy(cdg->planes[i], src, plane_bytes); src += plane_bytes;
		}
	} else {
		if (cdg->alpha_plane) { free(cdg->alpha_plane); cdg->alpha_plane = NULL; }
		for (int i = 0; i < 4; i++) {
			if (cdg->planes[i]) { free(cdg->planes[i]); cdg->planes[i] = NULL; }
		}
		return -1;
	}

	return 0;
}

static inline void pc98_cdg_put(int x, int y, const pc98_cdg_t *cdg)
{
	uint8_t **vram = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;
	if (!vram[0] || !cdg || !cdg->planes[0]) return;

	int stride_bytes = cdg->pixel_w / 8;
	int screen_stride = 640 / 8;
	int x_byte = x / 8;

	for (int r = 0; r < cdg->pixel_h; r++) {
		int dst_y = y + r;
		if (dst_y < 0 || dst_y >= (int)g_pc98_gfx.height) continue;
		int dst_off = dst_y * screen_stride + x_byte;
		int src_off = r * stride_bytes;

		for (int b = 0; b < stride_bytes; b++) {
			if (dst_off + b < 0 || dst_off + b >= (int)PC98_PLANE_SIZE) continue;
			uint8_t alpha = cdg->alpha_plane ? cdg->alpha_plane[src_off + b] : 0xFF;
			if (alpha == 0) continue;

			for (int p = 0; p < 4; p++) {
				uint8_t src_val = cdg->planes[p][src_off + b];
				vram[p][dst_off + b] = (vram[p][dst_off + b] & ~alpha) | (src_val & alpha);
			}
		}
	}
}

static inline void pc98_cdg_free(pc98_cdg_t *cdg)
{
	if (!cdg) return;
	if (cdg->alpha_plane) free(cdg->alpha_plane);
	for (int i = 0; i < 4; i++) {
		if (cdg->planes[i]) free(cdg->planes[i]);
		cdg->planes[i] = NULL;
	}
	cdg->alpha_plane = NULL;
}

/* ------------------------------------------------------------------ */
/* Strict Original Game Asset Verification                            */
/* ------------------------------------------------------------------ */
static inline void pc98_require_assets(const char *game_title, const char *game_id,
                                      const char **required_dats, int count)
{
	int missing = 0;
	for (int i = 0; i < count; i++) {
		pc98_dat_t *dat = pc98_dat_open(game_id, required_dats[i]);
		if (!dat) {
			if (missing == 0) {
				fprintf(stderr, "\n==================================================================\n");
				fprintf(stderr, "[ERROR] %s Requires Original PC-98 Game Assets!\n", game_title);
				fprintf(stderr, "==================================================================\n");
				fprintf(stderr, "The game engine strictly requires authentic retail PC-98 assets.\n");
				fprintf(stderr, "Please place the original .DAT archives in one of these locations:\n");
				fprintf(stderr, "  • /usr/share/touhou/%s/\n", game_id);
				fprintf(stderr, "  • ./assets/%s/\n", game_id);
				fprintf(stderr, "  • ./\n\n");
				fprintf(stderr, "Missing Archive(s):\n");
			}
			fprintf(stderr, "  [!] %s (NOT FOUND)\n", required_dats[i]);
			missing++;
		} else {
			pc98_dat_close(dat);
		}
	}

	if (missing > 0) {
		fprintf(stderr, "\nTo dump assets from original PC-98 floppy disks / HDI images:\n");
		fprintf(stderr, "  mkdir -p /usr/share/touhou/%s\n", game_id);
		fprintf(stderr, "  cp /path/to/%s/*.DAT /usr/share/touhou/%s/\n", game_id, game_id);
		fprintf(stderr, "==================================================================\n\n");
		exit(1);
	}
}

/* ------------------------------------------------------------------ */
/* PC-98 Asset Decoders (.PI 16-Color Image & .CDG Sprites)           */
/* ------------------------------------------------------------------ */
typedef struct {
	uint16_t width;
	uint16_t height;
	uint8_t  palette[16 * 3];
	uint8_t *planes[4];
} pc98_pi_image_t;

static inline int pc98_pi_load_from_memory(const uint8_t *buf, size_t size, pc98_pi_image_t *img)
{
	if (!buf || size < 60) return -1;

	img->width  = buf[8] | (buf[9] << 8);
	img->height = buf[10] | (buf[11] << 8);
	if (img->width == 0 || img->height == 0) {
		img->width = 640;
		img->height = 400;
	}

	memcpy(img->palette, buf + 12, 48);

	for (int i = 0; i < 4; i++) {
		img->planes[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
	}

	const uint8_t *src = buf + 60;
	size_t plane_bytes = (img->width / 8) * img->height;
	if (plane_bytes > PC98_PLANE_SIZE) plane_bytes = PC98_PLANE_SIZE;

	for (int i = 0; i < 4; i++) {
		if (src + plane_bytes <= buf + size) {
			memcpy(img->planes[i], src, plane_bytes);
			src += plane_bytes;
		}
	}

	return 0;
}

static inline void pc98_apply_pi_palette(const pc98_pi_image_t *img)
{
	if (!img) return;
	for (int i = 0; i < 16; i++) {
		uint8_t r = img->palette[i * 3 + 0];
		uint8_t g = img->palette[i * 3 + 1];
		uint8_t b = img->palette[i * 3 + 2];
		if (r <= 15 && g <= 15 && b <= 15) {
			r = (uint8_t)(r * 17);
			g = (uint8_t)(g * 17);
			b = (uint8_t)(b * 17);
		}
		pc98_gfx_set_palette((uint8_t)i, r, g, b);
	}
}

static inline int pc98_pi_load(const char *filename, pc98_pi_image_t *img)
{
	FILE *fp = fopen(filename, "rb");
	if (!fp) return -1;

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (sz < 60) {
		fclose(fp);
		return -1;
	}

	uint8_t *buf = (uint8_t *)malloc(sz);
	if (!buf) {
		fclose(fp);
		return -1;
	}

	if (fread(buf, 1, sz, fp) != (size_t)sz) {
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);

	int rc = pc98_pi_load_from_memory(buf, (size_t)sz, img);
	free(buf);
	return rc;
}

static inline void pc98_pi_free(pc98_pi_image_t *img)
{
	for (int i = 0; i < 4; i++) {
		if (img->planes[i]) free(img->planes[i]);
		img->planes[i] = NULL;
	}
}

static inline void pc98_pi_put(int x, int y, const pc98_pi_image_t *img)
{
	uint8_t **planes = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;
	if (!planes[0] || !img->planes[0]) return;

	if (x == 0 && y == 0 && img->width == 640 && img->height == 400) {
		for (int p = 0; p < 4; p++) {
			memcpy(planes[p], img->planes[p], PC98_PLANE_SIZE);
		}
		return;
	}

	int img_stride = img->width / 8;
	int screen_stride = 640 / 8;
	int x_byte = x / 8;
	int max_rows = (img->height < (400 - y)) ? img->height : (400 - y);
	int max_cols = (img_stride < (screen_stride - x_byte)) ? img_stride : (screen_stride - x_byte);

	if (y < 0 || x < 0 || max_rows <= 0 || max_cols <= 0) return;

	for (int p = 0; p < 4; p++) {
		for (int r = 0; r < max_rows; r++) {
			int dst_off = (y + r) * screen_stride + x_byte;
			int src_off = r * img_stride;
			if (dst_off >= 0 && dst_off + max_cols <= (int)PC98_PLANE_SIZE) {
				memcpy(&planes[p][dst_off], &img->planes[p][src_off], max_cols);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Built-in 8x16 PC-98 Bitmap Font & Text Renderer                    */
/* ------------------------------------------------------------------ */
static const uint8_t pc98_font8x16[128][16] = {
	[' '] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['!'] = { 0x00,0x00,0x18,0x3c,0x3c,0x3c,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00 },
	['"'] = { 0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['#'] = { 0x00,0x00,0x6c,0x6c,0xfe,0x6c,0x6c,0x6c,0xfe,0x6c,0x6c,0x00,0x00,0x00,0x00,0x00 },
	['$'] = { 0x18,0x18,0x7c,0xc6,0xc0,0x7c,0x06,0x06,0xc6,0x7c,0x18,0x18,0x00,0x00,0x00,0x00 },
	['%'] = { 0x00,0x00,0xc6,0xcc,0x18,0x30,0x60,0xc6,0x8c,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['&'] = { 0x00,0x38,0x6c,0x68,0x70,0xd8,0xcc,0xcc,0xce,0x7b,0x00,0x00,0x00,0x00,0x00,0x00 },
	['\''] = { 0x00,0x18,0x18,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['('] = { 0x00,0x0c,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00,0x00 },
	[')'] = { 0x00,0x30,0x18,0x0c,0x0c,0x0c,0x0c,0x0c,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00 },
	['*'] = { 0x00,0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['+'] = { 0x00,0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	[','] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10,0x20,0x00,0x00,0x00,0x00,0x00 },
	['-'] = { 0x00,0x00,0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['.'] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
	['/'] = { 0x00,0x02,0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['0'] = { 0x00,0x3c,0x66,0xc3,0xc7,0xcb,0xd3,0xe3,0x66,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['1'] = { 0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00,0x00,0x00,0x00,0x00 },
	['2'] = { 0x00,0x7c,0xc6,0x06,0x0c,0x18,0x30,0x60,0xc6,0xfe,0x00,0x00,0x00,0x00,0x00,0x00 },
	['3'] = { 0x00,0x7c,0xc6,0x06,0x06,0x3c,0x06,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['4'] = { 0x00,0x0c,0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x0c,0x1e,0x00,0x00,0x00,0x00,0x00,0x00 },
	['5'] = { 0x00,0xfe,0xc0,0xc0,0xfc,0x06,0x06,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['6'] = { 0x00,0x38,0x60,0xc0,0xfc,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['7'] = { 0x00,0xfe,0xc6,0x06,0x0c,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00 },
	['8'] = { 0x00,0x7c,0xc6,0xc6,0xc6,0x7c,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['9'] = { 0x00,0x7c,0xc6,0xc6,0xc6,0x7e,0x06,0x06,0x0c,0x78,0x00,0x00,0x00,0x00,0x00,0x00 },
	[':'] = { 0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	[';'] = { 0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x10,0x20,0x00,0x00,0x00,0x00,0x00 },
	['<'] = { 0x00,0x06,0x1c,0x70,0xc0,0x70,0x1c,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['='] = { 0x00,0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['>'] = { 0x00,0x60,0x38,0x0e,0x03,0x0e,0x38,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['?'] = { 0x00,0x7c,0xc6,0x0c,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
	['@'] = { 0x00,0x7c,0xc6,0xde,0xde,0xdc,0xd8,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['A'] = { 0x00,0x18,0x3c,0x66,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['B'] = { 0x00,0xfc,0x66,0x66,0x66,0x7c,0x66,0x66,0x66,0xfc,0x00,0x00,0x00,0x00,0x00,0x00 },
	['C'] = { 0x00,0x3c,0x66,0xc3,0xc0,0xc0,0xc0,0xc3,0x66,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['D'] = { 0x00,0xf8,0x6c,0x66,0x66,0x66,0x66,0x66,0x6c,0xf8,0x00,0x00,0x00,0x00,0x00,0x00 },
	['E'] = { 0x00,0xfe,0x62,0x62,0x68,0x78,0x68,0x62,0x62,0xfe,0x00,0x00,0x00,0x00,0x00,0x00 },
	['F'] = { 0x00,0xfe,0x62,0x62,0x68,0x78,0x68,0x60,0x60,0xf0,0x00,0x00,0x00,0x00,0x00,0x00 },
	['G'] = { 0x00,0x3c,0x66,0xc3,0xc0,0xc0,0xce,0xc3,0x66,0x3a,0x00,0x00,0x00,0x00,0x00,0x00 },
	['H'] = { 0x00,0xc3,0xc3,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['I'] = { 0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00,0x00,0x00,0x00,0x00 },
	['J'] = { 0x00,0x1e,0x0c,0x0c,0x0c,0x0c,0x0c,0xcc,0xcc,0x78,0x00,0x00,0x00,0x00,0x00,0x00 },
	['K'] = { 0x00,0xe3,0x66,0x6c,0x78,0x70,0x78,0x6c,0x66,0xe3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['L'] = { 0x00,0xf0,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xfe,0x00,0x00,0x00,0x00,0x00,0x00 },
	['M'] = { 0x00,0xc3,0xe7,0xff,0xdb,0xc3,0xc3,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['N'] = { 0x00,0xc3,0xe3,0xf3,0xdb,0xcb,0xc7,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['O'] = { 0x00,0x3c,0x66,0xc3,0xc3,0xc3,0xc3,0xc3,0x66,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['P'] = { 0x00,0xfc,0x66,0x66,0x66,0x7c,0x60,0x60,0x60,0xf0,0x00,0x00,0x00,0x00,0x00,0x00 },
	['Q'] = { 0x00,0x3c,0x66,0xc3,0xc3,0xc3,0xc3,0xdb,0x66,0x3c,0x0e,0x00,0x00,0x00,0x00,0x00 },
	['R'] = { 0x00,0xfc,0x66,0x66,0x66,0x7c,0x6c,0x66,0x63,0xe3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['S'] = { 0x00,0x78,0xcc,0x84,0xc0,0x78,0x0c,0x46,0xcc,0x78,0x00,0x00,0x00,0x00,0x00,0x00 },
	['T'] = { 0x00,0x7e,0x5a,0x18,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['U'] = { 0x00,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00 },
	['V'] = { 0x00,0xc3,0xc3,0xc3,0xc3,0xc3,0x66,0x66,0x3c,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
	['W'] = { 0x00,0xc3,0xc3,0xc3,0xc3,0xc3,0xdb,0xff,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00 },
	['X'] = { 0x00,0xc3,0xc3,0x66,0x3c,0x18,0x3c,0x66,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['Y'] = { 0x00,0xc3,0xc3,0x66,0x3c,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['Z'] = { 0x00,0xfe,0xc6,0x86,0x0c,0x18,0x30,0x61,0xc3,0xfe,0x00,0x00,0x00,0x00,0x00,0x00 },
	['['] = { 0x00,0x3c,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['\\'] = { 0x00,0x80,0x60,0x30,0x18,0x0c,0x06,0x03,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	[']'] = { 0x00,0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['^'] = { 0x08,0x1c,0x36,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
	['_'] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00 },
	['a'] = { 0x00,0x00,0x00,0x78,0x0c,0x7c,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00,0x00,0x00 },
	['b'] = { 0x00,0xe0,0x60,0x7c,0x66,0x66,0x66,0x66,0x66,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['c'] = { 0x00,0x00,0x00,0x3c,0x66,0xc0,0xc0,0xc0,0x66,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['d'] = { 0x00,0x1c,0x0c,0x3c,0x6c,0xcc,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00,0x00,0x00 },
	['e'] = { 0x00,0x00,0x00,0x7c,0xc6,0xfe,0xc0,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['f'] = { 0x00,0x1c,0x36,0x30,0x7c,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00,0x00 },
	['g'] = { 0x00,0x00,0x00,0x76,0xcc,0xcc,0xcc,0xcc,0x7c,0x0c,0xcc,0x78,0x00,0x00,0x00,0x00 },
	['h'] = { 0x00,0xe0,0x60,0x6c,0x76,0x66,0x66,0x66,0x66,0xe7,0x00,0x00,0x00,0x00,0x00,0x00 },
	['i'] = { 0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['j'] = { 0x00,0x06,0x06,0x00,0x0e,0x06,0x06,0x06,0x06,0x06,0x66,0x3c,0x00,0x00,0x00,0x00 },
	['k'] = { 0x00,0xe0,0x60,0x66,0x6c,0x78,0x78,0x6c,0x66,0xe7,0x00,0x00,0x00,0x00,0x00,0x00 },
	['l'] = { 0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['m'] = { 0x00,0x00,0x00,0xe6,0xdb,0xdb,0xdb,0xdb,0xdb,0xdb,0x00,0x00,0x00,0x00,0x00,0x00 },
	['n'] = { 0x00,0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00 },
	['o'] = { 0x00,0x00,0x00,0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['p'] = { 0x00,0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x7c,0x60,0x60,0xf0,0x00,0x00,0x00,0x00 },
	['q'] = { 0x00,0x00,0x00,0x76,0xcc,0xcc,0xcc,0xcc,0x7c,0x0c,0x0c,0x1e,0x00,0x00,0x00,0x00 },
	['r'] = { 0x00,0x00,0x00,0xdc,0x76,0x66,0x60,0x60,0x60,0xf0,0x00,0x00,0x00,0x00,0x00,0x00 },
	['s'] = { 0x00,0x00,0x00,0x7c,0xc6,0x70,0x1c,0x06,0x86,0x7c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['t'] = { 0x00,0x10,0x30,0x7c,0x30,0x30,0x30,0x30,0x36,0x1c,0x00,0x00,0x00,0x00,0x00,0x00 },
	['u'] = { 0x00,0x00,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00,0x00,0x00 },
	['v'] = { 0x00,0x00,0x00,0xc3,0xc3,0xc3,0x66,0x66,0x3c,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
	['w'] = { 0x00,0x00,0x00,0xc3,0xc3,0xdb,0xdb,0xff,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00 },
	['x'] = { 0x00,0x00,0x00,0xc3,0x66,0x3c,0x18,0x3c,0x66,0xc3,0x00,0x00,0x00,0x00,0x00,0x00 },
	['y'] = { 0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0x7e,0x06,0x0c,0x78,0x00,0x00,0x00,0x00 },
	['z'] = { 0x00,0x00,0x00,0xfe,0xcc,0x18,0x30,0x60,0xc6,0xfe,0x00,0x00,0x00,0x00,0x00,0x00 },
	['{'] = { 0x00,0x0e,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0e,0x00,0x00,0x00,0x00,0x00,0x00 },
	['|'] = { 0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
	['}'] = { 0x00,0x70,0x18,0x18,0x18,0x0e,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00,0x00 },
	['~'] = { 0x00,0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }
};

static inline void pc98_draw_char(int x, int y, char c, uint8_t color)
{
	unsigned char uc = (unsigned char)c;
	if (uc >= 128) return;
	const uint8_t *glyph = pc98_font8x16[uc];
	grcg_setcolor(GRCG_RMW, color);
	grcg_blit_sprite(x, y, glyph, 1, 16);
	grcg_off();
}

static inline void pc98_draw_string(int x, int y, const char *str, uint8_t color)
{
	int px = x;
	while (*str) {
		if (*str == '\n') {
			px = x;
			y += 18;
		} else {
			pc98_draw_char(px, y, *str, color);
			px += 8;
		}
		str++;
	}
}

static inline void pc98_draw_string_shadow(int x, int y, const char *str, uint8_t color, uint8_t shadow_color)
{
	pc98_draw_string(x + 1, y + 1, str, shadow_color);
	pc98_draw_string(x, y, str, color);
}

static inline void pc98_draw_dialog_box(int x, int y, int w, int h, uint8_t bg_color, uint8_t border_color)
{
	/* Fill dialog background */
	grcg_setcolor(GRCG_TCR, bg_color);
	grcg_boxfill(x, y, x + w, y + h);
	/* Outer double border */
	grcg_setcolor(GRCG_TCR, border_color);
	grcg_boxfill(x, y, x + w, y + 2);
	grcg_boxfill(x, y + h - 2, x + w, y + h);
	grcg_boxfill(x, y, x + 2, y + h);
	grcg_boxfill(x + w - 2, y, x + w, y + h);
	grcg_off();
}

/* ------------------------------------------------------------------ */
/* Palette Fading & Color Transitions                                 */
/* ------------------------------------------------------------------ */
static inline void pc98_gfx_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	g_pc98_gfx.raw_palette[index][0] = r;
	g_pc98_gfx.raw_palette[index][1] = g;
	g_pc98_gfx.raw_palette[index][2] = b;
	g_pc98_gfx.palette[index] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

	if (g_pc98_gfx.backend == PC98_BACKEND_FB && g_pc98_gfx.fb_fd >= 0) {
		uint16_t red = (uint16_t)r << 8;
		uint16_t green = (uint16_t)g << 8;
		uint16_t blue = (uint16_t)b << 8;
		struct fb_cmap cmap = {
			.start = index,
			.len = 1,
			.red = &red,
			.green = &green,
			.blue = &blue,
			.transp = NULL
		};
		(void)ioctl(g_pc98_gfx.fb_fd, FBIOPUTCMAP, &cmap);
	}
}

static inline void palette_black_out(int steps)
{
	if (steps <= 0) steps = 16;
	for (int s = steps; s >= 0; s--) {
		float factor = (float)s / (float)steps;
		for (int i = 0; i < 16; i++) {
			uint8_t r = (uint8_t)(g_pc98_gfx.raw_palette[i][0] * factor);
			uint8_t g = (uint8_t)(g_pc98_gfx.raw_palette[i][1] * factor);
			uint8_t b = (uint8_t)(g_pc98_gfx.raw_palette[i][2] * factor);
			pc98_gfx_set_palette(i, r, g, b);
		}
		pc98_gfx_wait_vsync();
	}
}

static inline void palette_black_in(int steps)
{
	if (steps <= 0) steps = 16;
	for (int s = 0; s <= steps; s++) {
		float factor = (float)s / (float)steps;
		for (int i = 0; i < 16; i++) {
			uint8_t r = (uint8_t)(g_pc98_gfx.raw_palette[i][0] * factor);
			uint8_t g = (uint8_t)(g_pc98_gfx.raw_palette[i][1] * factor);
			uint8_t b = (uint8_t)(g_pc98_gfx.raw_palette[i][2] * factor);
			pc98_gfx_set_palette(i, r, g, b);
		}
		pc98_gfx_wait_vsync();
	}
}

/* ------------------------------------------------------------------ */
/* Initialization & Display Backends                                  */
/* ------------------------------------------------------------------ */
static int pc98_init_x11(const char *title, uint32_t w, uint32_t h)
{
	g_pc98_gfx.x11_lib = dlopen("libX11.so.6", RTLD_LAZY);
	if (!g_pc98_gfx.x11_lib)
		g_pc98_gfx.x11_lib = dlopen("libX11.so", RTLD_LAZY);
	if (!g_pc98_gfx.x11_lib)
		return -1;

	#define LOAD_SYM(name) do { \
		_##name = (typeof(_##name))dlsym(g_pc98_gfx.x11_lib, #name); \
		if (!_##name) { dlclose(g_pc98_gfx.x11_lib); g_pc98_gfx.x11_lib = NULL; return -1; } \
	} while (0)

	LOAD_SYM(XOpenDisplay);
	LOAD_SYM(XCloseDisplay);
	LOAD_SYM(XCreateSimpleWindow);
	LOAD_SYM(XMapWindow);
	LOAD_SYM(XSelectInput);
	LOAD_SYM(XCreateGC);
	LOAD_SYM(XFreeGC);
	LOAD_SYM(XDestroyWindow);
	LOAD_SYM(XCreateImage);
	LOAD_SYM(XPutImage);
	LOAD_SYM(XPending);
	LOAD_SYM(XNextEvent);
	LOAD_SYM(XStoreName);
	LOAD_SYM(XDefaultRootWindow);
	LOAD_SYM(XDefaultScreen);
	LOAD_SYM(XDefaultVisual);
	LOAD_SYM(XDefaultDepth);
	LOAD_SYM(XInternAtom);
	LOAD_SYM(XChangeProperty);
	#undef LOAD_SYM

	g_pc98_gfx.x_display = _XOpenDisplay(NULL);
	if (!g_pc98_gfx.x_display)
		return -1;

	int screen = _XDefaultScreen(g_pc98_gfx.x_display);
	XWindow root = _XDefaultRootWindow(g_pc98_gfx.x_display);

	g_pc98_gfx.x_window = _XCreateSimpleWindow(g_pc98_gfx.x_display, root,
						   50, 50, w, h, 1, 0, 0);
	if (!g_pc98_gfx.x_window) {
		_XCloseDisplay(g_pc98_gfx.x_display);
		return -1;
	}

	_XStoreName(g_pc98_gfx.x_display, g_pc98_gfx.x_window,
		    title ? title : "PC-9800 Emulator / Game Window");
	_XSelectInput(g_pc98_gfx.x_display, g_pc98_gfx.x_window,
		      (1L << 0) | (1L << 1) | (1L << 2) | (1L << 3));

	if (g_pc98_gfx.fullscreen) {
		XAtom wm_state = _XInternAtom(g_pc98_gfx.x_display, "_NET_WM_STATE", 0);
		XAtom wm_fullscreen = _XInternAtom(g_pc98_gfx.x_display, "_NET_WM_STATE_FULLSCREEN", 0);
		if (wm_state && wm_fullscreen) {
			_XChangeProperty(g_pc98_gfx.x_display, g_pc98_gfx.x_window,
					 wm_state, 4, 32, 2,
					 (unsigned char *)&wm_fullscreen, 1);
		}
	}

	_XMapWindow(g_pc98_gfx.x_display, g_pc98_gfx.x_window);
	g_pc98_gfx.x_gc = _XCreateGC(g_pc98_gfx.x_display, g_pc98_gfx.x_window, 0, NULL);

	g_pc98_gfx.x_pixels = (uint32_t *)calloc(w * h, sizeof(uint32_t));
	g_pc98_gfx.x_image = _XCreateImage(g_pc98_gfx.x_display,
					   _XDefaultVisual(g_pc98_gfx.x_display, screen),
					   _XDefaultDepth(g_pc98_gfx.x_display, screen),
					   2, 0,
					   (char *)g_pc98_gfx.x_pixels,
					   w, h, 32, w * 4);

	g_pc98_gfx.width = w;
	g_pc98_gfx.height = h;
	g_pc98_gfx.pitch = w;
	g_pc98_gfx.bpp = 8;
	g_pc98_gfx.buffer_size = w * h;
	g_pc98_gfx.buffer = (uint8_t *)calloc(1, g_pc98_gfx.buffer_size);
	g_pc98_gfx.monitor_hz = 60.0f;

	for (int i = 0; i < 4; i++) {
		g_pc98_gfx.vram_page0[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
		g_pc98_gfx.vram_page1[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
	}

	g_pc98_gfx.backend = PC98_BACKEND_X11;
	return 0;
}

static inline int pc98_gfx_open(const char *device)
{
	if (g_pc98_gfx.target_hz == 0.0f) {
		g_pc98_gfx.backend = PC98_BACKEND_FB;
		g_pc98_gfx.mode_pref = PC98_MODE_NATIVE_640x400;
		g_pc98_gfx.fb_fd = -1;
		g_pc98_gfx.monitor_hz = 60.0f;
		g_pc98_gfx.target_hz = 56.42f;
		g_pc98_gfx.dirty_max_y = 400;
	}
	const char *disp = getenv("DISPLAY");
	if (disp && disp[0] != '\0') {
		if (pc98_init_x11("PC-9821 Game Surface", 640, 400) == 0) {
			return 0;
		}
	}

	const char *fb_dev = device ? device : "/dev/fb0";
	g_pc98_gfx.fb_fd = open(fb_dev, O_RDWR);
	if (g_pc98_gfx.fb_fd < 0) {
		g_pc98_gfx.fb_fd = open("/dev/fb1", O_RDWR);
		if (g_pc98_gfx.fb_fd < 0) {
			fprintf(stderr, "libpc98: cannot open framebuffer: %s\n", strerror(errno));
			return -1;
		}
	}

	if (ioctl(g_pc98_gfx.fb_fd, FBIOGET_FSCREENINFO, &g_pc98_gfx.fix) < 0 ||
	    ioctl(g_pc98_gfx.fb_fd, FBIOGET_VSCREENINFO, &g_pc98_gfx.var) < 0) {
		fprintf(stderr, "libpc98: failed to get screen info: %s\n", strerror(errno));
		close(g_pc98_gfx.fb_fd);
		g_pc98_gfx.fb_fd = -1;
		return -1;
	}

	g_pc98_gfx.orig_var = g_pc98_gfx.var;

	uint32_t target_w = 640, target_h = 400;
	if (g_pc98_gfx.mode_pref == PC98_MODE_DOUBLEWIDE_31K) {
		target_w = 1280;
		target_h = 480;
	} else if (g_pc98_gfx.mode_pref == PC98_MODE_VGA_640x480) {
		target_w = 640;
		target_h = 480;
	}

	if (g_pc98_gfx.var.xres != target_w || g_pc98_gfx.var.yres != target_h) {
		struct fb_var_screeninfo target_var = g_pc98_gfx.var;
		target_var.xres = target_w;
		target_var.yres = target_h;
		target_var.xres_virtual = target_w;
		target_var.yres_virtual = target_h;
		target_var.bits_per_pixel = 8;
		target_var.activate = FB_ACTIVATE_NOW;
		if (ioctl(g_pc98_gfx.fb_fd, FBIOPUT_VSCREENINFO, &target_var) == 0) {
			ioctl(g_pc98_gfx.fb_fd, FBIOGET_VSCREENINFO, &g_pc98_gfx.var);
			ioctl(g_pc98_gfx.fb_fd, FBIOGET_FSCREENINFO, &g_pc98_gfx.fix);
		}
	}

	g_pc98_gfx.width = g_pc98_gfx.var.xres;
	g_pc98_gfx.height = g_pc98_gfx.var.yres;
	g_pc98_gfx.pitch = g_pc98_gfx.fix.line_length;
	g_pc98_gfx.bpp = g_pc98_gfx.var.bits_per_pixel;
	g_pc98_gfx.buffer_size = g_pc98_gfx.pitch * g_pc98_gfx.height;

	if (g_pc98_gfx.var.pixclock > 0) {
		uint32_t htotal = g_pc98_gfx.var.left_margin + g_pc98_gfx.var.xres +
		                  g_pc98_gfx.var.right_margin + g_pc98_gfx.var.hsync_len;
		uint32_t vtotal = g_pc98_gfx.var.upper_margin + g_pc98_gfx.var.yres +
		                  g_pc98_gfx.var.lower_margin + g_pc98_gfx.var.vsync_len;
		if (htotal > 0 && vtotal > 0) {
			uint64_t total_pix = (uint64_t)htotal * vtotal;
			uint64_t clock_khz = 1000000000ULL / g_pc98_gfx.var.pixclock;
			g_pc98_gfx.monitor_hz = (float)((clock_khz * 1000ULL) / total_pix);
		} else {
			g_pc98_gfx.monitor_hz = 60.0f;
		}
	} else {
		g_pc98_gfx.monitor_hz = 60.0f;
	}

	if (fabsf(g_pc98_gfx.monitor_hz - g_pc98_gfx.target_hz) > 2.0f) {
		fprintf(stderr,
		        "libpc98: [NOTE] Monitor refresh rate (%.1f Hz) differs from game target (%.1f Hz).\n"
		        "         Frame pacing is actively synchronizing to avoid motion judder.\n",
		        g_pc98_gfx.monitor_hz, g_pc98_gfx.target_hz);
	}

	g_pc98_gfx.buffer = (uint8_t *)mmap(NULL, g_pc98_gfx.buffer_size,
					    PROT_READ | PROT_WRITE,
					    MAP_SHARED, g_pc98_gfx.fb_fd, 0);
	if (g_pc98_gfx.buffer == MAP_FAILED) {
		fprintf(stderr, "libpc98: failed to mmap framebuffer: %s\n", strerror(errno));
		close(g_pc98_gfx.fb_fd);
		g_pc98_gfx.fb_fd = -1;
		return -1;
	}

	for (int i = 0; i < 4; i++) {
		g_pc98_gfx.vram_page0[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
		g_pc98_gfx.vram_page1[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
	}

	g_pc98_gfx.backend = PC98_BACKEND_FB;
	return 0;
}

static inline void pc98_gfx_flip(void)
{
	uint8_t **disp_planes = (g_pc98_gfx.current_disp_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;

	if (g_pc98_gfx.backend == PC98_BACKEND_X11 && g_pc98_gfx.x_display && g_pc98_gfx.x_image) {
		if (disp_planes[0]) {
			pc98_planar_to_chunky(g_pc98_gfx.buffer, disp_planes,
			                      g_pc98_gfx.width, g_pc98_gfx.height);
		}

		uint32_t total = g_pc98_gfx.width * g_pc98_gfx.height;
		uint8_t *src = g_pc98_gfx.buffer;
		uint32_t *dst = g_pc98_gfx.x_pixels;
		uint32_t *pal = g_pc98_gfx.palette;

		for (uint32_t i = 0; i < total; i++) {
			dst[i] = pal[src[i]];
		}

		_XPutImage(g_pc98_gfx.x_display, g_pc98_gfx.x_window,
			   g_pc98_gfx.x_gc, g_pc98_gfx.x_image,
			   0, 0, 0, 0, g_pc98_gfx.width, g_pc98_gfx.height);

		while (_XPending(g_pc98_gfx.x_display)) {
			XEvent ev;
			_XNextEvent(g_pc98_gfx.x_display, &ev);
			if (ev.type == 2) {
				unsigned int kc = ev.xkey.keycode;
				if (kc == 113 || kc == 38) g_pc98_gfx.x_key_state |= PC98_BTN_LEFT;
				if (kc == 114 || kc == 40) g_pc98_gfx.x_key_state |= PC98_BTN_RIGHT;
				if (kc == 111 || kc == 25) g_pc98_gfx.x_key_state |= PC98_BTN_UP;
				if (kc == 116 || kc == 39) g_pc98_gfx.x_key_state |= PC98_BTN_DOWN;
				if (kc == 65 || kc == 44) g_pc98_gfx.x_key_state |= PC98_BTN_A;
				if (kc == 45) g_pc98_gfx.x_key_state |= PC98_BTN_B;
				if (kc == 50 || kc == 62) g_pc98_gfx.x_key_state |= PC98_BTN_SLOW;
			} else if (ev.type == 3) {
				unsigned int kc = ev.xkey.keycode;
				if (kc == 113 || kc == 38) g_pc98_gfx.x_key_state &= ~PC98_BTN_LEFT;
				if (kc == 114 || kc == 40) g_pc98_gfx.x_key_state &= ~PC98_BTN_RIGHT;
				if (kc == 111 || kc == 25) g_pc98_gfx.x_key_state &= ~PC98_BTN_UP;
				if (kc == 116 || kc == 39) g_pc98_gfx.x_key_state &= ~PC98_BTN_DOWN;
				if (kc == 65 || kc == 44) g_pc98_gfx.x_key_state &= ~PC98_BTN_A;
				if (kc == 45) g_pc98_gfx.x_key_state &= ~PC98_BTN_B;
				if (kc == 50 || kc == 62) g_pc98_gfx.x_key_state &= ~PC98_BTN_SLOW;
			}
		}
	} else if (g_pc98_gfx.backend == PC98_BACKEND_FB && disp_planes[0]) {
		pc98_planar_to_chunky(g_pc98_gfx.buffer, disp_planes,
		                      g_pc98_gfx.width, g_pc98_gfx.height);
	}
}

static inline void pc98_gfx_wait_vsync(void)
{
	if (g_pc98_gfx.backend == PC98_BACKEND_FB && g_pc98_gfx.fb_fd >= 0) {
		int crtc = 0;
		(void)ioctl(g_pc98_gfx.fb_fd, FBIO_WAITFORVSYNC, &crtc);
		pc98_gfx_flip();
	} else {
		/* Authentic PC-98 24.8 kHz V-Blank period: 1 / 56.423 Hz = 17,723,270 ns */
		struct timespec ts = { 0, 17723270L };
		nanosleep(&ts, NULL);
		pc98_gfx_flip();
	}
}

static inline void pc98_gfx_close(void)
{
	for (int i = 0; i < 4; i++) {
		if (g_pc98_gfx.vram_page0[i]) free(g_pc98_gfx.vram_page0[i]);
		if (g_pc98_gfx.vram_page1[i]) free(g_pc98_gfx.vram_page1[i]);
		g_pc98_gfx.vram_page0[i] = NULL;
		g_pc98_gfx.vram_page1[i] = NULL;
	}

	if (g_pc98_gfx.backend == PC98_BACKEND_X11) {
		if (g_pc98_gfx.x_pixels) free(g_pc98_gfx.x_pixels);
		if (g_pc98_gfx.buffer) free(g_pc98_gfx.buffer);
		if (_XFreeGC && g_pc98_gfx.x_gc) _XFreeGC(g_pc98_gfx.x_display, g_pc98_gfx.x_gc);
		if (_XDestroyWindow && g_pc98_gfx.x_window) _XDestroyWindow(g_pc98_gfx.x_display, g_pc98_gfx.x_window);
		if (_XCloseDisplay && g_pc98_gfx.x_display) _XCloseDisplay(g_pc98_gfx.x_display);
		if (g_pc98_gfx.x11_lib) dlclose(g_pc98_gfx.x11_lib);
	} else {
		if (g_pc98_gfx.buffer && g_pc98_gfx.buffer != MAP_FAILED) {
			munmap(g_pc98_gfx.buffer, g_pc98_gfx.buffer_size);
		}
		if (g_pc98_gfx.fb_fd >= 0) {
			if (g_pc98_gfx.orig_var.xres > 0) {
				ioctl(g_pc98_gfx.fb_fd, FBIOPUT_VSCREENINFO, &g_pc98_gfx.orig_var);
			}
			close(g_pc98_gfx.fb_fd);
			g_pc98_gfx.fb_fd = -1;
		}
	}
	g_pc98_gfx.buffer = NULL;
}

static inline void pc98_gfx_clear(uint8_t color)
{
	uint8_t **draw_planes = (g_pc98_gfx.current_draw_page == 1) ?
		g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;

	if (draw_planes[0]) {
		memset(draw_planes[0], (color & 1) ? 0xff : 0x00, PC98_PLANE_SIZE);
		memset(draw_planes[1], (color & 2) ? 0xff : 0x00, PC98_PLANE_SIZE);
		memset(draw_planes[2], (color & 4) ? 0xff : 0x00, PC98_PLANE_SIZE);
		memset(draw_planes[3], (color & 8) ? 0xff : 0x00, PC98_PLANE_SIZE);
	}

	if (g_pc98_gfx.buffer)
		memset(g_pc98_gfx.buffer, color, g_pc98_gfx.buffer_size);
}

static inline void pc98_gfx_putpixel(int x, int y, uint8_t color)
{
	if ((unsigned)x < g_pc98_gfx.width && (unsigned)y < g_pc98_gfx.height) {
		g_pc98_gfx.buffer[y * g_pc98_gfx.pitch + x] = color;
		uint8_t **planes = (g_pc98_gfx.current_draw_page == 1) ?
			g_pc98_gfx.vram_page1 : g_pc98_gfx.vram_page0;
		if (planes[0]) {
			uint32_t offset = y * PC98_PITCH + (x / 8);
			uint8_t mask = 1 << (7 - (x % 8));
			planes[0][offset] = (planes[0][offset] & ~mask) | ((color & 1) ? mask : 0);
			planes[1][offset] = (planes[1][offset] & ~mask) | ((color & 2) ? mask : 0);
			planes[2][offset] = (planes[2][offset] & ~mask) | ((color & 4) ? mask : 0);
			planes[3][offset] = (planes[3][offset] & ~mask) | ((color & 8) ? mask : 0);
		}
	}
}

static inline void pc98_gfx_fill_rect(int x, int y, int w, int h, uint8_t color)
{
	int py;
	int x2 = x + w;
	int y2 = y + h;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > (int)g_pc98_gfx.width) x2 = (int)g_pc98_gfx.width;
	if (y2 > (int)g_pc98_gfx.height) y2 = (int)g_pc98_gfx.height;
	if (x >= x2 || y >= y2) return;

	for (py = y; py < y2; py++) {
		uint8_t *dst = g_pc98_gfx.buffer + py * g_pc98_gfx.pitch + x;
		memset(dst, color, x2 - x);
	}
}

/* ------------------------------------------------------------------ */
/* master.lib Drop-in Compatibility Layer (ReC98 / PC-98 Games)       */
/* ------------------------------------------------------------------ */
static inline int graph_start(void)
{
	return pc98_gfx_open(NULL);
}

static inline void graph_end(void)
{
	pc98_gfx_close();
}

static inline void graph_showpage(int page)
{
	gdc_set_disp_page((uint8_t)page);
}

static inline void graph_accesspage(int page)
{
	gdc_set_draw_page((uint8_t)page);
}

static inline void graph_clear(void)
{
	pc98_gfx_clear(0);
}

static inline void vsync_wait(void)
{
	pc98_gfx_wait_vsync();
}

static inline void palette_init(void)
{
}

static inline void palette_show(void)
{
}

static inline void palette_set(int index, int r, int g, int b)
{
	pc98_gfx_set_palette((uint8_t)index, (uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17));
}

static inline void grcg_boxfill(int x1, int y1, int x2, int y2)
{
	int w = x2 - x1 + 1;
	int h = y2 - y1 + 1;
	uint8_t color = 0;
	if (g_pc98_gfx.grcg_tile[0]) color |= 1;
	if (g_pc98_gfx.grcg_tile[1]) color |= 2;
	if (g_pc98_gfx.grcg_tile[2]) color |= 4;
	if (g_pc98_gfx.grcg_tile[3]) color |= 8;
	pc98_gfx_fill_rect(x1, y1, w, h, color);
}

static inline int js_stat(int port)
{
	uint8_t state = 0xff;
	uint8_t pad = 0;
	if (opna_read_joystick(port, &state) == 0)
		pad = (uint8_t)(~state & 0x3f);
	if (g_pc98_gfx.backend == PC98_BACKEND_X11)
		pad |= g_pc98_gfx.x_key_state;
	return (int)pad;
}

/* ------------------------------------------------------------------ */
/* High-Resolution Monotonic Timing                                   */
/* ------------------------------------------------------------------ */
static inline uint64_t pc98_time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static inline void pc98_delay_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

/* ------------------------------------------------------------------ */
/* Audio & Gamepad Subsystem                                          */
/* ------------------------------------------------------------------ */
static inline int pc98_audio_init(void)
{
	return opna_init();
}

static inline void pc98_audio_close(void)
{
	opna_reset();
	opna_exit();
}

static inline uint8_t pc98_gamepad_read(int port)
{
	return (uint8_t)js_stat(port);
}

static inline void pc98_sfx_tone(unsigned int freq_hz, unsigned int ms)
{
	opna_ssg_tone(freq_hz, 12, 1);
	pc98_delay_ms(ms);
	opna_ssg_tone(freq_hz, 0, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* LIBPC98_H */
