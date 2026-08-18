// SPDX-License-Identifier: MIT
/*
 * boss_battle.cpp - ReC98 PC-98 Touhou Boss Battle & Spell Card Engine Demo
 *
 * Demonstrates:
 *   - Multi-layered 16-color starfield parallax scrolling
 *   - Spell card announcement banners & dialogue text boxes
 *   - Geometric laser beams (grcg_line) and expanding magic circles (grcg_circle)
 *   - Boss HP gauge and multi-phase bullet hell patterns
 *   - master.lib compatibility API with 60 FPS hardware V-Sync
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

#define MAX_BULLETS 320
#define MAX_STARS   64

struct Bullet {
	float x, y;
	float vx, vy;
	uint8_t color;
	int active;
};

struct Star {
	float x, y;
	float speed;
	uint8_t color;
};

static Bullet bullets[MAX_BULLETS];
static Star stars[MAX_STARS];

/* 16x16 Reimu Sprite */
static const uint8_t reimu_sprite[16 * 2] = {
	0x01, 0x80, 0x03, 0xc0, 0x07, 0xe0, 0x0f, 0xf0,
	0x1f, 0xf8, 0x3f, 0xfc, 0x7f, 0xfe, 0xff, 0xff,
	0x7b, 0xde, 0x3b, 0xdc, 0x1f, 0xf8, 0x0f, 0xf0,
	0x1b, 0xd8, 0x31, 0x8c, 0x60, 0x06, 0x40, 0x02,
};

static void spawn_spiral(float cx, float cy, float base_angle, int arms, float speed, uint8_t color)
{
	for (int i = 0; i < arms; i++) {
		for (int b = 0; b < MAX_BULLETS; b++) {
			if (!bullets[b].active) {
				float angle = base_angle + (i * 2.0f * 3.14159265f / arms);
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
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	printf("Starting PC-9821 Boss Battle & Spell Card Demo...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color Spell Card Palette */
	palette_set(0, 0, 0, 1);       /* Black/Deep Blue */
	palette_set(1, 0, 0, 15);      /* Blue */
	palette_set(2, 15, 0, 0);      /* Red */
	palette_set(3, 15, 0, 15);     /* Magenta */
	palette_set(4, 0, 15, 0);      /* Green */
	palette_set(5, 0, 15, 15);     /* Cyan */
	palette_set(6, 15, 15, 0);     /* Yellow */
	palette_set(7, 10, 10, 10);    /* Gray */
	palette_set(8, 4, 4, 6);       /* Dark Star */
	palette_set(9, 7, 7, 12);      /* Mid Star */
	palette_set(10, 15, 4, 4);     /* Bright Red */
	palette_set(11, 15, 6, 15);    /* Spell Card Magenta */
	palette_set(12, 4, 15, 4);     /* HP Green */
	palette_set(13, 6, 15, 15);    /* Laser Cyan */
	palette_set(14, 15, 15, 6);    /* Boss Star Yellow */
	palette_set(15, 15, 15, 15);   /* White */

	/* Init Parallax Starfield */
	for (int s = 0; s < MAX_STARS; s++) {
		stars[s].x = (float)(rand() % 640);
		stars[s].y = (float)(rand() % 400);
		stars[s].speed = 0.5f + (rand() % 30) / 10.0f;
		stars[s].color = (stars[s].speed > 2.0f) ? 15 : ((stars[s].speed > 1.0f) ? 9 : 8);
	}

	float px = 320.0f, py = 340.0f;
	float boss_x = 320.0f, boss_y = 90.0f;
	float boss_vx = 2.5f;
	int boss_hp = 1000;
	int max_hp = 1000;
	float spell_circle_r = 10.0f;

	uint32_t tick = 0;
	printf("Battle Running: Use Arrow Keys/Gamepad to dodge. Left Shift to Focus. Ctrl-C to exit.\n");

	while (running) {
		vsync_wait();

		/* 1. Input */
		int pad = js_stat(1);
		float speed = (pad & (JS_SLOW | JS_TRIG2)) ? 1.8f : 4.0f;

		if (pad & JS_LEFT)  px -= speed;
		if (pad & JS_RIGHT) px += speed;
		if (pad & JS_UP)    py -= speed;
		if (pad & JS_DOWN)  py += speed;

		if (px < 32.0f) px = 32.0f;
		if (px > 608.0f) px = 608.0f;
		if (py < 40.0f) py = 40.0f;
		if (py > 370.0f) py = 370.0f;

		/* 2. Parallax Starfield Scrolling */
		for (int s = 0; s < MAX_STARS; s++) {
			stars[s].y += stars[s].speed;
			if (stars[s].y >= 400.0f) {
				stars[s].y = 0.0f;
				stars[s].x = (float)(rand() % 640);
			}
		}

		/* 3. Boss Motion & Spell Card Attacks */
		boss_x += boss_vx;
		if (boss_x <= 120.0f || boss_x >= 520.0f)
			boss_vx = -boss_vx;

		/* Expanding Spell Card Magic Circle */
		spell_circle_r += 0.8f;
		if (spell_circle_r > 70.0f) spell_circle_r = 15.0f;

		/* Danmaku Spell Patterns */
		if (tick % 6 == 0) {
			float base_angle = (tick * 0.08f);
			spawn_spiral(boss_x, boss_y, base_angle, 4, 3.2f, (tick % 12 == 0) ? 11 : 13);
		}

		/* Update Bullets */
		for (int b = 0; b < MAX_BULLETS; b++) {
			if (bullets[b].active) {
				bullets[b].x += bullets[b].vx;
				bullets[b].y += bullets[b].vy;
				if (bullets[b].x < 0 || bullets[b].x > 640 ||
				    bullets[b].y < 0 || bullets[b].y > 400) {
					bullets[b].active = 0;
				}
			}
		}

		/* 4. Render Scene */
		graph_clear();

		/* Draw Parallax Stars */
		for (int s = 0; s < MAX_STARS; s++) {
			pc98_gfx_putpixel((int)stars[s].x, (int)stars[s].y, stars[s].color);
		}

		/* Boss Magic Circle (grcg_circle) */
		grcg_setcolor(GRCG_TCR, 11);
		grcg_circle((int)boss_x, (int)boss_y, (int)spell_circle_r);
		grcg_circle((int)boss_x, (int)boss_y, (int)spell_circle_r / 2);
		grcg_off();

		/* Geometric Laser Beams from Boss (grcg_line) */
		if ((tick / 60) % 2 == 1) {
			grcg_setcolor(GRCG_TCR, 13);
			float lx1 = boss_x - 150.0f * sinf(tick * 0.05f);
			float lx2 = boss_x + 150.0f * sinf(tick * 0.05f);
			grcg_line((int)boss_x, (int)boss_y, (int)lx1, 390);
			grcg_line((int)boss_x, (int)boss_y, (int)lx2, 390);
			grcg_off();
		}

		/* Boss Sprite (16x16 Yellow Diamond) */
		grcg_setcolor(GRCG_TCR, 14);
		grcg_boxfill((int)boss_x - 14, (int)boss_y - 14, (int)boss_x + 14, (int)boss_y + 14);
		grcg_off();

		/* Player Sprite */
		grcg_setcolor(GRCG_RMW, 10);
		grcg_blit_sprite((int)px - 8, (int)py - 8, reimu_sprite, 2, 16);
		grcg_off();
		pc98_gfx_putpixel((int)px, (int)py, 15);

		/* Bullets */
		for (int b = 0; b < MAX_BULLETS; b++) {
			if (bullets[b].active) {
				grcg_setcolor(GRCG_TCR, bullets[b].color);
				grcg_boxfill((int)bullets[b].x - 2, (int)bullets[b].y - 2, (int)bullets[b].x + 2, (int)bullets[b].y + 2);
			}
		}
		grcg_off();

		/* Boss HP Gauge */
		pc98_draw_dialog_box(16, 8, 608, 22, 0, 7);
		pc98_draw_string_shadow(24, 12, "BOSS: YUUKA KAZAMI", 14, 0);
		int hp_w = (int)((boss_hp / (float)max_hp) * 400.0f);
		grcg_setcolor(GRCG_TCR, 12);
		grcg_boxfill(190, 13, 190 + hp_w, 20);
		grcg_off();

		/* Spell Card Announcement Banner */
		if ((tick / 180) % 2 == 0) {
			pc98_draw_dialog_box(340, 360, 280, 28, 8, 11);
			pc98_draw_string_shadow(350, 366, "Spell Card: Stardust Reverie", 11, 0);
		}

		tick++;
	}

	pc98_audio_close();
	graph_end();
	printf("Boss Battle Demo Exited Cleanly.\n");
	return 0;
}
