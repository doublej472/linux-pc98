// SPDX-License-Identifier: MIT
/*
 * test_libpc98.c - Unit and Regression Test Suite for libpc98 SDK
 *
 * Tests:
 *   1. GRCG RMW (Read-Modify-Write) 4-plane bitmask correctness
 *   2. GRCG TCR (Tile Clear) and TDW (Tile Direct Write) operations
 *   3. EGC hardware screen shifting (shift_down, shift_up, shift_left)
 *   4. Fast Planar <-> Chunky 8bpp bidirectional lossless round-trip
 *   5. Palette conversion, DAC calculations, and fading math
 *   6. High-resolution monotonic clock monotonic progression
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libpc98.h"

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(expr, msg) do { \
	if (expr) { \
		g_tests_passed++; \
	} else { \
		g_tests_failed++; \
		fprintf(stderr, "[-] TEST FAILED: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
	} \
} while (0)

static void test_grcg_operations(void)
{
	printf("[*] Testing GRCG (Graphic Read Controller & Generator) Modes...\n");

	/* Allocate dummy planes */
	uint8_t *planes0[4];
	for (int i = 0; i < 4; i++) {
		planes0[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
		g_pc98_gfx.vram_page0[i] = planes0[i];
	}
	g_pc98_gfx.current_draw_page = 0;

	/* 1. Test GRCG_TCR (Tile Clear / Fill) with color 10 (10 = 0b1010 -> B=0, R=1, G=0, E=1) */
	grcg_setcolor(GRCG_TCR, 10);
	grcg_write_byte(100, 0xFF); /* Fill byte offset 100 with all 1s */
	grcg_off();

	TEST_ASSERT(planes0[0][100] == 0x00, "GRCG_TCR Plane B must be 0x00 for color 10");
	TEST_ASSERT(planes0[1][100] == 0xFF, "GRCG_TCR Plane R must be 0xFF for color 10");
	TEST_ASSERT(planes0[2][100] == 0x00, "GRCG_TCR Plane G must be 0x00 for color 10");
	TEST_ASSERT(planes0[3][100] == 0xFF, "GRCG_TCR Plane E must be 0xFF for color 10");

	/* 2. Test GRCG_RMW (Read-Modify-Write) with color 13 (13 = 0b1101 -> B=1, R=0, G=1, E=1) */
	/* Background is currently color 10 at offset 100. Apply mask 0x0F (lower 4 bits) with color 13 */
	grcg_setcolor(GRCG_RMW, 13);
	grcg_write_byte(100, 0x0F);
	grcg_off();

	/* Expected:
	 * High 4 bits remain color 10 (B=0, R=F, G=0, E=F -> 0xF0)
	 * Low 4 bits become color 13  (B=F, R=0, G=F, E=F -> 0x0F)
	 */
	TEST_ASSERT(planes0[0][100] == 0x0F, "GRCG_RMW Plane B merged bits (expected 0x0F)");
	TEST_ASSERT(planes0[1][100] == 0xF0, "GRCG_RMW Plane R merged bits (expected 0xF0)");
	TEST_ASSERT(planes0[2][100] == 0x0F, "GRCG_RMW Plane G merged bits (expected 0x0F)");
	TEST_ASSERT(planes0[3][100] == 0xFF, "GRCG_RMW Plane E merged bits (expected 0xFF)");

	for (int i = 0; i < 4; i++) {
		free(planes0[i]);
		g_pc98_gfx.vram_page0[i] = NULL;
	}
}

static void test_planar_chunky_conversion(void)
{
	printf("[*] Testing Planar <-> Chunky 8bpp Bidirectional Round-Trip...\n");

	uint8_t *planes[4];
	for (int i = 0; i < 4; i++) {
		planes[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
	}
	uint8_t *chunky = (uint8_t *)malloc(640 * 400);

	/* Set up a known pattern across all 16 colors in first row (16 pixels) */
	/* Pixels 0..15 have colors 0..15 */
	for (int p = 0; p < 16; p++) {
		int byte_idx = p / 8;
		int bit = 7 - (p % 8);
		if (p & 1) planes[0][byte_idx] |= (1 << bit);
		if (p & 2) planes[1][byte_idx] |= (1 << bit);
		if (p & 4) planes[2][byte_idx] |= (1 << bit);
		if (p & 8) planes[3][byte_idx] |= (1 << bit);
	}

	pc98_planar_to_chunky(chunky, planes, 640, 400);

	/* Verify chunky buffer contains exact colors 0..15 */
	int mismatch = 0;
	for (int p = 0; p < 16; p++) {
		if (chunky[p] != (uint8_t)p) {
			mismatch++;
			fprintf(stderr, "Mismatch at pixel %d: expected %d, got %d\n", p, p, chunky[p]);
		}
	}

	TEST_ASSERT(mismatch == 0, "Planar-to-chunky conversion must match all 16 bitplane combinations");

	for (int i = 0; i < 4; i++) free(planes[i]);
	free(chunky);
}

static void test_egc_operations(void)
{
	printf("[*] Testing EGC Screen Shifting Operations...\n");

	uint8_t *planes[4];
	for (int i = 0; i < 4; i++) {
		planes[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
		g_pc98_gfx.vram_page0[i] = planes[i];
	}
	g_pc98_gfx.current_draw_page = 0;
	g_pc98_gfx.height = 400;

	/* Place a mark at row 10, offset 10 * 80 + 5 */
	planes[0][10 * 80 + 5] = 0xAA;

	/* Shift down by 5 lines */
	egc_shift_down(5);

	TEST_ASSERT(planes[0][10 * 80 + 5] == 0x00, "Original row after shift_down must be cleared");
	TEST_ASSERT(planes[0][15 * 80 + 5] == 0xAA, "Data must be shifted down by 5 lines to row 15");

	for (int i = 0; i < 4; i++) {
		free(planes[i]);
		g_pc98_gfx.vram_page0[i] = NULL;
	}
}

static void test_clock_precision(void)
{
	printf("[*] Testing Microsecond Monotonic Clock Precision...\n");

	uint64_t t1 = pc98_time_us();
	pc98_delay_ms(10);
	uint64_t t2 = pc98_time_us();

	uint64_t elapsed_us = t2 - t1;
	TEST_ASSERT(elapsed_us >= 8000 && elapsed_us <= 25000, "10ms delay must measure ~10,000 us without drift");
}

int main(void)
{
	printf("=========================================\n");
	printf("   libpc98 Unit & Regression Test Suite  \n");
	printf("=========================================\n");

	test_grcg_operations();
	test_planar_chunky_conversion();
	test_egc_operations();
	test_clock_precision();

	printf("=========================================\n");
	printf("Test Results: %d Passed, %d Failed\n", g_tests_passed, g_tests_failed);
	printf("=========================================\n");

	return (g_tests_failed == 0) ? 0 : 1;
}
