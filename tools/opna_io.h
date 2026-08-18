/*
 * opna_io.h - PC-9801-86 (YM2608 OPNA) access via the pc98snd kernel
 * module's /dev/pc98snd0 character device.
 *
 * All register I/O goes through the device node (the module does the busy
 * polling and port I/O); timer underflows arrive as IRQs via poll()/read()
 * instead of userspace polling.
 */

#ifndef PC98SND_OPNA_IO_H
#define PC98SND_OPNA_IO_H

#include <stdint.h>
#include "pc98snd_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bank selectors (kept for the existing call sites; the module maps these
 * to the flat 9-bit register address). */
#define OPNA_PORT_ADDR1	0x0188u	/* bank 0 (regs 0x000-0x0FF) */
#define OPNA_PORT_DATA1	0x018au
#define OPNA_PORT_ADDR2	0x018cu	/* bank 1 (regs 0x100-0x1FF) */
#define OPNA_PORT_DATA2	0x018eu

int  opna_init(void);		/* open /dev/pc98snd0 */
void opna_exit(void);
int  opna_get_fd(void);		/* device fd (for poll()ing alongside stdin etc.) */
void opna_reset(void);

void opna_write(uint16_t addr_port, uint16_t data_port,
		uint8_t reg, uint8_t value);
int  opna_write_batch(const struct pc98snd_batch *batch);
int  opna_read_port(uint16_t port, uint8_t *value);
int  opna_read_status(uint8_t *status);
int  opna_read_reg(uint16_t addr_port, uint16_t data_port,
		   uint8_t reg, uint8_t *value);

/* IRQ-driven timer: block until a Timer-A/B underflow, then consume. */
int  opna_poll_wait(int timeout_ms);
int  opna_consume_events(struct pc98snd_events *events);

int  opna_get_info(struct pc98snd_info *info);
int  opna_read_joystick(int port_num, uint8_t *val);

/* Channel-level helpers for the .M player and tone tests. */
void opna_fm_freq(uint8_t ch, unsigned fnum, uint8_t block);
void opna_fm_tl(uint8_t ch, uint8_t tl);
void opna_fm_slot_tl(uint8_t ch, uint8_t slot, uint8_t tl);
void opna_fm_keyon(uint8_t ch);
void opna_fm_keyoff(uint8_t ch);
void opna_fm_voice(uint8_t ch, const uint8_t voice[32]);
void opna_ssg_note(uint8_t ch, unsigned period, uint8_t volume, int noise);
void opna_ssg_amp(uint8_t ch, uint8_t volume);
void opna_ssg_silence(uint8_t ch);
void opna_ssg_tone(unsigned freq, unsigned volume, int on);
void opna_fm_tone(unsigned freq, unsigned volume, int on);

#ifdef __cplusplus
}
#endif

#endif /* PC98SND_OPNA_IO_H */
