//=============================================================================
//	pmd_player.cpp - real-time hardware driver for the pmdmini sequencer.
//
//	Creates a single PMDWIN instance (the offline renderer's second
//	instance is only needed for song-length calculation, which the real
//	time player does not use), loads the .M from memory, and exposes the
//	Timer-A/B underflow handlers plus a small game-facing control API.
//
//	SPDX-License-Identifier: GPL-2.0-or-later
//=============================================================================

#include "pmd_player.h"

#include "pmdwincore.h"
#include "opna_io.h"

#include <cstring>

/* The single process-wide PMDWIN, created by pmdwininit() in pmdwin.cpp. */
extern PMDWIN *pmdwin;

static bool s_inited;
static bool s_playing;

int pmd_player_init(const char *pcmdir)
{
	if (s_inited)
		return 0;

	if (pmdwin == NULL)
		pmdwin = new PMDWIN;

	if (!pmdwin->init((TCHAR *)pcmdir))
		return -1;

	/* Rhythm + SSG together (PMDWin default is SSG-only). */
	pmdwin->setrhythmwithssgeffect(true);

	s_inited = true;
	return 0;
}

int pmd_player_load(const uint8_t *data, size_t size)
{
	if (!s_inited)
		return -1;
	if (pmdwin->music_load2(const_cast<uint8_t *>(data), (int32_t)size) != 0)
		return -1;
	return 0;
}

int pmd_player_start(void)
{
	struct pc98snd_events ev;

	if (!s_inited)
		return -1;

	/* Discard any underflows that accumulated since the last stop (the
	 * timers free-run while stopped), so a stop/restart never dispatches
	 * stale ticks as if they belonged to the new song. */
	(void)opna_consume_events(&ev);

	pmdwin->music_start();
	s_playing = true;
	return 0;
}

void pmd_player_stop(void)
{
	if (s_inited)
		pmdwin->music_stop();
	s_playing = false;
}

void pmd_player_timer_a(void)
{
	if (s_inited)
		pmdwin->TimerA_main();
}

void pmd_player_timer_b(void)
{
	if (s_inited)
		pmdwin->TimerB_main();
}

/* ------------------------------------------------------------------ */
/* Tick pump / wait                                                   */
/* ------------------------------------------------------------------ */

int pmd_player_wait(int timeout_ms)
{
	return opna_poll_wait(timeout_ms);
}

int pmd_player_pump(void)
{
	struct pc98snd_events ev;
	int total;

	if (!s_inited)
		return 0;
	if (opna_consume_events(&ev) != 0)
		return -1;

	total = (int)(ev.timer_a + ev.timer_b);

	/* Dispatch in the PMD reference order (Timer-A before Timer-B). */
	while (ev.timer_a--)
		pmdwin->TimerA_main();
	while (ev.timer_b--)
		pmdwin->TimerB_main();

	return total;
}

/* ------------------------------------------------------------------ */
/* Control API                                                        */
/* ------------------------------------------------------------------ */

void pmd_player_fadeout(int speed)
{
	if (s_inited)
		pmdwin->fadeout(speed);
}

int pmd_player_get_loop_count(void)
{
	if (!s_inited || !s_playing)
		return -1;
	return pmdwin->getloopcount();
}

int pmd_player_get_pos(void)
{
	if (!s_inited)
		return -1;
	return pmdwin->getpos2();
}

int pmd_player_is_playing(void)
{
	return s_playing ? 1 : 0;
}

int pmd_player_mask(int ch, int on)
{
	if (!s_inited)
		return -1;
	if (on)
		return pmdwin->maskon(ch);
	return pmdwin->maskoff(ch);
}

void pmd_player_set_fm_voldown(int v)      { if (s_inited) pmdwin->setfmvoldown(v); }
void pmd_player_set_ssg_voldown(int v)     { if (s_inited) pmdwin->setssgvoldown(v); }
void pmd_player_set_rhythm_voldown(int v)  { if (s_inited) pmdwin->setrhythmvoldown(v); }
void pmd_player_set_adpcm_voldown(int v)   { if (s_inited) pmdwin->setadpcmvoldown(v); }
