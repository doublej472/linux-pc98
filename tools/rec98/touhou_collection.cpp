// SPDX-License-Identifier: MIT
/*
 * touhou_collection.cpp - ReC98 PC-98 Touhou 1-5 Collection & Arcade Suite
 *
 * Integrated launcher for:
 *   [1] Touhou 1: Highly Responsive to Prayers (th01)
 *   [2] Touhou 2: Story of Eastern Wonderland (th02)
 *   [3] Touhou 3: Phantasmagoria of Dim.Dream (th03)
 *   [4] Touhou 4: Lotus Land Story (th04)
 *   [5] Touhou 5: Mystic Square (th05)
 *   [6] PC-98 Touhou Music Room Visualizer (pc98-music-room)
 *   [7] PC-98 Boss Rush & Spell Card Battle (pc98-boss-battle)
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

	printf("Starting PC-9821 Touhou Project 1-5 Arcade Suite...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color Palette */
	palette_set(0, 0, 0, 2);       /* Deep Navy */
	palette_set(1, 0, 0, 15);      /* Blue */
	palette_set(2, 15, 0, 0);      /* Red */
	palette_set(3, 15, 0, 15);     /* Magenta */
	palette_set(4, 0, 15, 0);      /* Green */
	palette_set(5, 0, 15, 15);     /* Cyan */
	palette_set(6, 15, 15, 0);     /* Yellow */
	palette_set(7, 12, 12, 12);    /* Gray */
	palette_set(8, 4, 4, 8);       /* Dark Panel */
	palette_set(9, 6, 6, 12);      /* Mid Panel */
	palette_set(10, 15, 4, 4);     /* Bright Red */
	palette_set(11, 15, 6, 15);    /* Spell Magenta */
	palette_set(12, 4, 15, 4);     /* Active Green */
	palette_set(13, 6, 15, 15);    /* Selection Cyan */
	palette_set(14, 15, 15, 4);    /* Gold */
	palette_set(15, 15, 15, 15);   /* White */

	int selected = 0;
	static const char *titles[] = {
		"[1] Touhou 1: Highly Responsive to Prayers (Rei'iden)",
		"[2] Touhou 2: Story of Eastern Wonderland  (Fuumaroku)",
		"[3] Touhou 3: Phantasmagoria of Dim.Dream  (Yumejikuu)",
		"[4] Touhou 4: Lotus Land Story             (Gensokyo)",
		"[5] Touhou 5: Mystic Square                (Kaikidan)",
		"[6] PC-98 Touhou Music Room Visualizer",
		"[7] PC-98 Boss Rush & Spell Card Battle",
		"[8] System Benchmark & Performance Profiler",
		"[9] Exit to Shell"
	};
	static const char *binaries[] = {
		"/usr/sbin/th01",
		"/usr/sbin/th02",
		"/usr/sbin/th03",
		"/usr/sbin/th04",
		"/usr/sbin/th05",
		"/usr/sbin/pc98-music-room",
		"/usr/sbin/pc98-boss-battle",
		"/usr/sbin/pc98-benchmark",
		NULL
	};

	uint32_t tick = 0;
	const char *launch_target = NULL;

	while (running) {
		vsync_wait();

		int pad = js_stat(1);
		if ((pad & JS_UP) && (tick % 10 == 0)) {
			selected = (selected + 8) % 9;
			opna_ssg_tone(1500, 4, 0);
		}
		if ((pad & JS_DOWN) && (tick % 10 == 0)) {
			selected = (selected + 1) % 9;
			opna_ssg_tone(1500, 4, 0);
		}
		if (pad & JS_TRIG1) {
			opna_ssg_tone(2200, 10, 1);
			launch_target = binaries[selected];
			break;
		}

		/* Render Menu */
		graph_clear();

		/* Title Banner */
		pc98_draw_dialog_box(20, 15, 600, 50, 8, 7);
		pc98_draw_string_shadow(36, 22, "================================================", 14, 0);
		pc98_draw_string_shadow(36, 36, "   TOUHOU PROJECT 1-5 PC-9821 LINUX COLLECTION  ", 15, 0);

		/* Game List Box */
		pc98_draw_dialog_box(20, 75, 600, 270, 0, 7);

		for (int i = 0; i < 9; i++) {
			int item_y = 90 + i * 28;
			if (i == selected) {
				grcg_setcolor(GRCG_TCR, 1);
				grcg_boxfill(28, item_y - 3, 612, item_y + 19);
				grcg_off();
				pc98_draw_string_shadow(36, item_y, titles[i], 13, 0);
			} else {
				pc98_draw_string_shadow(36, item_y, titles[i], 7, 0);
			}
		}

		/* Footer */
		pc98_draw_dialog_box(20, 355, 600, 30, 8, 7);
		pc98_draw_string_shadow(36, 362, "[UP/DOWN]: Select   [BUTTON 1 / ENTER]: Launch   [CTRL-C]: Exit", 14, 0);

		tick++;
	}

	pc98_audio_close();
	graph_end();

	if (launch_target) {
		printf("Launching %s...\n", launch_target);
		execl(launch_target, launch_target, (char *)NULL);
	}

	return 0;
}
