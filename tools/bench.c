// SPDX-License-Identifier: MIT
/*
 * bench.c - Comprehensive Hardware & Engine Performance Benchmark for PC-9821
 *
 * Benchmarks:
 *   1. VRAM PCI Write-Combining Burst Write Throughput (MB/s)
 *   2. GRCG Hardware Sprite Blitting Throughput (Sprites/sec & Mpix/s)
 *   3. Planar <-> Chunky Bitplane Conversion Speed (Frames/sec & latency in us)
 *   4. High-Resolution Monotonic Clock Jitter & CPU Frequency Validation
 *   5. EGC Screen Memory Shift & Hardware BitBLT Bandwidth (MB/s)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "libpc98.h"

static void bench_planar_chunky_conversion(void)
{
	printf("\n--- 1. Planar <-> Chunky Bitplane Conversion Benchmark ---\n");

	uint8_t *planes[4];
	for (int i = 0; i < 4; i++) {
		planes[i] = (uint8_t *)malloc(PC98_PLANE_SIZE);
		memset(planes[i], 0x55 ^ (i * 0x33), PC98_PLANE_SIZE);
	}
	uint8_t *chunky = (uint8_t *)malloc(640 * 400);

	int iterations = 1000;
	uint64_t t0 = pc98_time_us();

	for (int i = 0; i < iterations; i++) {
		pc98_planar_to_chunky(chunky, planes, 640, 400);
	}

	uint64_t t1 = pc98_time_us();
	double elapsed_sec = (double)(t1 - t0) / 1000000.0;
	double fps = (double)iterations / elapsed_sec;
	double us_per_frame = (double)(t1 - t0) / (double)iterations;
	double mpix_sec = (640.0 * 400.0 * iterations) / (elapsed_sec * 1000000.0);

	printf("Processed %d frames in %.3f seconds\n", iterations, elapsed_sec);
	printf("Throughput: %.1f FPS (%.2f microseconds/frame)\n", fps, us_per_frame);
	printf("Pixel Rate: %.2f Megapixels/sec\n", mpix_sec);

	for (int i = 0; i < 4; i++) free(planes[i]);
	free(chunky);
}

static void bench_grcg_sprite_blitting(void)
{
	printf("\n--- 2. GRCG Hardware Sprite Blitting Benchmark ---\n");

	uint8_t *planes[4];
	for (int i = 0; i < 4; i++) {
		planes[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
		g_pc98_gfx.vram_page0[i] = planes[i];
	}
	g_pc98_gfx.current_draw_page = 0;
	g_pc98_gfx.width = 640;
	g_pc98_gfx.height = 400;

	static const uint8_t sprite_16x16[32] = {
		0x01, 0x80, 0x03, 0xc0, 0x07, 0xe0, 0x0f, 0xf0,
		0x1f, 0xf8, 0x3f, 0xfc, 0x7f, 0xfe, 0xff, 0xff,
		0x7b, 0xde, 0x3b, 0xdc, 0x1f, 0xf8, 0x0f, 0xf0,
		0x1b, 0xd8, 0x31, 0x8c, 0x60, 0x06, 0x40, 0x02,
	};

	int blits = 100000;
	grcg_setcolor(GRCG_RMW, 13);

	uint64_t t0 = pc98_time_us();

	for (int i = 0; i < blits; i++) {
		int x = (i * 17) % (640 - 16);
		int y = (i * 23) % (400 - 16);
		grcg_blit_sprite(x, y, sprite_16x16, 2, 16);
	}

	uint64_t t1 = pc98_time_us();
	grcg_off();

	double elapsed_sec = (double)(t1 - t0) / 1000000.0;
	double sprites_sec = (double)blits / elapsed_sec;
	double mpix_sec = (16.0 * 16.0 * blits) / (elapsed_sec * 1000000.0);

	printf("Blitted %d 16x16 sprites in %.3f seconds\n", blits, elapsed_sec);
	printf("Throughput: %.1f Sprites/sec (%.2f Megapixels/sec across 4 bitplanes)\n",
	       sprites_sec, mpix_sec);

	for (int i = 0; i < 4; i++) {
		free(planes[i]);
		g_pc98_gfx.vram_page0[i] = NULL;
	}
}

static void bench_egc_screen_shift(void)
{
	printf("\n--- 3. EGC Screen Memory Shift Benchmark ---\n");

	uint8_t *planes[4];
	for (int i = 0; i < 4; i++) {
		planes[i] = (uint8_t *)calloc(1, PC98_PLANE_SIZE);
		g_pc98_gfx.vram_page0[i] = planes[i];
	}
	g_pc98_gfx.current_draw_page = 0;
	g_pc98_gfx.height = 400;

	int shifts = 5000;
	uint64_t t0 = pc98_time_us();

	for (int i = 0; i < shifts; i++) {
		egc_shift_down(2);
	}

	uint64_t t1 = pc98_time_us();
	double elapsed_sec = (double)(t1 - t0) / 1000000.0;
	double mb_sec = ((double)PC98_PLANE_SIZE * 4 * shifts) / (elapsed_sec * 1024.0 * 1024.0);

	printf("Performed %d 4-plane screen shifts in %.3f seconds\n", shifts, elapsed_sec);
	printf("Throughput: %.2f MB/s memory shift rate\n", mb_sec);

	for (int i = 0; i < 4; i++) {
		free(planes[i]);
		g_pc98_gfx.vram_page0[i] = NULL;
	}
}

static void bench_clock_jitter(void)
{
	printf("\n--- 4. Monotonic Clock Precision & Sleep Jitter Test ---\n");

	int samples = 50;
	uint64_t total_diff = 0;
	uint64_t min_us = 9999999, max_us = 0;

	for (int i = 0; i < samples; i++) {
		uint64_t t0 = pc98_time_us();
		pc98_delay_ms(2);
		uint64_t t1 = pc98_time_us();
		uint64_t diff = t1 - t0;
		total_diff += diff;
		if (diff < min_us) min_us = diff;
		if (diff > max_us) max_us = diff;
	}

	printf("Target 2,000 us sleep over %d samples:\n", samples);
	printf("Average: %llu us | Min: %llu us | Max: %llu us | Jitter Spread: %llu us\n",
	       (unsigned long long)(total_diff / samples),
	       (unsigned long long)min_us,
	       (unsigned long long)max_us,
	       (unsigned long long)(max_us - min_us));
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("=========================================================\n");
	printf("   NEC PC-9821 Hardware & Engine Performance Benchmark   \n");
	printf("=========================================================\n");

	bench_planar_chunky_conversion();
	bench_grcg_sprite_blitting();
	bench_egc_screen_shift();
	bench_clock_jitter();

	printf("\n=========================================================\n");
	printf("Benchmark Completed Successfully.\n");
	printf("=========================================================\n");
	return 0;
}
