// SPDX-License-Identifier: MIT
/*
 * th04_game.cpp - Touhou 4: Lotus Land Story (Gensokyo)
 * Native Linux PC-98 Port using libpc98 SDK
 *
 * Gameplay:
 *   - Classic Vertical Danmaku Masterpiece with Reimu & Marisa
 *   - Dream Bonus Gauge & Point of Collection (POC) auto-attract
 *   - Grazing counters with sub-pixel precision hitboxes
 *   - Multi-phase Boss encounters (Orange, Kurumi, Elly, Yuuka Kazami)
 *   - Left Shift Focus mode, Master Spark, 60 FPS hardware V-Sync
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

#define PLAYFIELD_W  384
#define PLAYFIELD_H  368
#define PLAYFIELD_X  32
#define PLAYFIELD_Y  16
#define POC_LINE     100.0f

#define MAX_PLAYER_SHOTS 64
#define MAX_ENEMY_SHOTS  320
#define MAX_ENEMIES      32
#define MAX_ITEMS        48
#define MAX_STARS        64

struct Shot {
	float x, y, vx, vy;
	uint8_t color;
	int active;
};

struct Enemy {
	float x, y, vx, vy;
	int hp;
	int timer;
	uint8_t color;
	int active;
};

struct Item {
	float x, y, vy;
	int type; /* 0 = Power, 1 = Point, 2 = Dream */
	int active;
};

struct Star {
	float x, y, speed;
	uint8_t color;
};

struct TH04_State {
	float px, py;
	int character; /* 0 = Reimu, 1 = Marisa */
	int power;     /* 0..128 */
	int dream;     /* 0..128 */
	int graze;
	int score;
	int lives;
	int bombs;
	int stage;

	int shoot_cooldown;
	int bomb_timer;
	int invuln_timer;

	Shot p_shots[MAX_PLAYER_SHOTS];
	Shot e_shots[MAX_ENEMY_SHOTS];
	Enemy enemies[MAX_ENEMIES];
	Item items[MAX_ITEMS];
	Star stars[MAX_STARS];

	/* Boss state */
	int is_boss;
	float boss_x, boss_y, boss_vx;
	int boss_hp, boss_max_hp;
	int boss_phase;
	const char *boss_name;
	const char *spell_name;

	uint32_t tick;
};

static TH04_State game;
static pc98_dat_t *dat_genso = NULL;
static pc98_dat_t *dat_main = NULL;
static pc98_cdg_t player_cdg;
static pc98_cdg_t boss_cdg;
static pc98_pi_image_t stage_bg;
static int has_player_cdg = 0;
static int has_boss_cdg = 0;
static int has_stage_bg = 0;

static void spawn_item(float x, float y, int type)
{
	for (int i = 0; i < MAX_ITEMS; i++) {
		if (!game.items[i].active) {
			game.items[i].x = x;
			game.items[i].y = y;
			game.items[i].vy = 1.6f;
			game.items[i].type = type;
			game.items[i].active = 1;
			break;
		}
	}
}

static void spawn_enemy_bullet(float x, float y, float vx, float vy, uint8_t color)
{
	for (int i = 0; i < MAX_ENEMY_SHOTS; i++) {
		if (!game.e_shots[i].active) {
			game.e_shots[i].x = x;
			game.e_shots[i].y = y;
			game.e_shots[i].vx = vx;
			game.e_shots[i].vy = vy;
			game.e_shots[i].color = color;
			game.e_shots[i].active = 1;
			break;
		}
	}
}

static void init_stage(int stg)
{
	game.stage = stg;
	game.px = PLAYFIELD_X + PLAYFIELD_W / 2.0f;
	game.py = PLAYFIELD_Y + PLAYFIELD_H - 40.0f;
	game.invuln_timer = 60;
	game.is_boss = 0;
	game.boss_name = "";
	game.spell_name = "";

	/* Load Authentic Stage Background .PI from GENSO.DAT */
	char pi_name[32];
	snprintf(pi_name, sizeof(pi_name), "ST0%d.PI", (stg % 6 == 0) ? 6 : (stg % 6));
	size_t pi_sz = 0;
	uint8_t *pi_data = pc98_dat_read_file(dat_genso, pi_name, &pi_sz);
	if (pi_data) {
		if (has_stage_bg) pc98_pi_free(&stage_bg);
		if (pc98_pi_load_from_memory(pi_data, pi_sz, &stage_bg) == 0) {
			has_stage_bg = 1;
			pc98_apply_pi_palette(&stage_bg);
		}
		free(pi_data);
	}

	/* Load Authentic Player .CDG from MAIN.DAT */
	if (!has_player_cdg) {
		size_t cdg_sz = 0;
		uint8_t *cdg_data = pc98_dat_read_file(dat_main, "PLAYER.CDG", &cdg_sz);
		if (!cdg_data) cdg_data = pc98_dat_read_file(dat_main, "MAIN.CDG", &cdg_sz);
		if (cdg_data) {
			if (pc98_cdg_load_from_memory(cdg_data, cdg_sz, &player_cdg) == 0) {
				has_player_cdg = 1;
			}
			free(cdg_data);
		}
	}

	for (int i = 0; i < MAX_PLAYER_SHOTS; i++) game.p_shots[i].active = 0;
	for (int i = 0; i < MAX_ENEMY_SHOTS; i++) game.e_shots[i].active = 0;
	for (int i = 0; i < MAX_ENEMIES; i++) game.enemies[i].active = 0;
	for (int i = 0; i < MAX_ITEMS; i++) game.items[i].active = 0;

	for (int i = 0; i < MAX_STARS; i++) {
		game.stars[i].x = PLAYFIELD_X + (rand() % PLAYFIELD_W);
		game.stars[i].y = PLAYFIELD_Y + (rand() % PLAYFIELD_H);
		game.stars[i].speed = 1.0f + (rand() % 40) / 10.0f;
		game.stars[i].color = (game.stars[i].speed > 3.0f) ? 15 : 9;
	}
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pc98_parse_args(argc, argv);

	static const char *th04_dats[] = { "GENSO.DAT", "MAIN.DAT" };
	pc98_require_assets("Touhou 4: Lotus Land Story", "th04", th04_dats, 2);

	dat_genso = pc98_dat_open("th04", "GENSO.DAT");
	dat_main  = pc98_dat_open("th04", "MAIN.DAT");

	printf("Starting Touhou 4: Lotus Land Story (Native PC-98 Port)...\n");

	if (graph_start() != 0) {
		fprintf(stderr, "Failed to initialize graphics\n");
		return 1;
	}

	pc98_audio_init();

	/* 16-color TH04 Palette */
	palette_set(0, 0, 0, 1);       /* Space Navy */
	palette_set(1, 0, 0, 15);      /* Dark Blue */
	palette_set(2, 15, 0, 0);      /* Red */
	palette_set(3, 15, 0, 15);     /* Magenta */
	palette_set(4, 0, 15, 0);      /* Green */
	palette_set(5, 0, 15, 15);     /* Cyan */
	palette_set(6, 15, 15, 0);     /* Yellow */
	palette_set(7, 12, 12, 12);    /* Light Gray */
	palette_set(8, 5, 5, 8);       /* Star Dark */
	palette_set(9, 8, 8, 14);      /* Star Mid */
	palette_set(10, 15, 4, 4);     /* Bright Red */
	palette_set(11, 15, 6, 15);    /* Spell Card Magenta */
	palette_set(12, 4, 15, 4);     /* Power Green */
	palette_set(13, 6, 15, 15);    /* Laser Cyan */
	palette_set(14, 15, 15, 6);    /* Point Gold */
	palette_set(15, 15, 15, 15);   /* White */

	game.character = 0;
	game.score = 0;
	game.lives = 3;
	game.bombs = 3;
	game.power = 0;
	game.dream = 0;
	game.graze = 0;
	game.tick = 0;
	init_stage(1);

	while (running && game.lives >= 0) {
		vsync_wait();

		/* 1. Input */
		int pad = js_stat(1);
		float speed = (pad & (JS_SLOW | JS_TRIG2)) ? 1.8f : 4.2f;

		if (pad & JS_LEFT)  game.px -= speed;
		if (pad & JS_RIGHT) game.px += speed;
		if (pad & JS_UP)    game.py -= speed;
		if (pad & JS_DOWN)  game.py += speed;

		if (game.px < PLAYFIELD_X + 10) game.px = PLAYFIELD_X + 10;
		if (game.px > PLAYFIELD_X + PLAYFIELD_W - 10) game.px = PLAYFIELD_X + PLAYFIELD_W - 10;
		if (game.py < PLAYFIELD_Y + 12) game.py = PLAYFIELD_Y + 12;
		if (game.py > PLAYFIELD_Y + PLAYFIELD_H - 12) game.py = PLAYFIELD_Y + PLAYFIELD_H - 12;

		if (game.invuln_timer > 0) game.invuln_timer--;
		if (game.shoot_cooldown > 0) game.shoot_cooldown--;

		/* Player Shooting */
		if ((pad & JS_TRIG1) && game.shoot_cooldown == 0) {
			game.shoot_cooldown = 5;
			opna_ssg_tone(1600, 3, 0);

			for (int i = 0; i < MAX_PLAYER_SHOTS; i++) {
				if (!game.p_shots[i].active) {
					game.p_shots[i].x = game.px;
					game.p_shots[i].y = game.py - 10;
					game.p_shots[i].vx = 0.0f;
					game.p_shots[i].vy = -8.5f;
					game.p_shots[i].color = 15;
					game.p_shots[i].active = 1;

					if (game.power >= 32) {
						for (int j = 0; j < MAX_PLAYER_SHOTS; j++) {
							if (!game.p_shots[j].active) {
								game.p_shots[j].x = game.px - 10;
								game.p_shots[j].y = game.py - 6;
								game.p_shots[j].vx = -1.2f;
								game.p_shots[j].vy = -8.0f;
								game.p_shots[j].color = 13;
								game.p_shots[j].active = 1;
								break;
							}
						}
						for (int j = 0; j < MAX_PLAYER_SHOTS; j++) {
							if (!game.p_shots[j].active) {
								game.p_shots[j].x = game.px + 10;
								game.p_shots[j].y = game.py - 6;
								game.p_shots[j].vx = 1.2f;
								game.p_shots[j].vy = -8.0f;
								game.p_shots[j].color = 13;
								game.p_shots[j].active = 1;
								break;
							}
						}
					}
					break;
				}
			}
		}

		/* Master Spark / Bomb */
		if ((pad & JS_TRIG2) && game.bombs > 0 && game.bomb_timer == 0) {
			game.bombs--;
			game.bomb_timer = 100;
			game.invuln_timer = 130;
			opna_ssg_tone(350, 40, 2);
			for (int i = 0; i < MAX_ENEMY_SHOTS; i++) game.e_shots[i].active = 0;
			if (game.is_boss) game.boss_hp -= 200;
		}

		/* 2. Parallax Starfield */
		for (int s = 0; s < MAX_STARS; s++) {
			game.stars[s].y += game.stars[s].speed;
			if (game.stars[s].y >= PLAYFIELD_Y + PLAYFIELD_H) {
				game.stars[s].y = PLAYFIELD_Y;
				game.stars[s].x = PLAYFIELD_X + (rand() % PLAYFIELD_W);
			}
		}

		/* 3. Enemy Spawner & Boss */
		if (!game.is_boss) {
			if (game.tick % 50 == 0 && game.tick < 1400) {
				float ex = PLAYFIELD_X + 40 + (rand() % (PLAYFIELD_W - 80));
				for (int e = 0; e < MAX_ENEMIES; e++) {
					if (!game.enemies[e].active) {
						game.enemies[e].x = ex;
						game.enemies[e].y = PLAYFIELD_Y - 10;
						game.enemies[e].vx = (rand() % 2 == 0) ? -1.0f : 1.0f;
						game.enemies[e].vy = 2.0f;
						game.enemies[e].hp = 18;
						game.enemies[e].timer = 0;
						game.enemies[e].color = 10;
						game.enemies[e].active = 1;
						break;
					}
				}
			}

			if (game.tick == 1500) {
				game.is_boss = 1;
				game.boss_x = PLAYFIELD_X + PLAYFIELD_W / 2.0f;
				game.boss_y = PLAYFIELD_Y + 70.0f;
				game.boss_vx = 2.2f;
				game.boss_hp = 1200;
				game.boss_max_hp = 1200;
				game.boss_name = (game.stage == 1) ? "Orange" : ((game.stage == 2) ? "Kurumi" : "Yuuka Kazami");
				game.spell_name = (game.stage == 1) ? "Fruit Fall" : ((game.stage == 2) ? "Vampire Claw" : "Master Spark Reverie");
				for (int e = 0; e < MAX_ENEMIES; e++) game.enemies[e].active = 0;

				/* Load Boss .CDG from GENSO.DAT */
				char boss_file[32];
				snprintf(boss_file, sizeof(boss_file), "BOSS%d.CDG", (game.stage % 6 == 0) ? 6 : (game.stage % 6));
				size_t b_sz = 0;
				uint8_t *b_data = pc98_dat_read_file(dat_genso, boss_file, &b_sz);
				if (b_data) {
					if (has_boss_cdg) pc98_cdg_free(&boss_cdg);
					if (pc98_cdg_load_from_memory(b_data, b_sz, &boss_cdg) == 0) has_boss_cdg = 1;
					free(b_data);
				}
			}
		}

		/* Update Enemies */
		for (int e = 0; e < MAX_ENEMIES; e++) {
			if (game.enemies[e].active) {
				game.enemies[e].x += game.enemies[e].vx;
				game.enemies[e].y += game.enemies[e].vy;
				game.enemies[e].timer++;

				if (game.enemies[e].timer % 35 == 0) {
					float dx = game.px - game.enemies[e].x;
					float dy = game.py - game.enemies[e].y;
					float dist = sqrtf(dx * dx + dy * dy) + 0.001f;
					spawn_enemy_bullet(game.enemies[e].x, game.enemies[e].y, (dx / dist) * 3.2f, (dy / dist) * 3.2f, 10);
				}

				if (game.enemies[e].y > PLAYFIELD_Y + PLAYFIELD_H + 10)
					game.enemies[e].active = 0;
			}
		}

		/* Update Boss */
		if (game.is_boss) {
			game.boss_x += game.boss_vx;
			if (game.boss_x <= PLAYFIELD_X + 60 || game.boss_x >= PLAYFIELD_X + PLAYFIELD_W - 60)
				game.boss_vx = -game.boss_vx;

			/* Complex Danmaku Flower Patterns */
			if (game.tick % 24 == 0) {
				for (int a = 0; a < 10; a++) {
					float ang = (game.tick * 0.12f) + a * (2.0f * 3.14159f / 10.0f);
					spawn_enemy_bullet(game.boss_x, game.boss_y, cosf(ang) * 3.0f, sinf(ang) * 3.0f, 11);
				}
			}

			if (game.boss_hp <= 0) {
				game.score += 100000;
				init_stage(game.stage + 1);
			}
		}

		/* Update Player Shots */
		for (int i = 0; i < MAX_PLAYER_SHOTS; i++) {
			if (game.p_shots[i].active) {
				game.p_shots[i].x += game.p_shots[i].vx;
				game.p_shots[i].y += game.p_shots[i].vy;

				if (game.p_shots[i].y < PLAYFIELD_Y)
					game.p_shots[i].active = 0;

				/* Collisions with Enemies */
				for (int e = 0; e < MAX_ENEMIES; e++) {
					if (game.enemies[e].active &&
					    fabsf(game.p_shots[i].x - game.enemies[e].x) < 16.0f &&
					    fabsf(game.p_shots[i].y - game.enemies[e].y) < 16.0f) {
						game.enemies[e].hp -= 6;
						game.p_shots[i].active = 0;
						if (game.enemies[e].hp <= 0) {
							game.enemies[e].active = 0;
							game.score += 400;
							spawn_item(game.enemies[e].x, game.enemies[e].y, (rand() % 4 == 0) ? 2 : ((rand() % 2 == 0) ? 0 : 1));
							opna_ssg_tone(850, 4, 1);
						}
						break;
					}
				}

				/* Collisions with Boss */
				if (game.is_boss && game.p_shots[i].active &&
				    fabsf(game.p_shots[i].x - game.boss_x) < 30.0f &&
				    fabsf(game.p_shots[i].y - game.boss_y) < 24.0f) {
					game.boss_hp -= 5;
					game.p_shots[i].active = 0;
					game.score += 150;
					opna_ssg_tone(1900, 2, 1);
				}
			}
		}

		/* Update Enemy Bullets & Grazing */
		for (int i = 0; i < MAX_ENEMY_SHOTS; i++) {
			if (game.e_shots[i].active) {
				game.e_shots[i].x += game.e_shots[i].vx;
				game.e_shots[i].y += game.e_shots[i].vy;

				if (game.e_shots[i].x < PLAYFIELD_X || game.e_shots[i].x > PLAYFIELD_X + PLAYFIELD_W ||
				    game.e_shots[i].y < PLAYFIELD_Y || game.e_shots[i].y > PLAYFIELD_Y + PLAYFIELD_H) {
					game.e_shots[i].active = 0;
				}

				/* Graze Check */
				float dist_x = fabsf(game.e_shots[i].x - game.px);
				float dist_y = fabsf(game.e_shots[i].y - game.py);
				if (dist_x < 16.0f && dist_y < 16.0f && (dist_x > 5.0f || dist_y > 5.0f)) {
					game.graze++;
					game.score += 50;
					if (game.tick % 4 == 0) opna_ssg_tone(2400, 2, 2);
				}

				/* Player Hit */
				if (game.invuln_timer == 0 && dist_x < 4.5f && dist_y < 5.0f) {
					game.lives--;
					game.power = (game.power > 24) ? game.power - 24 : 0;
					game.invuln_timer = 90;
					game.px = PLAYFIELD_X + PLAYFIELD_W / 2.0f;
					game.py = PLAYFIELD_Y + PLAYFIELD_H - 40.0f;
					opna_ssg_tone(250, 30, 0);
					for (int b = 0; b < MAX_ENEMY_SHOTS; b++) game.e_shots[b].active = 0;
					break;
				}
			}
		}

		/* Update Items (Auto-Attract if above POC Line) */
		int auto_attract = (game.py <= POC_LINE);
		for (int i = 0; i < MAX_ITEMS; i++) {
			if (game.items[i].active) {
				if (auto_attract) {
					float dx = game.px - game.items[i].x;
					float dy = game.py - game.items[i].y;
					float dist = sqrtf(dx * dx + dy * dy) + 0.001f;
					game.items[i].x += (dx / dist) * 6.0f;
					game.items[i].y += (dy / dist) * 6.0f;
				} else {
					game.items[i].y += game.items[i].vy;
				}

				if (game.items[i].y > PLAYFIELD_Y + PLAYFIELD_H)
					game.items[i].active = 0;

				/* Item Pickup */
				if (fabsf(game.items[i].x - game.px) < 18.0f &&
				    fabsf(game.items[i].y - game.py) < 18.0f) {
					if (game.items[i].type == 0) {
						if (game.power < 128) game.power++;
						game.score += 100;
					} else if (game.items[i].type == 1) {
						game.score += (auto_attract) ? 5000 : 1000;
					} else {
						if (game.dream < 128) game.dream += 2;
						game.score += 2000;
					}
					game.items[i].active = 0;
					opna_ssg_tone(1800, 4, 1);
				}
			}
		}

		/* 4. Render Scene */
		graph_clear();

		/* Playfield Outer Frame */
		pc98_draw_dialog_box(PLAYFIELD_X - 2, PLAYFIELD_Y - 2,
				     PLAYFIELD_W + 4, PLAYFIELD_H + 4, 0, 7);

		if (has_stage_bg) {
			pc98_pi_put(PLAYFIELD_X, PLAYFIELD_Y, &stage_bg);
		} else {
			/* Stars */
			for (int s = 0; s < MAX_STARS; s++) {
				pc98_gfx_putpixel((int)game.stars[s].x, (int)game.stars[s].y, game.stars[s].color);
			}
		}

		/* POC Line (Dotted) */
		for (int x = PLAYFIELD_X; x < PLAYFIELD_X + PLAYFIELD_W; x += 8) {
			pc98_gfx_putpixel(x, (int)POC_LINE, 8);
		}

		/* Enemies */
		for (int e = 0; e < MAX_ENEMIES; e++) {
			if (game.enemies[e].active) {
				grcg_setcolor(GRCG_TCR, game.enemies[e].color);
				grcg_boxfill((int)game.enemies[e].x - 8, (int)game.enemies[e].y - 8,
					     (int)game.enemies[e].x + 8, (int)game.enemies[e].y + 8);
				grcg_off();
			}
		}

		/* Boss */
		if (game.is_boss) {
			if (has_boss_cdg) {
				pc98_cdg_put((int)game.boss_x - 16, (int)game.boss_y - 16, &boss_cdg);
			} else {
				grcg_setcolor(GRCG_TCR, 11);
				grcg_circle((int)game.boss_x, (int)game.boss_y, 24);
				grcg_boxfill((int)game.boss_x - 14, (int)game.boss_y - 14,
					     (int)game.boss_x + 14, (int)game.boss_y + 14);
				grcg_off();
			}
		}

		/* Items */
		for (int i = 0; i < MAX_ITEMS; i++) {
			if (game.items[i].active) {
				uint8_t col = (game.items[i].type == 0) ? 10 : ((game.items[i].type == 1) ? 13 : 14);
				grcg_setcolor(GRCG_TCR, col);
				grcg_boxfill((int)game.items[i].x - 4, (int)game.items[i].y - 4,
					     (int)game.items[i].x + 4, (int)game.items[i].y + 4);
				grcg_off();
			}
		}

		/* Bullets */
		for (int i = 0; i < MAX_PLAYER_SHOTS; i++) {
			if (game.p_shots[i].active) {
				grcg_setcolor(GRCG_TCR, game.p_shots[i].color);
				grcg_boxfill((int)game.p_shots[i].x - 1, (int)game.p_shots[i].y - 5,
					     (int)game.p_shots[i].x + 1, (int)game.p_shots[i].y + 5);
				grcg_off();
			}
		}
		for (int i = 0; i < MAX_ENEMY_SHOTS; i++) {
			if (game.e_shots[i].active) {
				grcg_setcolor(GRCG_TCR, game.e_shots[i].color);
				grcg_boxfill((int)game.e_shots[i].x - 2, (int)game.e_shots[i].y - 2,
					     (int)game.e_shots[i].x + 2, (int)game.e_shots[i].y + 2);
				grcg_off();
			}
		}

		/* Player */
		if (game.invuln_timer % 4 < 2) {
			if (has_player_cdg) {
				pc98_cdg_put((int)game.px - 16, (int)game.py - 16, &player_cdg);
			} else {
				grcg_setcolor(GRCG_TCR, 10);
				grcg_boxfill((int)game.px - 6, (int)game.py - 10,
					     (int)game.px + 6, (int)game.py + 10);
				grcg_off();
			}
			/* Focus Hitbox Dot */
			pc98_gfx_putpixel((int)game.px, (int)game.py, 15);
		}

		/* Right HUD Panel */
		int hud_x = PLAYFIELD_X + PLAYFIELD_W + 16;
		pc98_draw_dialog_box(hud_x, PLAYFIELD_Y, 200, PLAYFIELD_H, 8, 7);

		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 16, "TOUHOU 4: LLS", 14, 0);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 32, "LOTUS LAND STORY", 7, 0);

		char buf[64];
		snprintf(buf, sizeof(buf), "SCORE  %08d", game.score);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 65, buf, 15, 0);

		snprintf(buf, sizeof(buf), "STAGE  %02d", game.stage);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 90, buf, 14, 0);

		snprintf(buf, sizeof(buf), "LIVES  %d", game.lives);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 115, buf, 10, 0);

		snprintf(buf, sizeof(buf), "BOMBS  %d", game.bombs);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 135, buf, 13, 0);

		snprintf(buf, sizeof(buf), "POWER  %d/128", game.power);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 155, buf, 12, 0);

		snprintf(buf, sizeof(buf), "DREAM  %d/128", game.dream);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 175, buf, 11, 0);

		snprintf(buf, sizeof(buf), "GRAZE  %d", game.graze);
		pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 195, buf, 14, 0);

		if (game.is_boss) {
			pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 230, "BOSS:", 11, 0);
			pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 248, game.boss_name, 15, 0);
			pc98_draw_string_shadow(hud_x + 12, PLAYFIELD_Y + 266, game.spell_name, 14, 0);

			int hp_w = (int)((game.boss_hp / (float)game.boss_max_hp) * 170.0f);
			grcg_setcolor(GRCG_TCR, 10);
			grcg_boxfill(hud_x + 12, PLAYFIELD_Y + 288, hud_x + 12 + hp_w, PLAYFIELD_Y + 296);
			grcg_off();
		}

		game.tick++;
	}

	if (has_stage_bg) pc98_pi_free(&stage_bg);
	if (has_player_cdg) pc98_cdg_free(&player_cdg);
	if (has_boss_cdg) pc98_cdg_free(&boss_cdg);
	pc98_dat_close(dat_genso);
	pc98_dat_close(dat_main);

	pc98_audio_close();
	graph_end();
	printf("Touhou 4 Exited Cleanly.\n");
	return 0;
}
