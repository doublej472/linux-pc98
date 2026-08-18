/*
 * pc98snd - unified PC-98 C-Bus sound card tool.
 *
 * Subcommands:
 *   detect                 identify the installed sound card (PC-9801-86
 *                          and compatibles are sound-system ID 4/5)
 *   tone [FREQ] [MS]       play an SSG tone-A square wave (default 440 Hz
 *                          for 750 ms) to prove the card produces sound
 *
 * The tool runs entirely in userspace as root, talking to the card
 * through the pc98snd kernel module (/dev/pc98snd0).  Timer underflows
 * arrive as interrupts; the play loop blocks in poll() and dispatches the
 * sequencer's Timer-A/B handlers on each underflow.
 *
 * The sound-system ID mapping follows the Linux/98 sound_pc9800.h
 * definitions (PC9800_SOUND_ID): the high nibble of port 0xA460
 * identifies the board.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "opna_io.h"
#include "pmd_player.h"

#define SOUND_ID_PORT	0xa460u

static void stdin_restore(void);

/* Ctrl-C / kill while playing: silence the card immediately so no notes
 * keep sounding and no timers keep running, and restore terminal mode. */
static void on_fatal_signal(int sig)
{
	(void)sig;
	stdin_restore();
	opna_reset();
	_exit(128 + sig);
}

struct sound_card {
	unsigned id;
	const char *name;
	uint16_t fm_base;
};

static const struct sound_card cards[] = {
	{ 0x0, "PC-98DO+ internal sound", 0 },
	{ 0x1, "PC-98GS internal sound", 0 },
	{ 0x2, "PC-9801-73 (base 0x18x)", OPNA_PORT_ADDR1 },
	{ 0x3, "PC-9801-73/76 (base 0x28x)", 0x0288u },
	{ 0x4, "PC-9801-86 and compatible (base 0x18x)", OPNA_PORT_ADDR1 },
	{ 0x5, "PC-9801-86 (base 0x28x)", 0x0288u },
	{ 0x6, "PC-9821Nf/Np internal sound", 0 },
	{ 0x7, "X-Mate internal and compatible", 0 },
	{ 0x8, "PC-9801-118 and compatible", 0 },
};

static void usage(FILE *out)
{
	fprintf(out,
		"usage: pc98snd COMMAND [args]\n"
		"\n"
		"commands:\n"
		"  detect               identify the installed sound card\n"
		"  reset                reset the sound card to a known silent state\n"
		"  tone [--fm] [FREQ] [MS]  test tone, SSG by default or FM\n"
		"  play FILE.M [SECONDS]               play a PMD .M music file\n"
		"      (SECONDS=0 runs until Ctrl-C)\n"
		"  play --interactive FILE.M          play + SFX on keypress (q=quit)\n"
		"  fmvoice FILE.M VOICE NOTE          test one FM voice\n"
		"  ssgnote NOTE [VOL]                 test one SSG note\n"
		"  joystick [PORT] [-w|--watch]       read joystick/gamepad state (port 1/2)\n");
}

static int cmd_detect(void)
{
	uint8_t idreg, status;
	unsigned id, i;
	const struct sound_card *found = NULL;

	if (opna_read_port(SOUND_ID_PORT, &idreg) != 0) {
		fprintf(stderr, "pc98snd: cannot read port 0x%04X: %s\n",
			SOUND_ID_PORT, strerror(errno));
		return 2;
	}
	id = (idreg >> 4) & 0x0f;

	for (i = 0; i < sizeof(cards) / sizeof(cards[0]); i++)
		if (cards[i].id == id) {
			found = &cards[i];
			break;
		}

	if (found == NULL || id == 0x0f) {
		printf("PC-98 sound system ID 0x%04X = 0x%02X (ID %u): "
		       "no sound card detected\n",
		       SOUND_ID_PORT, idreg, id);
		return 1;
	}

	printf("PC-98 sound system ID 0x%04X = 0x%02X (ID %u): %s\n",
	       SOUND_ID_PORT, idreg, id, found->name);

	if (found->fm_base != 0) {
		if (opna_read_status(&status) == 0)
			printf("  FM/SSG status register 0x%04X reads 0x%02X\n",
			       found->fm_base, status);
		else
			printf("  (could not probe status register 0x%04X)\n",
			       found->fm_base);
	}
	return 0;
}

static void msleep(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static int cmd_tone(int use_fm, unsigned freq, unsigned ms)
{
	uint8_t status;

	if (opna_read_status(&status) != 0) {
		fprintf(stderr, "pc98snd: cannot read the OPNA status: %s\n",
			strerror(errno));
		return 2;
	}
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x27, 0x00);

	printf("%s tone: %u Hz for %u ms\n", use_fm ? "FM" : "SSG",
	       freq, ms);
	if (use_fm) {
		opna_fm_tone(freq, 12, 1);
		msleep(ms);
		opna_fm_tone(freq, 0, 0);
	} else {
		opna_ssg_tone(freq, 12, 1);
		msleep(ms);
		opna_ssg_tone(freq, 0, 0);
	}
	return 0;
}

/* Play a single FM note with a voice loaded from a .M file's tone
 * data (targeted instrument verification on the real chip). */
static int cmd_tone_voice(const char *path, unsigned voice_id, unsigned note)
{
	static const uint16_t fnum_data[12] = {
		0x026a, 0x028f, 0x02b6, 0x02df, 0x030b, 0x0339,
		0x036a, 0x039e, 0x03d5, 0x0410, 0x044e, 0x048f,
	};
	FILE *fp;
	uint8_t *data;
	long size;
	const uint8_t *tone;
	size_t toff, p;
	unsigned idx, block;

	fp = fopen(path, "rb");
	if (fp == NULL) {
		perror(path);
		return 2;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = malloc((size_t)size);
	if (data == NULL) {
		fclose(fp);
		perror("malloc");
		return 2;
	}
	if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(data);
		perror("read");
		return 2;
	}
	fclose(fp);
	if (size < 0x1b) {
		fprintf(stderr, "pc98snd: file too small\n");
		free(data);
		return 2;
	}
	toff = (size_t)(data[0x19] | (data[0x1a] << 8)) + 1;
	if (toff + 26 > (size_t)size) {
		fprintf(stderr, "pc98snd: tone table offset %zu out of range\n",
			toff);
		free(data);
		return 2;
	}
	tone = NULL;
	for (p = 0; p + 26 <= (size_t)size - toff; p += 26) {
		if (data[toff + p] == (uint8_t)voice_id) {
			tone = data + toff + p;
			break;
		}
	}
	if (tone == NULL) {
		fprintf(stderr, "pc98snd: voice %u not found in %s\n",
			voice_id, path);
		free(data);
		return 2;
	}
	idx = (note & 0x0f) % 12;
	block = (note >> 4) & 7;
	opna_reset();
	opna_fm_voice(0, tone + 1);	/* 24 op bytes + alg/fb */
	opna_fm_freq(0, fnum_data[idx], (uint8_t)block);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0xb4, 0xc0);
	opna_fm_keyon(0);
	printf("FM voice %u note 0x%02x (fnum %04x blk %u) for 2 s\n",
	       voice_id, note, fnum_data[idx], block);
	msleep(2000);
	opna_fm_keyoff(0);
	opna_reset();
	free(data);
	return 0;
}

/* Play a single SSG note (targeted pitch verification). */
static int cmd_tone_ssgnote(unsigned note, unsigned volume)
{
	static const uint16_t psg_tune_data[12] = {
		0x0ee8, 0x0e12, 0x0d48, 0x0c89, 0x0bd5, 0x0b2b,
		0x0a8a, 0x09f3, 0x0964, 0x08dd, 0x085e, 0x07e6,
	};
	unsigned idx = (note & 0x0f) % 12;
	unsigned block = (note >> 4) & 7;
	unsigned period = psg_tune_data[idx] >> block;

	if ((psg_tune_data[idx] >> block) & 1)
		period++;
	if (period == 0)
		period = 1;
	opna_reset();
	opna_ssg_note(0, period, (uint8_t)(volume & 0x0f), 0);
	printf("SSG note 0x%02x (period %u, %.1f Hz) for 2 s\n", note,
	       period, 7987200.0 / (64.0 * period));
	msleep(2000);
	opna_ssg_amp(0, 0);
	opna_reset();
	return 0;
}

static uint64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull +
	       (uint64_t)ts.tv_nsec / 1000ull;
}

/* Opt-in SCHED_FIFO for the music pump: cuts the poll()->wakeup latency on
 * the single-core machine so ticks dispatch promptly.  Set PC98SND_RT=1. */
static void set_realtime(void)
{
	struct sched_param sp;

	if (getenv("PC98SND_RT") == NULL)
		return;
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
	if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
		fprintf(stderr, "pc98snd: SCHED_FIFO unavailable: %s\n",
			strerror(errno));
}

static int load_music(const char *path, uint8_t **out, size_t *out_size)
{
	FILE *fp;
	uint8_t *data;
	long size;

	fp = fopen(path, "rb");
	if (fp == NULL) {
		fprintf(stderr, "pc98snd: cannot open %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
	    fseek(fp, 0, SEEK_SET) != 0) {
		fprintf(stderr, "pc98snd: cannot size %s\n", path);
		fclose(fp);
		return -1;
	}
	data = malloc((size_t)size);
	if (data == NULL) {
		fprintf(stderr, "pc98snd: out of memory\n");
		fclose(fp);
		return -1;
	}
	if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
		fprintf(stderr, "pc98snd: short read on %s\n", path);
		free(data);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out = data;
	*out_size = (size_t)size;
	return 0;
}

static int start_music(const char *path)
{
	uint8_t *data;
	size_t size;

	if (pmd_player_init(".") != 0) {
		fprintf(stderr, "pc98snd: cannot initialise player\n");
		return -1;
	}
	if (load_music(path, &data, &size) != 0)
		return -1;
	if (pmd_player_load(data, size) != 0) {
		fprintf(stderr, "pc98snd: bad music data: %s\n", path);
		free(data);
		return -1;
	}
	free(data);
	if (pmd_player_start() != 0) {
		fprintf(stderr, "pc98snd: cannot start playback\n");
		return -1;
	}
	return 0;
}

static int cmd_play(const char *path, unsigned seconds)
{
	uint64_t start;

	set_realtime();
	if (start_music(path) != 0)
		return 2;

	printf("playing %s\n", path);

	/* The pc98snd module raises an IRQ on every Timer-A/B underflow.
	 * Block in poll() for the next one, then dispatch the matching
	 * handler once per underflow.  No userspace polling, no TSC, no
	 * /dev/port. */
	start = mono_us();
	for (;;) {
		int timeout = -1;

		if (seconds != 0) {
			uint64_t elapsed = mono_us() - start;
			uint64_t total = (uint64_t)seconds * 1000000ull;

			if (elapsed >= total)
				break;
			timeout = (int)((total - elapsed) / 1000ull);
			if (timeout < 1)
				timeout = 1;
		}

		if (pmd_player_wait(timeout) <= 0)	/* timeout or error */
			break;
		pmd_player_pump();
	}

	pmd_player_stop();
	printf("stopped\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Interactive / game demo                                            */
/* ------------------------------------------------------------------ */

static struct termios saved_tio;
static int tio_saved;
static int sfx_on;
static uint64_t sfx_off_at;

static void stdin_raw(void)
{
	struct termios tio;

	/* Raw mode only applies to a terminal; over ssh/stdin-pipe the
	 * tcgetattr call fails and we skip it.  O_NONBLOCK must always be set
	 * so the key-read loop can never block the music pump. */
	if (tcgetattr(0, &tio) == 0) {
		saved_tio = tio;
		tio_saved = 1;
		tio.c_lflag &= (tcflag_t)~(ICANON | ECHO);
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &tio);
	}
	fcntl(0, F_SETFL, O_NONBLOCK);
}

static void stdin_restore(void)
{
	if (tio_saved)
		tcsetattr(0, TCSANOW, &saved_tio);
}

/* Trigger a short FM "blip" on FM channel 6 (music part index 5), which is
 * reserved with maskon()/maskoff() so the music never clobbers it mid-SFX.
 * Key-off is handled on a deadline by the pump loop, not by sleeping. */
static void sfx_trigger(void)
{
	static const uint8_t voice[25] = {
		0x01, 0x00, 0x00, 0x01,	/* DT/MUL */
		0x19, 0x7f, 0x7f, 0x19,	/* TL */
		0x1f, 0x00, 0x00, 0x1f,	/* KS/AR */
		0x00, 0x00, 0x00, 0x00,	/* AM/DR */
		0x00, 0x00, 0x00, 0x00,	/* SR */
		0xff, 0x00, 0x00, 0xff,	/* SL/RR */
		0x07,					/* algo/fb */
	};

	if (sfx_on)
		return;
	pmd_player_mask(5, 1);
	opna_fm_voice(5, voice);
	opna_fm_freq(5, 0x0353u, 5);
	opna_fm_keyon(5);
	sfx_on = 1;
	sfx_off_at = mono_us() + 120000ull;
}

static void sfx_maybe_off(void)
{
	if (sfx_on && mono_us() >= sfx_off_at) {
		opna_fm_keyoff(5);
		pmd_player_mask(5, 0);
		sfx_on = 0;
	}
}

static int cmd_play_interactive(const char *path)
{
	struct pollfd pfds[2];

	set_realtime();
	if (start_music(path) != 0)
		return 2;

	printf("playing %s (interactive: any key = SFX, q = quit)\n", path);
	fflush(stdout);
	stdin_raw();

	pfds[0].fd = opna_get_fd();
	pfds[0].events = POLLIN;
	pfds[1].fd = 0;
	pfds[1].events = POLLIN;

	for (;;) {
		int rc;
		int timeout = 50;	/* wake periodically for SFX key-off */

		if (sfx_on) {
			uint64_t rem = sfx_off_at - mono_us();

			if ((int64_t)rem < timeout)
				timeout = (int)(rem / 1000ull);
			if (timeout < 1)
				timeout = 1;
		}

		rc = poll(pfds, 2, timeout);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			break;

		if (pfds[0].revents & POLLIN)
			pmd_player_pump();
		sfx_maybe_off();

		if (pfds[1].revents & POLLIN) {
			char c;

			while (read(0, &c, 1) == 1) {
				if (c == 'q' || c == 'Q')
					goto out;
				sfx_trigger();
				printf("SFX! loop=%d pos=%d\n",
				       pmd_player_get_loop_count(),
				       pmd_player_get_pos());
				fflush(stdout);
			}
		}
	}

out:
	pmd_player_stop();
	stdin_restore();
	printf("stopped\n");
	return 0;
}

/* Read DE-9 joystick/gamepad state from YM2608 SSG port A/B. */
static int cmd_joystick(int port_num, int watch)
{
	uint8_t state = 0xff;

	if (opna_read_joystick(port_num, &state) != 0) {
		fprintf(stderr, "pc98snd: failed to read joystick port %d\n", port_num);
		return 2;
	}

	if (!watch) {
		printf("PC-9801-86 Joystick Port %d (raw=0x%02X):\n", port_num, state);
		printf("  Up       : %s\n", (state & 0x01) ? "open" : "PRESSED");
		printf("  Down     : %s\n", (state & 0x02) ? "open" : "PRESSED");
		printf("  Left     : %s\n", (state & 0x04) ? "open" : "PRESSED");
		printf("  Right    : %s\n", (state & 0x08) ? "open" : "PRESSED");
		printf("  Button A : %s\n", (state & 0x10) ? "open" : "PRESSED");
		printf("  Button B : %s\n", (state & 0x20) ? "open" : "PRESSED");
		return 0;
	}

	printf("Monitoring Joystick Port %d (Ctrl-C to stop)...\n", port_num);
	uint8_t last = 0;
	for (;;) {
		if (opna_read_joystick(port_num, &state) == 0) {
			if (state != last) {
				printf("State: 0x%02X [ %s%s%s%s%s%s]\n",
				       state,
				       !(state & 0x01) ? "UP " : "",
				       !(state & 0x02) ? "DOWN " : "",
				       !(state & 0x04) ? "LEFT " : "",
				       !(state & 0x08) ? "RIGHT " : "",
				       !(state & 0x10) ? "BTN_A " : "",
				       !(state & 0x20) ? "BTN_B " : "");
				fflush(stdout);
				last = state;
			}
		}
		usleep(20000); /* 50 Hz poll */
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_fatal_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	const char *command;

	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	command = argv[1];

	if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
		usage(stdout);
		return 0;
	}

	if (opna_init() != 0)
		return 2;

	if (strcmp(command, "detect") == 0)
		return cmd_detect();

	if (strcmp(command, "reset") == 0) {
		uint8_t status;

		opna_reset();
		if (opna_read_status(&status) == 0)
			printf("sound card reset; OPNA status = 0x%02X\n", status);
		else
			printf("sound card reset\n");
		return 0;
	}

	if (strcmp(command, "tone") == 0) {
		int use_fm = 0;
		unsigned freq = 440, ms = 750;
		char *end;
		int argi = 2;

		if (argc > argi && strcmp(argv[argi], "--fm") == 0) {
			use_fm = 1;
			argi++;
		}
		if (argc > argi) {
			freq = (unsigned)strtoul(argv[argi], &end, 0);
			if (*end != '\0' || freq == 0 || freq > 20000) {
				fprintf(stderr, "pc98snd: bad frequency: %s\n",
					argv[argi]);
				return 2;
			}
		}
		if (argc > argi + 1) {
			ms = (unsigned)strtoul(argv[argi + 1], &end, 0);
			if (*end != '\0' || ms > 60000) {
				fprintf(stderr, "pc98snd: bad duration: %s\n",
					argv[argi + 1]);
				return 2;
			}
		}
		return cmd_tone(use_fm, freq, ms);
	}

	if (strcmp(command, "fmvoice") == 0 && argc >= 5) {
		unsigned vid = (unsigned)strtoul(argv[3], NULL, 0);
		unsigned note = (unsigned)strtoul(argv[4], NULL, 0);

		return cmd_tone_voice(argv[2], vid, note);
	}
	if (strcmp(command, "ssgnote") == 0 && argc >= 4) {
		unsigned note = (unsigned)strtoul(argv[2], NULL, 0);
		unsigned vol = argc > 3 ?
			(unsigned)strtoul(argv[3], NULL, 0) : 12;

		return cmd_tone_ssgnote(note, vol);
	}
	if (strcmp(command, "joystick") == 0 || strcmp(command, "gamepad") == 0 || strcmp(command, "joy") == 0) {
		int port_num = 1;
		int watch = 0;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--watch") == 0) {
				watch = 1;
			} else if (strcmp(argv[i], "1") == 0 || strcmp(argv[i], "2") == 0) {
				port_num = atoi(argv[i]);
			}
		}
		return cmd_joystick(port_num, watch);
	}
	if (strcmp(command, "play") == 0) {
		int interactive = 0;
		int argi = 2;
		unsigned seconds = 0;

		if (argc < 3) {
			fprintf(stderr, "pc98snd: play needs a FILE.M\n");
			return 2;
		}
		if (strcmp(argv[argi], "--interactive") == 0) {
			interactive = 1;
			argi++;
		}
		if (argi >= argc) {
			fprintf(stderr, "pc98snd: play needs a FILE.M\n");
			return 2;
		}
		if (argc > argi + 1) {
			char *end;

			seconds = (unsigned)strtoul(argv[argi + 1], &end, 0);
			if (*end != '\0') {
				fprintf(stderr, "pc98snd: bad duration: %s\n",
					argv[argi + 1]);
				return 2;
			}
		}
		if (interactive)
			return cmd_play_interactive(argv[argi]);
		return cmd_play(argv[argi], seconds);
	}

	fprintf(stderr, "pc98snd: unknown command: %s\n", command);
	usage(stderr);
	return 2;
}
