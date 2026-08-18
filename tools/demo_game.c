// SPDX-License-Identifier: MIT
/*
 * demo_game.c - Native PC-9821 Arcade Game Demo using libpc98 & GRCG
 *
 * Demonstrates:
 *   - PC-98 GRCG (Graphic Read/Write Controller) hardware sprite blitting (RMW mode)
 *   - Dual-Backend: Runs natively on /dev/fb0 console OR inside an X11 window
 *   - 60 FPS / 56.4 Hz tear-free hardware V-Sync (pc98_gfx_wait_vsync)
 *   - Double buffering page flipping (gdc_flip_page)
 *   - Real-time DE-9 gamepad / joystick control (port 1 & 2)
 *   - Real-time hardware OPNA SSG sound effects on collisions
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "libpc98.h"

static volatile int running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

/* 16x16 PC-98 Sprite Mask (2 bytes per row * 16 rows) */
static const uint8_t player_ship_mask[16 * 2] = {
	0x01, 0x80, /*       ##       */
	0x03, 0xc0, /*      ####      */
	0x07, 0xe0, /*     ######     */
	0x07, 0xe0, /*     ######     */
	0x0f, 0xf0, /*    ########    */
	0x1f, 0xf8, /*   ##########   */
	0x3f, 0xfc, /*  ############  */
	0x7f, 0xfe, /* ############## */
	0xff, 0xff, /* ################ */
	0x7f, 0xfe, /* ############## */
	0x3f, 0xfc, /*  ############  */
	0x1f, 0xf8, /*   ##########   */
	0x3b, 0xdc, /*  ###.####.###  */
	0x71, 0x8e, /* ###...##...### */
	0x60, 0x06, /* ##..........## */
	0x40, 0x02, /* #............# */
};

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	printf("Starting PC-9821 GRCG / GDC Game Engine Demo...\n");

	if (pc98_gfx_open("/dev/fb0") != 0) {
		fprintf(stderr, "Cannot initialize graphics backend\n");
		return 1;
	}

	pc98_audio_init();

	printf("Backend: %s | Screen: %dx%d (%d bpp)\n",
	       (g_pc98_gfx.backend == PC98_BACKEND_X11) ? "X11 Window" : "Direct Framebuffer",
	       g_pc98_gfx.width, g_pc98_gfx.height, g_pc98_gfx.bpp);

	/* Set up 16-color PC-98 palette */
	pc98_gfx_set_palette(0, 0, 0, 0);       /* Black */
	pc98_gfx_set_palette(1, 0, 0, 180);     /* Blue */
	pc98_gfx_set_palette(2, 180, 0, 0);     /* Red */
	pc98_gfx_set_palette(3, 180, 0, 180);   /* Magenta */
	pc98_gfx_set_palette(4, 0, 180, 0);     /* Green */
	pc98_gfx_set_palette(5, 0, 180, 180);   /* Cyan */
	pc98_gfx_set_palette(6, 180, 180, 0);   /* Yellow */
	pc98_gfx_set_palette(7, 180, 180, 180); /* Light Gray */
	pc98_gfx_set_palette(8, 80, 80, 80);    /* Dark Gray */
	pc98_gfx_set_palette(9, 0, 0, 255);     /* Bright Blue */
	pc98_gfx_set_palette(10, 255, 0, 0);    /* Bright Red */
	pc98_gfx_set_palette(11, 255, 0, 255);  /* Bright Magenta */
	pc98_gfx_set_palette(12, 0, 255, 0);    /* Bright Green */
	pc98_gfx_set_palette(13, 0, 255, 255);  /* Bright Cyan */
	pc98_gfx_set_palette(14, 255, 255, 0);  /* Bright Yellow */
	pc98_gfx_set_palette(15, 255, 255, 255);/* White */

	int pad_x = g_pc98_gfx.width / 2 - 40;
	int pad_y = g_pc98_gfx.height - 35;
	int pad_w = 80;
	int pad_h = 10;

	float ball_x = g_pc98_gfx.width / 2.0f;
	float ball_y = g_pc98_gfx.height / 3.0f;
	float ball_vx = 4.0f;
	float ball_vy = 3.0f;
	int ball_size = 12;

	int score = 0;
	uint64_t last_time = pc98_time_us();
	uint32_t frames = 0;

	printf("Game Loop Running: Gamepad Port 1 / Arrow Keys to move. Press Ctrl-C to quit.\n");

	while (running) {
		/* 1. Hardware/Software V-Blank Sync */
		pc98_gfx_wait_vsync();

		/* 2. Read Gamepad / Keyboard Input */
		uint8_t pad = pc98_gamepad_read(1);
		if (pad & PC98_BTN_LEFT) {
			pad_x -= 8;
			if (pad_x < 16) pad_x = 16;
		}
		if (pad & PC98_BTN_RIGHT) {
			pad_x += 8;
			if (pad_x + pad_w > (int)g_pc98_gfx.width - 16)
				pad_x = g_pc98_gfx.width - 16 - pad_w;
		}

		/* 3. Physics & Ball Motion */
		ball_x += ball_vx;
		ball_y += ball_vy;

		/* Wall Collisions */
		if (ball_x <= 16.0f) {
			ball_x = 16.0f;
			ball_vx = -ball_vx;
			opna_ssg_tone(880, 10, 1);
		} else if (ball_x + ball_size >= (int)g_pc98_gfx.width - 16) {
			ball_x = (float)(g_pc98_gfx.width - 16 - ball_size);
			ball_vx = -ball_vx;
			opna_ssg_tone(880, 10, 1);
		}

		if (ball_y <= 16.0f) {
			ball_y = 16.0f;
			ball_vy = -ball_vy;
			opna_ssg_tone(1174, 10, 1);
		}

		/* Paddle Collision */
		if (ball_y + ball_size >= (float)pad_y &&
		    ball_y <= (float)(pad_y + pad_h) &&
		    ball_x + ball_size >= (float)pad_x &&
		    ball_x <= (float)(pad_x + pad_w)) {
			ball_y = (float)(pad_y - ball_size);
			ball_vy = -ball_vy;
			score++;
			opna_ssg_tone(1760, 12, 1);
		}

		/* Bottom Out / Reset */
		if (ball_y > (float)g_pc98_gfx.height) {
			ball_x = g_pc98_gfx.width / 2.0f;
			ball_y = g_pc98_gfx.height / 3.0f;
			ball_vy = 3.0f;
			score = 0;
			opna_ssg_tone(220, 14, 1);
		}

		/* 4. Render Frame using GRCG Sprite Blitting & Planar Operations */
		pc98_gfx_clear(0); /* Black background */

		/* Draw Border Arena */
		pc98_gfx_fill_rect(0, 0, g_pc98_gfx.width, 10, 7);
		pc98_gfx_fill_rect(0, 0, 10, g_pc98_gfx.height, 7);
		pc98_gfx_fill_rect(g_pc98_gfx.width - 10, 0, 10, g_pc98_gfx.height, 7);

		/* Draw Paddle */
		pc98_gfx_fill_rect(pad_x, pad_y, pad_w, pad_h, 14);

		/* Draw Player Sprite using PC-98 GRCG RMW (Read-Modify-Write) mode */
		grcg_set_mode(GRCG_RMW);
		grcg_set_color(13); /* Bright Cyan */
		grcg_blit_sprite(pad_x + (pad_w / 2) - 8, pad_y - 18, player_ship_mask, 2, 16);
		grcg_set_mode(GRCG_OFF);

		/* Draw Ball */
		pc98_gfx_fill_rect((int)ball_x, (int)ball_y, ball_size, ball_size, 10);

		frames++;
		if (frames % 120 == 0) {
			uint64_t now = pc98_time_us();
			float fps = (120.0f * 1000000.0f) / (float)(now - last_time);
			printf("FPS: %.1f | Score: %d | Paddle: %d\n", fps, score, pad_x);
			fflush(stdout);
			last_time = now;
		}
	}

	pc98_audio_close();
	pc98_gfx_close();
	printf("Game Exited Cleanly.\n");
	return 0;
}
