/*
 * opna_hw.c - pmdmini register-address space -> PC-9801-86 port I/O.
 *
 * The real YM2608 cannot read back most registers (the status register
 * and the ADPCM data port are the exceptions), while PMD uses a
 * read-modify-write on the SSG mixer (0x07) and reads the ADPCM data
 * port back in pcmread().  We therefore keep a software shadow of the
 * whole 9-bit register space and service reads from it, except for the
 * ADPCM data port (0x108) which is read from the chip.
 *
 * Register 0x29's low two bits are the Timer-A/B IRQ enables; bit 7 is
 * the FM 6-channel mode.  The pc98snd module installs the IRQ handler,
 * so the sequencer's 0x29=0x83 write is passed through unmasked (the
 * module delivers the timer underflows via poll()).
 */

#include <stdlib.h>
#include <string.h>

#include "opna_io.h"
#include "opna_hw_c.h"

static uint8_t reg_shadow[0x200];

int opna_hw_init(void)
{
	if (opna_init() != 0)
		return -1;
	opna_hw_reset();
	return 0;
}

void opna_hw_exit(void)
{
	opna_exit();
}

/* Clear the register shadow only.  The chip itself is fully re-programmed
 * by the sequencer's mstart(), so a hardware reset here would only risk
 * desynchronising the shadow (opna_reset() writes the chip directly). */
void opna_hw_reset(void)
{
	memset(reg_shadow, 0, sizeof(reg_shadow));
}

void opna_hw_setreg(uint32_t addr, uint32_t data)
{
	unsigned a = (unsigned)(addr & 0x1ffu);
	uint8_t reg = (uint8_t)(a & 0xffu);
	uint8_t val = (uint8_t)data;
	int bank = (int)((a >> 8) & 1u);

	reg_shadow[a] = val;

	opna_write(bank ? OPNA_PORT_ADDR2 : OPNA_PORT_ADDR1,
		   bank ? OPNA_PORT_DATA2 : OPNA_PORT_DATA1,
		   reg, val);
}

uint32_t opna_hw_getreg(uint32_t addr)
{
	unsigned a = (unsigned)(addr & 0x1ffu);

	/* ADPCM data port: readable on the real chip (auto-increment). */
	if (a == 0x108) {
		uint8_t v = 0;

		if (opna_read_reg(OPNA_PORT_ADDR2, OPNA_PORT_DATA2,
				  0x08, &v) == 0) {
			reg_shadow[a] = v;
			return v;
		}
	}
	return reg_shadow[a];
}

uint32_t opna_hw_readstatus(void)
{
	uint8_t st = 0;

	if (opna_read_status(&st) != 0)
		return 0;
	return st;
}
