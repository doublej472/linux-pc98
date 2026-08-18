/*
 * opna.c - PC-9801-86 (YM2608 OPNA) access layer via the pc98snd kernel
 * module.  All register writes go to /dev/pc98snd0 (the module does the
 * busy-flag polling and port I/O in kernel space); timer underflows are
 * delivered as interrupts through poll()/read().
 *
 * Register addresses use the same flat 9-bit space as the pmdmini
 * sequencer: (bank << 8) | reg, where bank 0 is ports 0x188/0x18A and
 * bank 1 is 0x18C/0x18E.  The OPNA_PORT_ADDR* macros act as bank
 * selectors for the existing call sites.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "opna_io.h"

#define OPNA_MASTER_CLOCK	7987200u
#define OPNA_SSG_CLOCK		(OPNA_MASTER_CLOCK / 4u)
#define SSG_MAX_PERIOD		0xfffu

#define SSG_TONEA_PERIOD_LO	0x00u
#define SSG_TONEA_PERIOD_HI	0x01u
#define SSG_MIXER		0x07u
#define SSG_AMPLITUDE_A		0x08u

#define OPNA_KEYON		0x28u
#define OPNA_FNUM_LO		0xa0u
#define OPNA_FNUM_HI_BLOCK	0xa4u
#define OPNA_ALGO_FB		0xb0u

#define OPNA_SLOT_DT_MUL	0x30u
#define OPNA_SLOT_TL		0x40u
#define OPNA_SLOT_KS_AR		0x50u
#define OPNA_SLOT_AM_DR		0x60u
#define OPNA_SLOT_SR		0x70u
#define OPNA_SLOT_SL_RR		0x80u
#define OPNA_SLOT_SSG_EG	0x90u

#define SOUND_ID_PORT		0xa460u

static int devfd = -1;
static uint8_t ssg_mixer_shadow = 0x3f;

int opna_init(void)
{
	devfd = open("/dev/pc98snd0", O_RDWR);
	if (devfd < 0) {
		fprintf(stderr, "opna: cannot open /dev/pc98snd0: %s\n",
			strerror(errno));
		fprintf(stderr, "load the pc98snd module first\n");
		return -1;
	}
	return 0;
}

int opna_get_fd(void)
{
	return devfd;
}

void opna_exit(void)
{
	if (devfd >= 0) {
		close(devfd);
		devfd = -1;
	}
}

/* Map the bank-selector port to the flat 9-bit address. */
static uint16_t bank_addr(uint16_t addr_port, uint8_t reg)
{
	unsigned bank = (addr_port == OPNA_PORT_ADDR2) ? 1 : 0;

	return (uint16_t)((bank << 8) | reg);
}

void opna_write(uint16_t addr_port, uint16_t data_port,
		uint8_t reg, uint8_t value)
{
	struct pc98snd_reg r;

	(void)data_port;
	r.addr = bank_addr(addr_port, reg);
	r.data = value;
	(void)write(devfd, &r, sizeof(r));
}

int opna_write_batch(const struct pc98snd_batch *batch)
{
	if (ioctl(devfd, PC98SND_WRITE_REGS, batch) != 0)
		return -1;
	return 0;
}

int opna_read_status(uint8_t *status)
{
	if (ioctl(devfd, PC98SND_GET_STATUS, status) != 0)
		return -1;
	return 0;
}

int opna_read_reg(uint16_t addr_port, uint16_t data_port,
		  uint8_t reg, uint8_t *value)
{
	struct pc98snd_reg r;

	(void)data_port;
	r.addr = bank_addr(addr_port, reg);
	r.data = 0;
	if (ioctl(devfd, PC98SND_READ_REG, &r) != 0)
		return -1;
	*value = r.data;
	return 0;
}

int opna_read_port(uint16_t port, uint8_t *value)
{
	struct pc98snd_info info;

	if (port == SOUND_ID_PORT) {
		/* The board ID is the high nibble of 0xA460; the module read
		 * it at probe time. */
		if (opna_get_info(&info) != 0)
			return -1;
		*value = (uint8_t)(info.id << 4);
		return 0;
	}
	return -1;
}

int opna_get_info(struct pc98snd_info *info)
{
	if (ioctl(devfd, PC98SND_GET_INFO, info) != 0)
		return -1;
	return 0;
}

int opna_poll_wait(int timeout_ms)
{
	struct pollfd pfd;
	int rc;

	pfd.fd = devfd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	do {
		rc = poll(&pfd, 1, timeout_ms);
	} while (rc < 0 && errno == EINTR);

	return rc;	/* >0 IRQ, 0 timeout, -1 error */
}

int opna_consume_events(struct pc98snd_events *events)
{
	if (read(devfd, events, sizeof(*events)) != (ssize_t)sizeof(*events))
		return -1;
	return 0;
}

int opna_read_joystick(int port_num, uint8_t *val)
{
	uint8_t select_val = (port_num == 2) ? 0x00 : 0x80;

	/* Configure SSG register 0x07: bit 7 = 1 (Port B output), bit 6 = 0 (Port A input) */
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x07, (uint8_t)(ssg_mixer_shadow | 0x80));

	/* Write channel select to SSG Register 0x0F (Port B) */
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x0F, select_val);

	/* Read joystick state from SSG Register 0x0E (Port A) */
	return opna_read_reg(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x0E, val);
}

void opna_reset(void)
{
	unsigned ch, i;

	ssg_mixer_shadow = 0x3f;
	for (ch = 0; ch < 6; ch++) {
		static const uint16_t bases[6] = {
			0x30, 0x40, 0x50, 0x60, 0x70, 0x80
		};
		uint16_t addr, data;
		uint8_t c = (uint8_t)(ch % 3);
		unsigned g;

		opna_fm_keyoff((uint8_t)ch);
		addr = (uint16_t)(ch < 3 ? OPNA_PORT_ADDR1 : OPNA_PORT_ADDR2);
		data = (uint16_t)(ch < 3 ? OPNA_PORT_DATA1 : OPNA_PORT_DATA2);
		for (g = 0; g < 6; g++) {
			uint8_t value = 0x00;

			if (g == 1)
				value = 0x7f;	/* TL: muted */
			else if (g == 2)
				value = 0x1f;	/* KS/AR: fastest attack */
			else if (g == 5)
				value = 0xff;	/* SL/RR */
			for (i = 0; i < 4; i++)
				opna_write(addr, data,
					   (uint8_t)((ch < 3 ? bases[g] :
						    bases[g] + 0x100) +
						    i * 4 + c),
					   value);
		}
		opna_write(addr, data, (uint8_t)((ch < 3 ? 0xb0 : 0xb4) + c), 0x00);
		opna_write(addr, data, (uint8_t)((ch < 3 ? 0xb4 : 0xb8) + c), 0xc0);
		opna_write(addr, data, (uint8_t)((ch < 3 ? 0xa0 : 0xa8) + c), 0x00);
		opna_write(addr, data, (uint8_t)((ch < 3 ? 0xa4 : 0xac) + c), 0x00);
	}
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x07, 0x3f);
	for (i = 0; i < 7; i++)
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, (uint8_t)i, 0);
	for (i = 0; i < 3; i++)
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(0x08 + i), 0);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x22, 0x00);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, 0x2a, 0x00);
}

void opna_ssg_tone(unsigned freq, unsigned volume, int on)
{
	unsigned period;

	if (freq == 0)
		period = 0;
	else {
		period = OPNA_SSG_CLOCK / 16u / freq;
		if (period > SSG_MAX_PERIOD)
			period = SSG_MAX_PERIOD;
	}

	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, SSG_TONEA_PERIOD_LO,
		   (uint8_t)(period & 0xff));
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, SSG_TONEA_PERIOD_HI,
		   (uint8_t)((period >> 8) & 0x0f));
	ssg_mixer_shadow = 0x7e;
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, SSG_MIXER, ssg_mixer_shadow);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, SSG_AMPLITUDE_A,
		   (uint8_t)(on ? (volume & 0x0f) : 0));
}

void opna_fm_tone(unsigned freq, unsigned volume, int on)
{
	uint32_t fnum = 0;
	unsigned block, i, tl;

	if (freq == 0)
		freq = 1;
	for (block = 7; block > 0; block--) {
		fnum = (uint32_t)(((uint64_t)freq * 144u *
				   (1u << (20 - block))) /
				  OPNA_MASTER_CLOCK);
		if (fnum < 0x400u)
			break;
	}
	if (block == 0)
		block = 1;

	tl = on ? (unsigned)((15 - (volume & 0x0f)) * 2) : 127u;

	for (i = 0; i < 4; i++) {
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_DT_MUL + i * 4), 0x01);
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_TL + i * 4),
			   (uint8_t)(i == 0 ? tl : 127));
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_KS_AR + i * 4), 0x1f);
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_AM_DR + i * 4), 0x00);
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_SR + i * 4), 0x00);
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_SL_RR + i * 4), 0xff);
		opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1,
			   (uint8_t)(OPNA_SLOT_SSG_EG + i * 4), 0x00);
	}
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, OPNA_ALGO_FB, 0x07);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, OPNA_FNUM_LO,
		   (uint8_t)(fnum & 0xff));
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, OPNA_FNUM_HI_BLOCK,
		   (uint8_t)((block << 3) | ((fnum >> 8) & 0x07)));
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, OPNA_KEYON,
		   (uint8_t)(on ? 0xf0 : 0x00));
}

static void fm_reg_pair(uint8_t ch, uint16_t *addr, uint16_t *data)
{
	if (ch < 3) {
		*addr = OPNA_PORT_ADDR1;
		*data = OPNA_PORT_DATA1;
	} else {
		*addr = OPNA_PORT_ADDR2;
		*data = OPNA_PORT_DATA2;
	}
}

static void fm_freq_regs(uint8_t ch, uint16_t *fnum_lo, uint16_t *fnum_hi,
			 uint16_t *algo)
{
	uint8_t c = (uint8_t)(ch % 3);

	*fnum_lo = (uint16_t)((ch < 3 ? 0xa0 : 0xa8) + c);
	*fnum_hi = (uint16_t)((ch < 3 ? 0xa4 : 0xac) + c);
	*algo = (uint16_t)((ch < 3 ? 0xb0 : 0x1b0) + c);
}

void opna_fm_freq(uint8_t ch, unsigned fnum, uint8_t block)
{
	uint16_t lo, hi, algo, addr, data;

	fm_freq_regs(ch, &lo, &hi, &algo);
	fm_reg_pair(ch, &addr, &data);
	opna_write(addr, data, (uint8_t)hi,
		   (uint8_t)((block << 3) | ((fnum >> 8) & 0x07)));
	opna_write(addr, data, (uint8_t)lo,
		   (uint8_t)(fnum & 0xff));
}

void opna_fm_tl(uint8_t ch, uint8_t tl)
{
	uint16_t addr, data;
	uint8_t c = (uint8_t)(ch % 3);
	int i;

	fm_reg_pair(ch, &addr, &data);
	for (i = 0; i < 4; i++)
		opna_write(addr, data,
			   (uint8_t)((ch < 3 ? 0x40 : 0x140) + i * 4 + c), tl);
}

void opna_fm_slot_tl(uint8_t ch, uint8_t slot, uint8_t tl)
{
	uint16_t addr, data;
	uint8_t c = (uint8_t)(ch % 3);

	if (slot > 3)
		slot = 3;
	fm_reg_pair(ch, &addr, &data);
	opna_write(addr, data,
		   (uint8_t)((ch < 3 ? 0x40 : 0x140) + slot * 4 + c), tl);
}

void opna_fm_keyon(uint8_t ch)
{
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, OPNA_KEYON,
		   (uint8_t)(0xf0 | (ch % 3) | (ch >= 3 ? 0x04 : 0x00)));
}

void opna_fm_keyoff(uint8_t ch)
{
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, OPNA_KEYON,
		   (uint8_t)((ch % 3) | (ch >= 3 ? 0x04 : 0x00)));
}

void opna_fm_voice(uint8_t ch, const uint8_t voice[32])
{
	static const uint16_t bases[6] = {
		0x30, 0x40, 0x50, 0x60, 0x70, 0x80
	};
	uint16_t addr, data, algo;
	uint8_t c = (uint8_t)(ch % 3);
	unsigned i, group;

	fm_reg_pair(ch, &addr, &data);
	fm_freq_regs(ch, &algo, &algo, &algo);
	for (group = 0; group < 6; group++) {
		for (i = 0; i < 4; i++)
			opna_write(addr, data,
				   (uint8_t)((ch < 3 ? bases[group] :
						    bases[group] + 0x100) +
					    i * 4 + c),
				   voice[group * 4 + i]);
	}
	opna_write(addr, data, (uint8_t)algo, voice[24]);
}

void opna_ssg_note(uint8_t ch, unsigned period, uint8_t volume, int noise)
{
	static const uint8_t period_lo[3] = { 0x00, 0x02, 0x04 };
	static const uint8_t period_hi[3] = { 0x01, 0x03, 0x05 };
	static const uint8_t amp[3] = { 0x08, 0x09, 0x0a };
	static const uint8_t tone_bit[3] = { 0x01, 0x02, 0x04 };
	static const uint8_t noise_bit[3] = { 0x08, 0x10, 0x20 };

	if (ch > 2)
		ch = 2;
	period &= 0xfff;
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, period_lo[ch],
		   (uint8_t)(period & 0xff));
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, period_hi[ch],
		   (uint8_t)((period >> 8) & 0x0f));
	ssg_mixer_shadow &= (uint8_t)~(tone_bit[ch] | noise_bit[ch]);
	ssg_mixer_shadow |= (uint8_t)(noise ? tone_bit[ch] : noise_bit[ch]);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, SSG_MIXER, ssg_mixer_shadow);
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, amp[ch], volume & 0x0f);
}

void opna_ssg_amp(uint8_t ch, uint8_t volume)
{
	static const uint8_t amp[3] = { 0x08, 0x09, 0x0a };

	if (ch > 2)
		ch = 2;
	opna_write(OPNA_PORT_ADDR1, OPNA_PORT_DATA1, amp[ch], volume & 0x0f);
}

void opna_ssg_silence(uint8_t ch)
{
	opna_ssg_amp(ch, 0);
}
