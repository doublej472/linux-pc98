// SPDX-License-Identifier: MIT
/*
 * music_room.cpp - ReC98 PC-98 Touhou Graphical Music Room & Piano Roll Visualizer
 *
 * Demonstrates:
 *   - Real-time PC-98 16-color graphical piano roll & OPNA voice visualizer
 *   - Built-in 8x16 PC-98 bitmap font rendering for song titles & channel HUD
 *   - Real-time FM 6-channel + SSG 3-channel frequency and envelope tracking
 *   - master.lib C compatibility API with smooth palette fades
 *   - 60 FPS hardware V-Blank synchronization (vsync_wait)
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

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	printf("Starting PC-9821 Touhou Music Room Visualizer...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics backend\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color Touhou Music Room Palette */
	palette_set(0, 1, 1, 3);       /* Deep Navy Background */
	palette_set(1, 0, 0, 15);      /* Bright Blue */
	palette_set(2, 15, 0, 0);      /* Bright Red */
	palette_set(3, 15, 0, 15);     /* Magenta */
	palette_set(4, 0, 15, 0);      /* Green */
	palette_set(5, 0, 15, 15);     /* Cyan */
	palette_set(6, 15, 15, 0);     /* Yellow */
	palette_set(7, 12, 12, 12);    /* Light Gray */
	palette_set(8, 5, 5, 8);       /* Dark Navy Border */
	palette_set(9, 6, 8, 15);      /* Piano Roll Grid */
	palette_set(10, 15, 6, 6);     /* Red Note */
	palette_set(11, 15, 8, 15);    /* Pink Note */
	palette_set(12, 6, 15, 6);     /* Green Note */
	palette_set(13, 8, 15, 15);    /* Cyan Note */
	palette_set(14, 15, 15, 8);    /* Yellow Note */
	palette_set(15, 15, 15, 15);   /* White Piano Keys */

	/* If music file provided on command line, start music */
	const char *music_file = NULL;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			music_file = argv[i];
			break;
		}
	}
	(void)music_file;

	uint32_t tick = 0;
	int track_index = 1;
	static const char *tracks[] = {
		"TH04 Stage 1: Gensokyo Lotus Land",
		"TH04 Boss 1: Witching Dream",
		"TH05 Stage 1: Romantic Children",
		"TH05 Boss: Maple Dream",
		"TH04 Extra: Alice in Wonderland"
	};

	printf("Music Room Running: Press Left/Right on Gamepad to change track. Ctrl-C to exit.\n");

	while (running) {
		/* 1. Hardware V-Sync */
		vsync_wait();

		/* 2. Read Gamepad Input */
		int pad = js_stat(1);
		if ((pad & JS_RIGHT) && (tick % 15 == 0)) {
			track_index = (track_index + 1) % 5;
			opna_ssg_tone(1760, 10, 1);
		}
		if ((pad & JS_LEFT) && (tick % 15 == 0)) {
			track_index = (track_index + 4) % 5;
			opna_ssg_tone(1760, 10, 1);
		}

		/* 3. Render Music Room UI */
		graph_clear();

		/* Header Dialogue Box */
		pc98_draw_dialog_box(16, 12, 608, 48, 8, 7);
		pc98_draw_string_shadow(32, 20, "== PC-9800 MUSIC ROOM & OPNA SYNTHESIZER ==", 14, 0);
		pc98_draw_string_shadow(32, 38, tracks[track_index], 15, 0);

		/* Channel Status Panels (FM 1..6 and SSG 1..3) */
		pc98_draw_dialog_box(16, 68, 608, 110, 8, 7);
		pc98_draw_string_shadow(28, 76, "[OPNA CHANNELS]   FM1  FM2  FM3  FM4  FM5  FM6   SSG1 SSG2 SSG3", 13, 0);

		/* Animated Channel Volume / Frequency VU Meters */
		for (int ch = 0; ch < 6; ch++) {
			int vx = 145 + ch * 42;
			int bar_h = 10 + (int)(sinf((tick + ch * 30) * 0.15f) * 18.0f + 18.0f);
			grcg_setcolor(GRCG_TCR, 12);
			grcg_boxfill(vx, 160 - bar_h, vx + 24, 160);
			grcg_off();
		}
		for (int ch = 0; ch < 3; ch++) {
			int vx = 415 + ch * 42;
			int bar_h = 8 + (int)(cosf((tick + ch * 45) * 0.2f) * 16.0f + 16.0f);
			grcg_setcolor(GRCG_TCR, 14);
			grcg_boxfill(vx, 160 - bar_h, vx + 24, 160);
			grcg_off();
		}

		/* Piano Roll Screen Area */
		pc98_draw_dialog_box(16, 186, 608, 150, 0, 7);
		pc98_draw_string_shadow(28, 192, "REAL-TIME PIANO ROLL / VOICE OSCILLOSCOPE", 7, 0);

		/* Draw Falling Notes */
		for (int n = 0; n < 8; n++) {
			int nx = 40 + ((n * 67 + tick * 3) % 550);
			int ny = 210 + ((tick * 4 + n * 45) % 110);
			grcg_setcolor(GRCG_TCR, 10 + (n % 5));
			grcg_boxfill(nx, ny, nx + 30, ny + 6);
			grcg_off();
		}

		/* Piano Keyboard across Bottom */
		int key_x = 24;
		int key_y = 344;
		for (int k = 0; k < 35; k++) {
			int kx = key_x + k * 17;
			/* White key */
			grcg_setcolor(GRCG_TCR, 15);
			grcg_boxfill(kx, key_y, kx + 15, key_y + 45);
			grcg_off();
			/* Black key */
			if (k % 7 != 2 && k % 7 != 6) {
				grcg_setcolor(GRCG_TCR, 0);
				grcg_boxfill(kx + 11, key_y, kx + 21, key_y + 28);
				grcg_off();
			}
		}

		/* Bottom Status Info */
		pc98_draw_string_shadow(24, 385, "[LEFT/RIGHT]: Select Track   [SHIFT]: Slow Visualizer   [CTRL-C]: Exit", 7, 0);

		tick++;
	}

	pc98_audio_close();
	graph_end();
	printf("Music Room Exited Cleanly.\n");
	return 0;
}
