/*
 * pc98snd_ioctl.h - shared ABI between the pc98snd kernel module and
 * userspace (tools/pc98snd).
 *
 * The module exposes a character device (/dev/pc98snd0) that is a thin
 * wrapper over the raw YM2608 / PC-98 hardware:
 *   - write(fd, &reg, sizeof(reg))  write one OPNA register (busy-wait + I/O)
 *   - ioctl(PC98SND_READ_REG)       read one register (shadow / ADPCM data)
 *   - ioctl(PC98SND_GET_STATUS)     raw status-port read (detect/debug)
 *   - ioctl(PC98SND_WRITE_REGS)     write several registers atomically
 *   - ioctl(PC98SND_GET_INFO)       board ID / I/O base / IRQ
 *   - poll(fd, ...)                 block until a timer-A/B underflow IRQ
 *   - read(fd, &ev, sizeof(ev))     consume pending timer-A/B underflow counts
 *
 * All PMD / music-sequencer policy lives in userspace; the module only
 * performs register I/O and delivers the chip's raw timer interrupts.
 *
 * The register address is the same flat 9-bit space the pmdmini sequencer
 * uses: (bank << 8) | reg, where bank 0 is ports 0x188/0x18A and bank 1 is
 * 0x18C/0x18E.
 */

#ifndef PC98SND_IOCTL_H
#define PC98SND_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct pc98snd_reg {
	__u16 addr;	/* 9-bit register address (bank << 8) | reg */
	__u8  data;
} __attribute__((packed));

struct pc98snd_info {
	__u8  id;		/* PC-98 sound-system board ID (high nibble of 0xA460) */
	__u8  _pad;
	__u16 base;		/* bank-1 I/O base (0x188/0x288) */
	__u32 irq;		/* board IRQ */
};

/* Returned by read(): how many Timer-A/B underflows fired since the last
 * read.  Counts (rather than boolean flags) preserve ticks that arrive
 * while the reader is descheduled. */
struct pc98snd_events {
	__u32 timer_a;		/* Timer-A underflow count */
	__u32 timer_b;		/* Timer-B underflow count */
};

/* Batched register write (PC98SND_WRITE_REGS): the whole array is applied
 * with IRQs disabled, so a multi-register sequence (a voice change, an SFX
 * burst) is atomic w.r.t. the IRQ handler and costs one syscall. */
#define PC98SND_MAX_BATCH	64
struct pc98snd_batch {
	__u32 count;			/* entries used (1..PC98SND_MAX_BATCH) */
	struct pc98snd_reg regs[PC98SND_MAX_BATCH];
};

#define PC98SND_MAGIC		'P'
#define PC98SND_READ_REG	_IOWR(PC98SND_MAGIC, 1, struct pc98snd_reg)
#define PC98SND_GET_STATUS	_IOR(PC98SND_MAGIC, 2, __u8)
#define PC98SND_GET_INFO	_IOR(PC98SND_MAGIC, 4, struct pc98snd_info)
#define PC98SND_WRITE_REGS	_IOW(PC98SND_MAGIC, 5, struct pc98snd_batch)

/* Legacy timer-underflow flag bits (status register bits 0/1). */
#define PC98SND_FLAG_TIMERA	0x01
#define PC98SND_FLAG_TIMERB	0x02

#endif /* PC98SND_IOCTL_H */
