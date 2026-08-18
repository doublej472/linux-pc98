/*
 * pmd_player.h - C interface to the pmdmini (PMDWin) sequencer running in
 * real time against the PC-9801-86 hardware.
 *
 * The sequencer is clocked by the chip's own free-running Timer-B: the
 * caller blocks on the pc98snd device (poll()) and invokes
 * pmd_player_timer_a() / pmd_player_timer_b() once per underflow, exactly
 * like the original DOS TSR.  No software timing loop is involved.
 *
 * Two driving styles are supported:
 *
 *   - Music player (faithful): pmd_player_wait() + pmd_player_pump() in a
 *     loop.  pump() dispatches every pending underflow (catch-up), so a
 *     scheduling hiccup never drops a note.
 *
 *   - Game / interactive: call pmd_player_pump() from the frame loop to
 *     advance the music, and use the control API (fadeout, mask, volume,
 *     loop/pos query) plus opna_write_batch() for atomic SFX bursts.  All
 *     calls are safe to make from one thread while music plays.
 */

#ifndef PC98SND_PMD_PLAYER_H
#define PC98SND_PMD_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  pmd_player_init(const char *pcmdir);	/* one PMDWIN + hardware OPNA */
int  pmd_player_load(const uint8_t *data, size_t size);
int  pmd_player_start(void);
void pmd_player_stop(void);
void pmd_player_timer_a(void);			/* Timer-A underflow handler */
void pmd_player_timer_b(void);			/* Timer-B underflow handler */

/* Tick pump / wait. */
int  pmd_player_wait(int timeout_ms);		/* block for the next underflow */
int  pmd_player_pump(void);			/* dispatch all pending underflows */

/* Control (valid while a song is loaded / playing). */
void pmd_player_fadeout(int speed);		/* start a fadeout (PMD d2) */
int  pmd_player_get_loop_count(void);		/* loop count, -1 while stopped */
int  pmd_player_get_pos(void);			/* position in music counts */
int  pmd_player_is_playing(void);		/* nonzero while music plays */
int  pmd_player_mask(int ch, int on);		/* mute/unmute a music part */
void pmd_player_set_fm_voldown(int v);		/* master volume trim (FM) */
void pmd_player_set_ssg_voldown(int v);
void pmd_player_set_rhythm_voldown(int v);
void pmd_player_set_adpcm_voldown(int v);

#ifdef __cplusplus
}
#endif

#endif /* PC98SND_PMD_PLAYER_H */
