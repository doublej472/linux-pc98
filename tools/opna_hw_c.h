/*
 * opna_hw_c.h - C interface for the pmdmini hardware OPNA backend.
 *
 * pmdmini addresses the YM2608 with a flat 9-bit register number
 * (0x000-0x1FF): bit 8 selects the register bank, the low byte is the
 * register number.  The PC-9801-86 exposes the two banks as two port
 * pairs (0x188/0x18A and 0x18C/0x18E); opna_hw_setreg() performs that
 * translation and delegates to tools/opna.c for the actual I/O.
 */

#ifndef PC98SND_OPNA_HW_C_H
#define PC98SND_OPNA_HW_C_H

#include <stdint.h>

int      opna_hw_init(void);		/* open /dev/pc98snd0, clear shadow */
void     opna_hw_exit(void);
void     opna_hw_reset(void);		/* clear the register shadow */

void     opna_hw_setreg(uint32_t addr, uint32_t data);
uint32_t opna_hw_getreg(uint32_t addr);
uint32_t opna_hw_readstatus(void);

#endif /* PC98SND_OPNA_HW_C_H */
