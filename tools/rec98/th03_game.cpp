// SPDX-License-Identifier: MIT
/*
 * th03_game.cpp - Touhou 3: Phantasmagoria of Dim.Dream (Yumejikuu)
 * Native Linux PC-98 Port using libpc98 SDK
 *
 * Gameplay:
 *   - Head-to-Head Split-Screen Competitive Danmaku Battle (Player vs CPU)
 *   - Dual Playfields (1P Left Field vs 2P Right Field)
 *   - Chain Combos send Boss Ex-Attacks and Fireball Spirits across fields
 *   - 4-Tier Charge Gauge system (Charge 1..4)
 *   - 60 FPS hardware V-Sync with OPNA audio effects
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

#define FIELD_W  280
#define FIELD_H  360
#define P1_X     16
#define P1_Y     20
#define P2_X     344
#define P2_Y     20

#define MAX_SHOTS 64
#define MAX_MOBS  16

struct Bullet {
	float x, y, vx, vy;
	uint8_t color;
	int active;
};

struct Mob {
	float x, y, vx, vy;
	int hp;
	int timer;
	int active;
};

struct PlayerField {
	float px, py;
	int lives;
	int score;
	int charge;      /* 0..100 */
	int max_charge;
	int combo;
	int invuln;
	int shoot_timer;

	Bullet p_shots[MAX_SHOTS];
	Bullet e_shots[MAX_SHOTS];
	Mob mobs[MAX_MOBS];
};

struct TH03_State {
	PlayerField p1;
	PlayerField p2;
	int round;
	int p1_wins;
	int p2_wins;
	uint32_t tick;
};

static TH03_State game;
static pc98_dat_t *dat_yume = NULL;
static pc98_dat_t *dat_main = NULL;
static pc98_cdg_t p1_cdg;
static pc98_cdg_t p2_cdg;
static pc98_pi_image_t bg_pi;
static int has_p1_cdg = 0;
static int has_p2_cdg = 0;
static int has_bg_pi  = 0;

static void spawn_mob(PlayerField *f, int field_x, float x, float y)
{
	for (int i = 0; i < MAX_MOBS; i++) {
		if (!f->mobs[i].active) {
			f->mobs[i].x = field_x + x;
			f->mobs[i].y = P1_Y + y;
			f->mobs[i].vx = (rand() % 2 == 0) ? -1.0f : 1.0f;
			f->mobs[i].vy = 1.2f;
			f->mobs[i].hp = 5;
			f->mobs[i].timer = 0;
			f->mobs[i].active = 1;
			break;
		}
	}
}

static void send_attack_to_opponent(PlayerField *opp, int field_x, int power)
{
	for (int i = 0; i < power * 2; i++) {
		for (int b = 0; b < MAX_SHOTS; b++) {
			if (!opp->e_shots[b].active) {
				opp->e_shots[b].x = field_x + 30 + (rand() % (FIELD_W - 60));
				opp->e_shots[b].y = P1_Y + 10;
				opp->e_shots[b].vx = ((rand() % 20) - 10) / 8.0f;
				opp->e_shots[b].vy = 2.0f + (rand() % 20) / 10.0f;
				opp->e_shots[b].color = (power > 2) ? 11 : 9;
				opp->e_shots[b].active = 1;
				break;
			}
		}
	}
}

static void init_round(void)
{
	/* Load Authentic Background .PI from YUME.DAT */
	if (!has_bg_pi) {
		size_t pi_sz = 0;
		uint8_t *pi_data = pc98_dat_read_file(dat_yume, "ST01.PI", &pi_sz);
		if (!pi_data) pi_data = pc98_dat_read_file(dat_yume, "OP.PI", &pi_sz);
		if (pi_data) {
			if (pc98_pi_load_from_memory(pi_data, pi_sz, &bg_pi) == 0) {
				has_bg_pi = 1;
				pc98_apply_pi_palette(&bg_pi);
			}
			free(pi_data);
		}
	}

	/* Load Authentic Character .CDG from MAIN.DAT */
	if (!has_p1_cdg) {
		size_t cdg_sz = 0;
		uint8_t *cdg_data = pc98_dat_read_file(dat_main, "CHAR0.CDG", &cdg_sz);
		if (cdg_data) {
			if (pc98_cdg_load_from_memory(cdg_data, cdg_sz, &p1_cdg) == 0) has_p1_cdg = 1;
			free(cdg_data);
		}
	}
	if (!has_p2_cdg) {
		size_t cdg_sz = 0;
		uint8_t *cdg_data = pc98_dat_read_file(dat_main, "CHAR1.CDG", &cdg_sz);
		if (cdg_data) {
			if (pc98_cdg_load_from_memory(cdg_data, cdg_sz, &p2_cdg) == 0) has_p2_cdg = 1;
			free(cdg_data);
		}
	}

	game.p1.px = P1_X + FIELD_W / 2.0f;
	game.p1.py = P1_Y + FIELD_H - 40.0f;
	game.p1.lives = 3;
	game.p1.charge = 0;
	game.p1.max_charge = 100;
	game.p1.combo = 0;
	game.p1.invuln = 60;
	game.p1.shoot_timer = 0;

	game.p2.px = P2_X + FIELD_W / 2.0f;
	game.p2.py = P2_Y + FIELD_H - 40.0f;
	game.p2.lives = 3;
	game.p2.charge = 0;
	game.p2.max_charge = 100;
	game.p2.combo = 0;
	game.p2.invuln = 60;
	game.p2.shoot_timer = 0;

	for (int i = 0; i < MAX_SHOTS; i++) {
		game.p1.p_shots[i].active = 0;
		game.p1.e_shots[i].active = 0;
		game.p2.p_shots[i].active = 0;
		game.p2.e_shots[i].active = 0;
	}
	for (int i = 0; i < MAX_MOBS; i++) {
		game.p1.mobs[i].active = 0;
		game.p2.mobs[i].active = 0;
	}
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	static const char *th03_dats[] = { "YUME.DAT", "MAIN.DAT" };
	pc98_require_assets("Touhou 3: Phantasmagoria of Dim.Dream", "th03", th03_dats, 2);

	dat_yume = pc98_dat_open("th03", "YUME.DAT");
	dat_main = pc98_dat_open("th03", "MAIN.DAT");

	printf("Starting Touhou 3: Phantasmagoria of Dim.Dream (Native PC-98 Port)...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color Palette */
	palette_set(0, 0, 0, 2);       /* Dark Blue/Black */
	palette_set(1, 0, 0, 15);      /* 1P Blue */
	palette_set(2, 15, 0, 0);      /* 2P Red */
	palette_set(3, 15, 0, 15);     /* Magenta */
	palette_set(4, 0, 15, 0);      /* Green */
	palette_set(5, 0, 15, 15);     /* Cyan */
	palette_set(6, 15, 15, 0);     /* Yellow */
	palette_set(7, 10, 10, 10);    /* Gray */
	palette_set(8, 4, 4, 8);       /* Dark Panel */
	palette_set(9, 15, 8, 4);      /* Fireball Orange */
	palette_set(10, 15, 4, 4);     /* Bright Red */
	palette_set(11, 15, 4, 15);    /* Boss Attack Purple */
	palette_set(12, 4, 15, 4);     /* Charge Green */
	palette_set(13, 4, 15, 15);    /* 1P Shot Cyan */
	palette_set(14, 15, 15, 4);    /* Gold */
	palette_set(15, 15, 15, 15);   /* White */

	game.p1_wins = 0;
	game.p2_wins = 0;
	game.round = 1;
	game.tick = 0;
	init_round();

	while (running) {
		vsync_wait();

		/* 1. 1P Input (Gamepad / Arrow Keys) */
		int pad = js_stat(1);
		float speed = 3.5f;

		if (pad & JS_LEFT)  game.p1.px -= speed;
		if (pad & JS_RIGHT) game.p1.px += speed;
		if (pad & JS_UP)    game.p1.py -= speed;
		if (pad & JS_DOWN)  game.p1.py += speed;

		if (game.p1.px < P1_X + 10) game.p1.px = P1_X + 10;
		if (game.p1.px > P1_X + FIELD_W - 10) game.p1.px = P1_X + FIELD_W - 10;
		if (game.p1.py < P1_Y + 12) game.p1.py = P1_Y + 12;
		if (game.p1.py > P1_Y + FIELD_H - 12) game.p1.py = P1_Y + FIELD_H - 12;

		if (game.p1.invuln > 0) game.p1.invuln--;
		if (game.p1.shoot_timer > 0) game.p1.shoot_timer--;

		/* 1P Shooting & Charge */
		if (pad & JS_TRIG1) {
			if (game.p1.shoot_timer == 0) {
				game.p1.shoot_timer = 6;
				for (int i = 0; i < MAX_SHOTS; i++) {
					if (!game.p1.p_shots[i].active) {
						game.p1.p_shots[i].x = game.p1.px;
						game.p1.p_shots[i].y = game.p1.py - 10;
						game.p1.p_shots[i].vx = 0.0f;
						game.p1.p_shots[i].vy = -7.5f;
						game.p1.p_shots[i].color = 13;
						game.p1.p_shots[i].active = 1;
						opna_ssg_tone(1600, 3, 0);
						break;
					}
				}
			}
			if (game.p1.charge < 100) game.p1.charge++;
		} else {
			/* Release Charge Attack */
			if (game.p1.charge >= 80) {
				/* Tier 4 Boss Attack! */
				send_attack_to_opponent(&game.p2, P2_X, 5);
				opna_ssg_tone(2200, 15, 1);
			} else if (game.p1.charge >= 40) {
				/* Tier 2 Attack */
				send_attack_to_opponent(&game.p2, P2_X, 2);
				opna_ssg_tone(1800, 10, 1);
			}
			game.p1.charge = 0;
		}

		/* 2. 2P CPU AI Logic */
		if (game.p2.invuln > 0) game.p2.invuln--;
		if (game.tick % 4 == 0) {
			/* Follow nearest mob or dodge */
			float target_x = P2_X + FIELD_W / 2.0f;
			for (int i = 0; i < MAX_MOBS; i++) {
				if (game.p2.mobs[i].active) {
					target_x = game.p2.mobs[i].x;
					break;
				}
			}
			if (game.p2.px < target_x) game.p2.px += 2.5f;
			if (game.p2.px > target_x) game.p2.px -= 2.5f;

			/* CPU Auto Fire */
			for (int i = 0; i < MAX_SHOTS; i++) {
				if (!game.p2.p_shots[i].active) {
					game.p2.p_shots[i].x = game.p2.px;
					game.p2.p_shots[i].y = game.p2.py - 10;
					game.p2.p_shots[i].vx = 0.0f;
					game.p2.p_shots[i].vy = -7.5f;
					game.p2.p_shots[i].color = 10;
					game.p2.p_shots[i].active = 1;
					break;
				}
			}
		}

		/* 3. Mob Wave Spawning */
		if (game.tick % 50 == 0) {
			spawn_mob(&game.p1, P1_X, 30 + (rand() % (FIELD_W - 60)), 10);
			spawn_mob(&game.p2, P2_X, 30 + (rand() % (FIELD_W - 60)), 10);
		}

		/* Update Mobs & Collisions for 1P */
		for (int m = 0; m < MAX_MOBS; m++) {
			if (game.p1.mobs[m].active) {
				game.p1.mobs[m].x += game.p1.mobs[m].vx;
				game.p1.mobs[m].y += game.p1.mobs[m].vy;
				if (game.p1.mobs[m].x <= P1_X + 10 || game.p1.mobs[m].x >= P1_X + FIELD_W - 10)
					game.p1.mobs[m].vx = -game.p1.mobs[m].vx;

				for (int s = 0; s < MAX_SHOTS; s++) {
					if (game.p1.p_shots[s].active &&
					    fabsf(game.p1.p_shots[s].x - game.p1.mobs[m].x) < 14.0f &&
					    fabsf(game.p1.p_shots[s].y - game.p1.mobs[m].y) < 14.0f) {
						game.p1.mobs[m].active = 0;
						game.p1.p_shots[s].active = 0;
						game.p1.score += 200;
						game.p1.combo++;
						if (game.p1.combo % 3 == 0) {
							send_attack_to_opponent(&game.p2, P2_X, 2);
						}
						opna_ssg_tone(1000, 4, 1);
						break;
					}
				}

				if (game.p1.mobs[m].y > P1_Y + FIELD_H) game.p1.mobs[m].active = 0;
			}
		}

		/* Update Mobs & Collisions for 2P */
		for (int m = 0; m < MAX_MOBS; m++) {
			if (game.p2.mobs[m].active) {
				game.p2.mobs[m].x += game.p2.mobs[m].vx;
				game.p2.mobs[m].y += game.p2.mobs[m].vy;
				if (game.p2.mobs[m].x <= P2_X + 10 || game.p2.mobs[m].x >= P2_X + FIELD_W - 10)
					game.p2.mobs[m].vx = -game.p2.mobs[m].vx;

				for (int s = 0; s < MAX_SHOTS; s++) {
					if (game.p2.p_shots[s].active &&
					    fabsf(game.p2.p_shots[s].x - game.p2.mobs[m].x) < 14.0f &&
					    fabsf(game.p2.p_shots[s].y - game.p2.mobs[m].y) < 14.0f) {
						game.p2.mobs[m].active = 0;
						game.p2.p_shots[s].active = 0;
						game.p2.score += 200;
						send_attack_to_opponent(&game.p1, P1_X, 1);
						break;
					}
				}

				if (game.p2.mobs[m].y > P2_Y + FIELD_H) game.p2.mobs[m].active = 0;
			}
		}

		/* Update 1P Enemy Shots */
		for (int s = 0; s < MAX_SHOTS; s++) {
			if (game.p1.e_shots[s].active) {
				game.p1.e_shots[s].x += game.p1.e_shots[s].vx;
				game.p1.e_shots[s].y += game.p1.e_shots[s].vy;
				if (game.p1.e_shots[s].y > P1_Y + FIELD_H) game.p1.e_shots[s].active = 0;

				if (game.p1.invuln == 0 &&
				    fabsf(game.p1.e_shots[s].x - game.p1.px) < 6.0f &&
				    fabsf(game.p1.e_shots[s].y - game.p1.py) < 6.0f) {
					game.p1.lives--;
					game.p1.invuln = 60;
					opna_ssg_tone(250, 20, 0);
					for (int b = 0; b < MAX_SHOTS; b++) game.p1.e_shots[b].active = 0;
					if (game.p1.lives <= 0) {
						game.p2_wins++;
						init_round();
					}
					break;
				}
			}
		}

		/* Update 2P Enemy Shots */
		for (int s = 0; s < MAX_SHOTS; s++) {
			if (game.p2.e_shots[s].active) {
				game.p2.e_shots[s].x += game.p2.e_shots[s].vx;
				game.p2.e_shots[s].y += game.p2.e_shots[s].vy;
				if (game.p2.e_shots[s].y > P2_Y + FIELD_H) game.p2.e_shots[s].active = 0;

				if (game.p2.invuln == 0 &&
				    fabsf(game.p2.e_shots[s].x - game.p2.px) < 6.0f &&
				    fabsf(game.p2.e_shots[s].y - game.p2.py) < 6.0f) {
					game.p2.lives--;
					game.p2.invuln = 60;
					for (int b = 0; b < MAX_SHOTS; b++) game.p2.e_shots[b].active = 0;
					if (game.p2.lives <= 0) {
						game.p1_wins++;
						init_round();
					}
					break;
				}
			}
		}

		/* Update 1P/2P Player Shots */
		for (int s = 0; s < MAX_SHOTS; s++) {
			if (game.p1.p_shots[s].active) {
				game.p1.p_shots[s].y += game.p1.p_shots[s].vy;
				if (game.p1.p_shots[s].y < P1_Y) game.p1.p_shots[s].active = 0;
			}
			if (game.p2.p_shots[s].active) {
				game.p2.p_shots[s].y += game.p2.p_shots[s].vy;
				if (game.p2.p_shots[s].y < P2_Y) game.p2.p_shots[s].active = 0;
			}
		}

		/* 4. Render Dual Screen Scene */
		graph_clear();

		if (has_bg_pi) {
			pc98_pi_put(0, 0, &bg_pi);
		}

		/* 1P Left Box */
		pc98_draw_dialog_box(P1_X - 2, P1_Y - 2, FIELD_W + 4, FIELD_H + 4, 0, 1);
		/* 2P Right Box */
		pc98_draw_dialog_box(P2_X - 2, P2_Y - 2, FIELD_W + 4, FIELD_H + 4, 0, 2);

		/* Middle Center HUD Banner */
		pc98_draw_dialog_box(300, 20, 40, FIELD_H, 8, 7);
		pc98_draw_string_shadow(308, 30, "T\nH\n0\n3", 14, 0);

		char wbuf[32];
		snprintf(wbuf, sizeof(wbuf), "%d-%d", game.p1_wins, game.p2_wins);
		pc98_draw_string_shadow(304, 180, wbuf, 15, 0);

		/* 1P Mobs & Shots */
		for (int m = 0; m < MAX_MOBS; m++) {
			if (game.p1.mobs[m].active) {
				grcg_setcolor(GRCG_TCR, 14);
				grcg_boxfill((int)game.p1.mobs[m].x - 6, (int)game.p1.mobs[m].y - 6,
					     (int)game.p1.mobs[m].x + 6, (int)game.p1.mobs[m].y + 6);
				grcg_off();
			}
			if (game.p2.mobs[m].active) {
				grcg_setcolor(GRCG_TCR, 14);
				grcg_boxfill((int)game.p2.mobs[m].x - 6, (int)game.p2.mobs[m].y - 6,
					     (int)game.p2.mobs[m].x + 6, (int)game.p2.mobs[m].y + 6);
				grcg_off();
			}
		}

		/* Shots */
		for (int s = 0; s < MAX_SHOTS; s++) {
			if (game.p1.p_shots[s].active) {
				grcg_setcolor(GRCG_TCR, game.p1.p_shots[s].color);
				grcg_boxfill((int)game.p1.p_shots[s].x - 1, (int)game.p1.p_shots[s].y - 4,
					     (int)game.p1.p_shots[s].x + 1, (int)game.p1.p_shots[s].y + 4);
				grcg_off();
			}
			if (game.p1.e_shots[s].active) {
				grcg_setcolor(GRCG_TCR, game.p1.e_shots[s].color);
				grcg_circle((int)game.p1.e_shots[s].x, (int)game.p1.e_shots[s].y, 3);
				grcg_off();
			}
			if (game.p2.p_shots[s].active) {
				grcg_setcolor(GRCG_TCR, game.p2.p_shots[s].color);
				grcg_boxfill((int)game.p2.p_shots[s].x - 1, (int)game.p2.p_shots[s].y - 4,
					     (int)game.p2.p_shots[s].x + 1, (int)game.p2.p_shots[s].y + 4);
				grcg_off();
			}
			if (game.p2.e_shots[s].active) {
				grcg_setcolor(GRCG_TCR, game.p2.e_shots[s].color);
				grcg_circle((int)game.p2.e_shots[s].x, (int)game.p2.e_shots[s].y, 3);
				grcg_off();
			}
		}

		/* 1P Player */
		if (game.p1.invuln % 4 < 2) {
			if (has_p1_cdg) {
				pc98_cdg_put((int)game.p1.px - 16, (int)game.p1.py - 16, &p1_cdg);
			} else {
				grcg_setcolor(GRCG_TCR, 10);
				grcg_boxfill((int)game.p1.px - 6, (int)game.p1.py - 10,
					     (int)game.p1.px + 6, (int)game.p1.py + 10);
				grcg_off();
			}
		}

		/* 2P Player */
		if (game.p2.invuln % 4 < 2) {
			if (has_p2_cdg) {
				pc98_cdg_put((int)game.p2.px - 16, (int)game.p2.py - 16, &p2_cdg);
			} else {
				grcg_setcolor(GRCG_TCR, 13);
				grcg_boxfill((int)game.p2.px - 6, (int)game.p2.py - 10,
					     (int)game.p2.px + 6, (int)game.p2.py + 10);
				grcg_off();
			}
		}

		/* 1P & 2P HUD Headers */
		char h1[64], h2[64];
		snprintf(h1, sizeof(h1), "1P: REIMU  LIVES:%d", game.p1.lives);
		pc98_draw_string_shadow(P1_X + 8, P1_Y + 6, h1, 15, 0);

		snprintf(h2, sizeof(h2), "2P: CPU  LIVES:%d", game.p2.lives);
		pc98_draw_string_shadow(P2_X + 8, P2_Y + 6, h2, 15, 0);

		/* Charge Meters */
		int ch1 = (int)((game.p1.charge / 100.0f) * 100.0f);
		grcg_setcolor(GRCG_TCR, 12);
		grcg_boxfill(P1_X + 8, P1_Y + FIELD_H - 12, P1_X + 8 + ch1, P1_Y + FIELD_H - 6);
		grcg_off();

		game.tick++;
	}

	if (has_bg_pi) pc98_pi_free(&bg_pi);
	if (has_p1_cdg) pc98_cdg_free(&p1_cdg);
	if (has_p2_cdg) pc98_cdg_free(&p2_cdg);
	pc98_dat_close(dat_yume);
	pc98_dat_close(dat_main);

	pc98_audio_close();
	graph_end();
	printf("Touhou 3 Exited Cleanly.\n");
	return 0;
}
