// SPDX-License-Identifier: MIT
/*
 * touhou_demo.cpp - ReC98 PC-98 Native Touhou Engine & Danmaku Demo
 *
 * Demonstrates:
 *   - master.lib C compatibility API via libpc98.h
 *   - PC-98 GRCG (Graphic Read/Write Controller) hardware sprite blitting
 *   - Multi-ring Danmaku (bullet hell) emitter patterns with sub-pixel physics
 *   - Hardware V-Blank synchronization (vsync_wait)
 *   - Real-time DE-9 gamepad / keyboard input (js_stat)
 *   - OPNA sound effects on bullet fire and collisions
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#include "libpc98.h"

static volatile int running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

#define MAX_BULLETS 256

struct Bullet {
	float x, y;
	float vx, vy;
	uint8_t color;
	int active;
};

static Bullet bullets[MAX_BULLETS];

/* 16x16 Reimu Hakurei / Player Ship Sprite (2 bytes/row * 16 rows) */
static const uint8_t reimu_sprite[16 * 2] = {
	0x01, 0x80, /*       ##       */
	0x03, 0xc0, /*      ####      */
	0x07, 0xe0, /*     ######     */
	0x0f, 0xf0, /*    ########    */
	0x1f, 0xf8, /*   ##########   */
	0x3f, 0xfc, /*  ############  */
	0x7f, 0xfe, /* ############## */
	0xff, 0xff, /* ################ */
	0x7b, 0xde, /* ###.####.###   */
	0x3b, 0xdc, /*  ##.####.##    */
	0x1f, 0xf8, /*   ##########   */
	0x0f, 0xf0, /*    ########    */
	0x1b, 0xd8, /*   ##.####.##   */
	0x31, 0x8c, /*  ##...##...##  */
	0x60, 0x06, /* ##..........## */
	0x40, 0x02, /* #............# */
};

/* 8x8 Danmaku Bullet Mask (1 byte/row * 8 rows) */
static const uint8_t bullet_mask[8] = {
	0x3c, /*   ####   */
	0x7e, /*  ######  */
	0xff, /* ######## */
	0xff, /* ######## */
	0xff, /* ######## */
	0xff, /* ######## */
	0x7e, /*  ######  */
	0x3c, /*   ####   */
};

static void spawn_bullet_ring(float cx, float cy, int count, float speed, uint8_t color)
{
	float angle_step = (2.0f * 3.14159265f) / count;
	for (int i = 0; i < count; i++) {
		for (int b = 0; b < MAX_BULLETS; b++) {
			if (!bullets[b].active) {
				float angle = i * angle_step;
				bullets[b].x = cx;
				bullets[b].y = cy;
				bullets[b].vx = cosf(angle) * speed;
				bullets[b].vy = sinf(angle) * speed;
				bullets[b].color = color;
				bullets[b].active = 1;
				break;
			}
		}
	}
	opna_ssg_tone(1480, 8, 1); /* Danmaku fire tone */
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	printf("ReC98 PC-9821 Touhou Engine & Danmaku Demo Starting...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color PC-98 master.lib palette */
	palette_set(0, 0, 0, 0);       /* Black */
	palette_set(1, 0, 0, 10);      /* Dark Blue */
	palette_set(2, 10, 0, 0);      /* Dark Red */
	palette_set(3, 10, 0, 10);     /* Dark Magenta */
	palette_set(4, 0, 10, 0);      /* Dark Green */
	palette_set(5, 0, 10, 10);     /* Dark Cyan */
	palette_set(6, 10, 10, 0);     /* Dark Yellow */
	palette_set(7, 10, 10, 10);    /* Gray */
	palette_set(8, 5, 5, 5);       /* Darker Gray */
	palette_set(9, 0, 0, 15);      /* Bright Blue */
	palette_set(10, 15, 0, 0);     /* Bright Red */
	palette_set(11, 15, 0, 15);    /* Bright Magenta */
	palette_set(12, 0, 15, 0);     /* Bright Green */
	palette_set(13, 0, 15, 15);    /* Bright Cyan */
	palette_set(14, 15, 15, 0);    /* Bright Yellow */
	palette_set(15, 15, 15, 15);   /* White */

	float px = 320.0f;
	float py = 340.0f;
	float boss_x = 320.0f;
	float boss_y = 90.0f;
	float boss_dir = 2.0f;

	uint32_t tick = 0;
	uint64_t last_time = pc98_time_us();
	uint32_t frames = 0;

	printf("Engine Running: Use Gamepad Port 1 / Arrow Keys to dodge bullets. Ctrl-C to exit.\n");

	while (running) {
		/* 1. Hardware V-Sync (vsync_wait) */
		vsync_wait();

		/* 2. Read Gamepad Input via master.lib js_stat() */
		int pad = js_stat(1);
		float speed = 4.0f;
		if (pad & (JS_SLOW | JS_TRIG2)) speed = 1.8f; /* Focus / Slow mode (Left Shift / Trigger 2) */

		if (pad & JS_LEFT)  px -= speed;
		if (pad & JS_RIGHT) px += speed;
		if (pad & JS_UP)    py -= speed;
		if (pad & JS_DOWN)  py += speed;

		/* Clamp to playfield */
		if (px < 32.0f) px = 32.0f;
		if (px > 608.0f) px = 608.0f;
		if (py < 40.0f) py = 40.0f;
		if (py > 370.0f) py = 370.0f;

		/* 3. Boss Motion & Danmaku Patterns */
		boss_x += boss_dir;
		if (boss_x <= 100.0f || boss_x >= 540.0f)
			boss_dir = -boss_dir;

		if (tick % 45 == 0) {
			/* Spawn expanding ring */
			spawn_bullet_ring(boss_x, boss_y, 16, 2.8f, (tick % 90 == 0) ? 10 : 13);
		}
		if (tick % 60 == 30) {
			/* Spawn aimed stream */
			float dx = px - boss_x;
			float dy = py - boss_y;
			float dist = sqrtf(dx * dx + dy * dy);
			if (dist > 0.1f) {
				for (int b = 0; b < MAX_BULLETS; b++) {
					if (!bullets[b].active) {
						bullets[b].x = boss_x;
						bullets[b].y = boss_y;
						bullets[b].vx = (dx / dist) * 4.5f;
						bullets[b].vy = (dy / dist) * 4.5f;
						bullets[b].color = 14; /* Yellow */
						bullets[b].active = 1;
						break;
					}
				}
			}
		}

		/* 4. Update Bullets */
		for (int b = 0; b < MAX_BULLETS; b++) {
			if (bullets[b].active) {
				bullets[b].x += bullets[b].vx;
				bullets[b].y += bullets[b].vy;

				/* Check Bounds */
				if (bullets[b].x < 0 || bullets[b].x > 640 ||
				    bullets[b].y < 0 || bullets[b].y > 400) {
					bullets[b].active = 0;
					continue;
				}

				/* Hitbox Check against Player (4px radius) */
				float bdx = bullets[b].x - px;
				float bdy = bullets[b].y - py;
				if (bdx * bdx + bdy * bdy < 16.0f) {
					/* Player Hit */
					opna_ssg_tone(220, 20, 1); /* Hit sound */
					px = 320.0f;
					py = 340.0f;
					/* Clear bullets */
					for (int k = 0; k < MAX_BULLETS; k++)
						bullets[k].active = 0;
					break;
				}
			}
		}

		/* 5. Render Scene */
		graph_clear();

		/* Draw Playfield Border */
		grcg_setcolor(GRCG_TCR, 7);
		grcg_boxfill(20, 15, 620, 20);
		grcg_boxfill(20, 385, 620, 390);
		grcg_boxfill(20, 15, 25, 390);
		grcg_boxfill(615, 15, 620, 390);
		grcg_off();

		/* Draw Boss (16x16 red diamond) */
		grcg_setcolor(GRCG_TCR, 11); /* Magenta Boss */
		grcg_boxfill((int)boss_x - 12, (int)boss_y - 12, (int)boss_x + 12, (int)boss_y + 12);
		grcg_off();

		/* Draw Player Reimu Sprite using GRCG RMW blit */
		grcg_setcolor(GRCG_RMW, 10); /* Red / White */
		grcg_blit_sprite((int)px - 8, (int)py - 8, reimu_sprite, 2, 16);
		grcg_off();

		/* Draw Player Hitbox Center */
		pc98_gfx_putpixel((int)px, (int)py, 15); /* White dot */

		/* Draw Danmaku Bullets using GRCG RMW mode */
		for (int b = 0; b < MAX_BULLETS; b++) {
			if (bullets[b].active) {
				grcg_setcolor(GRCG_RMW, bullets[b].color);
				grcg_blit_sprite((int)bullets[b].x - 4, (int)bullets[b].y - 4, bullet_mask, 1, 8);
			}
		}
		grcg_off();

		tick++;
		frames++;
		if (frames % 120 == 0) {
			uint64_t now = pc98_time_us();
			float fps = (120.0f * 1000000.0f) / (float)(now - last_time);
			printf("ReC98 Danmaku FPS: %.1f | Bullets: %d | Tick: %u\n", fps, MAX_BULLETS, tick);
			fflush(stdout);
			last_time = now;
		}
	}

	pc98_audio_close();
	graph_end();
	printf("ReC98 Engine Demo Exited Cleanly.\n");
	return 0;
}
