// SPDX-License-Identifier: MIT
/*
 * th01_game.cpp - Touhou 1: Highly Responsive to Prayers (Rei'iden)
 * Native Linux PC-98 Port using libpc98 SDK
 *
 * Gameplay:
 *   - Breakout / Danmaku hybrid action
 *   - Yin-Yang Orb physics with Gohei batting and slide kicks
 *   - Card bumper flip grid & boss spell phases (SinGyoku & Sariel)
 *   - Hardware V-Sync, DE-9 Gamepad, OPNA SSG/FM audio effects
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

#define PLAYFIELD_LEFT    32
#define PLAYFIELD_RIGHT   608
#define PLAYFIELD_TOP     32
#define PLAYFIELD_BOTTOM  370

#define CARD_ROWS 5
#define CARD_COLS 10
#define MAX_BULLETS 128

struct Card {
	int active;
	int flips;
	uint8_t color;
};

struct Bullet {
	float x, y, vx, vy;
	uint8_t color;
	int active;
};

struct TH01_State {
	float player_x;
	float player_y;
	float player_vy;
	int is_jumping;
	int swing_timer;
	int slide_timer;
	int facing; /* -1 = left, 1 = right */

	float orb_x;
	float orb_y;
	float orb_vx;
	float orb_vy;

	int score;
	int lives;
	int bombs;
	int stage;

	Card cards[CARD_ROWS][CARD_COLS];
	int cards_remaining;

	/* Boss state */
	int is_boss;
	float boss_x, boss_y;
	float boss_vx;
	int boss_hp, boss_max_hp;
	int boss_phase;
	float boss_angle;

	Bullet bullets[MAX_BULLETS];
	uint32_t tick;
};

static TH01_State game;
static pc98_dat_t *dat_reiiden = NULL;
static pc98_dat_t *dat_fuuin   = NULL;
static pc98_pi_image_t stage_bg;
static int has_stage_bg = 0;

static void init_stage(int stg)
{
	game.stage = stg;
	game.player_x = 320.0f;
	game.player_y = 350.0f;
	game.player_vy = 0.0f;
	game.is_jumping = 0;
	game.swing_timer = 0;
	game.slide_timer = 0;

	/* Load Authentic Stage Background .PI from REIIDEN.DAT */
	char pi_name[32];
	snprintf(pi_name, sizeof(pi_name), "ST0%d.PI", (stg % 5 == 0) ? 5 : (stg % 5));
	size_t pi_sz = 0;
	uint8_t *pi_data = pc98_dat_read_file(dat_reiiden, pi_name, &pi_sz);
	if (!pi_data) pi_data = pc98_dat_read_file(dat_fuuin, pi_name, &pi_sz);
	if (pi_data) {
		if (has_stage_bg) pc98_pi_free(&stage_bg);
		if (pc98_pi_load_from_memory(pi_data, pi_sz, &stage_bg) == 0) {
			has_stage_bg = 1;
			pc98_apply_pi_palette(&stage_bg);
		}
		free(pi_data);
	}

	game.orb_x = 320.0f;
	game.orb_y = 280.0f;
	game.orb_vx = 2.0f;
	game.orb_vy = -3.5f;

	for (int i = 0; i < MAX_BULLETS; i++)
		game.bullets[i].active = 0;

	if (stg % 5 == 0) {
		/* Boss Stage */
		game.is_boss = 1;
		game.boss_x = 320.0f;
		game.boss_y = 100.0f;
		game.boss_vx = 2.0f;
		game.boss_hp = 500;
		game.boss_max_hp = 500;
		game.boss_phase = 1;
		game.boss_angle = 0.0f;
		game.cards_remaining = 0;
	} else {
		/* Card Grid Stage */
		game.is_boss = 0;
		game.cards_remaining = 0;
		for (int r = 0; r < CARD_ROWS; r++) {
			for (int c = 0; c < CARD_COLS; c++) {
				game.cards[r][c].active = 1;
				game.cards[r][c].flips = (stg > 2) ? 2 : 1;
				game.cards[r][c].color = 9 + (r % 6);
				game.cards_remaining++;
			}
		}
	}
}

static void spawn_bullet(float x, float y, float vx, float vy, uint8_t color)
{
	for (int i = 0; i < MAX_BULLETS; i++) {
		if (!game.bullets[i].active) {
			game.bullets[i].x = x;
			game.bullets[i].y = y;
			game.bullets[i].vx = vx;
			game.bullets[i].vy = vy;
			game.bullets[i].color = color;
			game.bullets[i].active = 1;
			break;
		}
	}
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	static const char *th01_dats[] = { "REIIDEN.DAT", "FUUIN.DAT" };
	pc98_require_assets("Touhou 1: Highly Responsive to Prayers", "th01", th01_dats, 2);

	dat_reiiden = pc98_dat_open("th01", "REIIDEN.DAT");
	dat_fuuin   = pc98_dat_open("th01", "FUUIN.DAT");

	printf("Starting Touhou 1: Highly Responsive to Prayers (Native PC-98 Port)...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color TH01 Palette */
	palette_set(0, 0, 0, 1);       /* Black */
	palette_set(1, 0, 0, 15);      /* Deep Blue */
	palette_set(2, 15, 0, 0);      /* Red */
	palette_set(3, 15, 0, 15);     /* Magenta */
	palette_set(4, 0, 15, 0);      /* Green */
	palette_set(5, 0, 15, 15);     /* Cyan */
	palette_set(6, 15, 15, 0);     /* Yellow */
	palette_set(7, 12, 12, 12);    /* Light Gray */
	palette_set(8, 6, 6, 6);       /* Dark Gray */
	palette_set(9, 15, 4, 4);      /* Bright Red */
	palette_set(10, 4, 15, 4);     /* Bright Green */
	palette_set(11, 4, 4, 15);     /* Bright Blue */
	palette_set(12, 15, 15, 4);    /* Gold */
	palette_set(13, 15, 8, 15);    /* Purple */
	palette_set(14, 8, 15, 15);    /* Aqua */
	palette_set(15, 15, 15, 15);   /* White */

	game.score = 0;
	game.lives = 3;
	game.bombs = 2;
	game.tick = 0;
	init_stage(1);

	while (running && game.lives >= 0) {
		vsync_wait();

		/* 1. Read Inputs */
		int pad = js_stat(1);
		float speed = 3.5f;

		if (game.swing_timer > 0) game.swing_timer--;
		if (game.slide_timer > 0) game.slide_timer--;

		if (game.slide_timer == 0) {
			if (pad & JS_LEFT) {
				game.player_x -= speed;
				game.facing = -1;
			}
			if (pad & JS_RIGHT) {
				game.player_x += speed;
				game.facing = 1;
			}
		}

		/* Jump (Up or Button B) */
		if ((pad & (JS_UP | JS_TRIG2)) && !game.is_jumping) {
			game.player_vy = -6.5f;
			game.is_jumping = 1;
			opna_ssg_tone(880, 5, 0);
		}

		/* Gohei Swing (Button A / Z) */
		if ((pad & JS_TRIG1) && game.swing_timer == 0) {
			game.swing_timer = 12;
			opna_ssg_tone(1200, 4, 0);

			/* Bat the Orb if close */
			float dx = game.orb_x - game.player_x;
			float dy = game.orb_y - game.player_y;
			if (fabsf(dx) < 32.0f && fabsf(dy) < 30.0f) {
				game.orb_vx = (game.facing * 4.0f) + (dx * 0.1f);
				game.orb_vy = -5.5f;
				opna_ssg_tone(1760, 10, 1);
			}
		}

		/* Gravity & Player Physics */
		if (game.is_jumping) {
			game.player_y += game.player_vy;
			game.player_vy += 0.35f;
			if (game.player_y >= 350.0f) {
				game.player_y = 350.0f;
				game.player_vy = 0.0f;
				game.is_jumping = 0;
			}
		}

		/* Boundaries */
		if (game.player_x < PLAYFIELD_LEFT + 16) game.player_x = PLAYFIELD_LEFT + 16;
		if (game.player_x > PLAYFIELD_RIGHT - 16) game.player_x = PLAYFIELD_RIGHT - 16;

		/* 2. Orb Physics & Card Collisions */
		game.orb_x += game.orb_vx;
		game.orb_y += game.orb_vy;
		game.orb_vy += 0.08f; /* Orb gravity */

		/* Wall Bounces */
		if (game.orb_x <= PLAYFIELD_LEFT + 12) {
			game.orb_x = PLAYFIELD_LEFT + 12;
			game.orb_vx = -game.orb_vx;
			opna_ssg_tone(900, 3, 2);
		}
		if (game.orb_x >= PLAYFIELD_RIGHT - 12) {
			game.orb_x = PLAYFIELD_RIGHT - 12;
			game.orb_vx = -game.orb_vx;
			opna_ssg_tone(900, 3, 2);
		}
		if (game.orb_y <= PLAYFIELD_TOP + 12) {
			game.orb_y = PLAYFIELD_TOP + 12;
			game.orb_vy = -game.orb_vy;
			opna_ssg_tone(900, 3, 2);
		}
		if (game.orb_y >= PLAYFIELD_BOTTOM) {
			game.orb_y = PLAYFIELD_BOTTOM;
			game.orb_vy = -4.0f;
			opna_ssg_tone(600, 5, 2);
		}

		/* Card Grid Collision */
		if (!game.is_boss) {
			for (int r = 0; r < CARD_ROWS; r++) {
				for (int c = 0; c < CARD_COLS; c++) {
					if (!game.cards[r][c].active) continue;
					int cx = PLAYFIELD_LEFT + 40 + c * 52;
					int cy = PLAYFIELD_TOP + 30 + r * 24;
					if (game.orb_x >= cx && game.orb_x <= cx + 44 &&
					    game.orb_y >= cy && game.orb_y <= cy + 18) {
						game.cards[r][c].flips--;
						if (game.cards[r][c].flips <= 0) {
							game.cards[r][c].active = 0;
							game.cards_remaining--;
							game.score += 100;
						}
						game.orb_vy = -game.orb_vy;
						opna_ssg_tone(1400, 6, 1);
						break;
					}
				}
			}

			if (game.cards_remaining == 0) {
				game.score += 5000;
				init_stage(game.stage + 1);
			}
		} else {
			/* Boss Battle Logic */
			game.boss_x += game.boss_vx;
			if (game.boss_x <= 160.0f || game.boss_x >= 480.0f)
				game.boss_vx = -game.boss_vx;

			/* Boss Hit with Orb */
			float bdx = game.orb_x - game.boss_x;
			float bdy = game.orb_y - game.boss_y;
			if (fabsf(bdx) < 30.0f && fabsf(bdy) < 30.0f) {
				game.boss_hp -= 15;
				game.score += 250;
				game.orb_vy = -game.orb_vy;
				opna_ssg_tone(2200, 8, 1);
				if (game.boss_hp <= 0) {
					game.score += 20000;
					init_stage(game.stage + 1);
				}
			}

			/* Boss Danmaku Fire */
			if (game.tick % 45 == 0) {
				for (int a = 0; a < 6; a++) {
					float ang = game.boss_angle + a * (2.0f * 3.14159f / 6.0f);
					spawn_bullet(game.boss_x, game.boss_y, cosf(ang) * 2.5f, sinf(ang) * 2.5f, 9);
				}
				game.boss_angle += 0.25f;
			}
		}

		/* Update Bullets */
		for (int i = 0; i < MAX_BULLETS; i++) {
			if (game.bullets[i].active) {
				game.bullets[i].x += game.bullets[i].vx;
				game.bullets[i].y += game.bullets[i].vy;
				if (game.bullets[i].x < PLAYFIELD_LEFT || game.bullets[i].x > PLAYFIELD_RIGHT ||
				    game.bullets[i].y < PLAYFIELD_TOP || game.bullets[i].y > PLAYFIELD_BOTTOM) {
					game.bullets[i].active = 0;
				}

				/* Player Hit */
				if (fabsf(game.bullets[i].x - game.player_x) < 8.0f &&
				    fabsf(game.bullets[i].y - game.player_y) < 12.0f) {
					game.lives--;
					game.player_x = 320.0f;
					opna_ssg_tone(300, 25, 0);
					for (int b = 0; b < MAX_BULLETS; b++) game.bullets[b].active = 0;
					break;
				}
			}
		}

		/* 3. Render Scene */
		graph_clear();

		if (has_stage_bg) {
			pc98_pi_put(0, 0, &stage_bg);
		}

		/* Playfield Frame */
		pc98_draw_dialog_box(PLAYFIELD_LEFT - 4, PLAYFIELD_TOP - 4,
				     (PLAYFIELD_RIGHT - PLAYFIELD_LEFT) + 8,
				     (PLAYFIELD_BOTTOM - PLAYFIELD_TOP) + 8, 0, 7);

		/* Top HUD */
		char hud[128];
		snprintf(hud, sizeof(hud), "SCORE:%07d  LIVES:%d  BOMBS:%d  STAGE:%02d",
			 game.score, game.lives, game.bombs, game.stage);
		pc98_draw_string_shadow(32, 10, hud, 14, 0);

		/* Cards */
		if (!game.is_boss) {
			for (int r = 0; r < CARD_ROWS; r++) {
				for (int c = 0; c < CARD_COLS; c++) {
					if (!game.cards[r][c].active) continue;
					int cx = PLAYFIELD_LEFT + 40 + c * 52;
					int cy = PLAYFIELD_TOP + 30 + r * 24;
					grcg_setcolor(GRCG_TCR, game.cards[r][c].color);
					grcg_boxfill(cx, cy, cx + 44, cy + 18);
					grcg_off();
				}
			}
		} else {
			/* Render Boss */
			grcg_setcolor(GRCG_TCR, 13);
			grcg_circle((int)game.boss_x, (int)game.boss_y, 24);
			grcg_circle((int)game.boss_x, (int)game.boss_y, 12);
			grcg_off();

			/* Boss HP Bar */
			int hp_w = (int)((game.boss_hp / (float)game.boss_max_hp) * 300.0f);
			grcg_setcolor(GRCG_TCR, 9);
			grcg_boxfill(170, 38, 170 + hp_w, 44);
			grcg_off();
		}

		/* Bullets */
		for (int i = 0; i < MAX_BULLETS; i++) {
			if (game.bullets[i].active) {
				grcg_setcolor(GRCG_TCR, game.bullets[i].color);
				grcg_boxfill((int)game.bullets[i].x - 2, (int)game.bullets[i].y - 2,
					     (int)game.bullets[i].x + 2, (int)game.bullets[i].y + 2);
				grcg_off();
			}
		}

		/* Yin-Yang Orb (Circle + Core) */
		grcg_setcolor(GRCG_TCR, 15);
		grcg_circle((int)game.orb_x, (int)game.orb_y, 10);
		grcg_setcolor(GRCG_TCR, 2);
		grcg_boxfill((int)game.orb_x - 3, (int)game.orb_y - 3, (int)game.orb_x + 3, (int)game.orb_y + 3);
		grcg_off();

		/* Reimu Player */
		grcg_setcolor(GRCG_TCR, 9);
		grcg_boxfill((int)game.player_x - 8, (int)game.player_y - 14,
			     (int)game.player_x + 8, (int)game.player_y + 14);
		grcg_off();

		/* Gohei Swing Wand */
		if (game.swing_timer > 0) {
			grcg_setcolor(GRCG_TCR, 15);
			int wx = (int)game.player_x + (game.facing * 16);
			grcg_boxfill(wx - 2, (int)game.player_y - 12, wx + 2, (int)game.player_y + 8);
			grcg_off();
		}

		game.tick++;
	}

	if (has_stage_bg) pc98_pi_free(&stage_bg);
	pc98_dat_close(dat_reiiden);
	pc98_dat_close(dat_fuuin);

	pc98_audio_close();
	graph_end();
	printf("Touhou 1 Exited Cleanly.\n");
	return 0;
}
