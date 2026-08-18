// SPDX-License-Identifier: GPL-2.0
/*
 * pc98snd - PC-9801-86 / PC-9801-73 (YM2608 OPNA) sound board driver.
 *
 * Exposes a character device (/dev/pc98snd0) that lets userspace drive the
 * YM2608 directly and receive the Timer-A/B underflow interrupts without
 * polling:
 *
 *   write(fd, &{u16 addr, u8 data}, 3)    register write
 *   ioctl(PC98SND_READ_REG, &reg)         register read (shadow / ADPCM)
 *   ioctl(PC98SND_GET_STATUS, &st)        raw status-port read
 *   ioctl(PC98SND_WRITE_REGS, &batch)     atomic multi-register write
 *   ioctl(PC98SND_GET_INFO, &info)        board ID / I/O base / IRQ
 *   poll(fd, ...)                         block until a timer IRQ fires
 *   read(fd, &events, sizeof(events))     consume pending timer-A/B counts
 *
 * This is a thin wrapper over the raw hardware: it performs register I/O and
 * delivers the chip's timer interrupts; all PMD / sequencer policy lives in
 * userspace.  The IRQ handler clears the underflow flag for the timer(s)
 * that fired, re-arms the one-shot timer, counts the event and wakes the
 * poll() waiter.  Delivering counts (not booleans) means a scheduling
 * hiccup cannot silently drop music ticks.
 *
 * The design assumes one userspace client at a time (one player or game).
 * This is not enforced - multiple opens are permitted and the board is only
 * silenced on the last close - but a register write is atomic only with
 * respect to the IRQ handler on this single CPU, not between two processes.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#include "pc98snd_ioctl.h"

#define PC98SND_SOUND_ID_PORT	0x00a460u
#define PC98SND_IO_DELAY_PORT	0x00005fu

#define PC98SND_STATUS_TIMERA	0x01
#define PC98SND_STATUS_TIMERB	0x02
#define PC98SND_STATUS_BUSY	0x80

/* YM2608 timer-control (0x27) bits. */
#define PC98SND_TC_RESET_A	0x10
#define PC98SND_TC_RESET_B	0x20
#define PC98SND_TC_MODE	0xc0		/* FM3 special-mode (CSM/multi-freq) */

/* Default resources for the NEC PC-9801-86: I/O 0x188, INT5 -> IRQ12.
 * The DIP-switch alternatives are INT0->IRQ3, INT4->IRQ10, INT6->IRQ13. */
static unsigned int irq = 12;
module_param(irq, uint, 0444);
MODULE_PARM_DESC(irq, "board IRQ (default 12 = INT5; alternatives 3/10/13)");

static unsigned long base;      /* bank-1 I/O base (0x188/0x288) */
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "bank-1 I/O base, 0 = auto-detect (0x188/0x288)");

static unsigned long base2;		/* bank-2 address port (base + 4) */

static u8 shadow[0x200];		/* register shadow for readback */
static u8 detected_id;			/* board ID from 0xA460 at probe time */
static u32 pending_a, pending_b;	/* pending Timer-A/B underflow counts */
static wait_queue_head_t wq;
static spinlock_t lock;			/* protects the pending counters */
static atomic_t open_count = ATOMIC_INIT(0);

/* ------------------------------------------------------------------ */
/* Low-level register access                                          */
/* ------------------------------------------------------------------ */

static inline void pc98snd_delay(void)
{
	outb(0, PC98SND_IO_DELAY_PORT);
}

/* The busy flag (status bit 7) is only readable through the bank-1
 * address port.  Reading the status does NOT clear the timer underflow
 * flags (only a 0x27 reset-bit write does), so the busy poll never eats a
 * pending timer interrupt. */
static void pc98snd_wait_busy(void)
{
	int spins = 0;

	while (inb(base) & PC98SND_STATUS_BUSY) {
		if (++spins > 1000000)
			break;
		cpu_relax();
	}
}

/* Write one register.  The caller holds IRQs disabled so the IRQ handler's
 * own 0x27 write cannot interleave and corrupt the address latch. */
static void pc98snd_write_reg_locked(u16 addr, u8 val)
{
	unsigned long a = (addr & 0x100) ? base2 : base;
	unsigned long d = a + 2;

	pc98snd_wait_busy();
	outb((u8)(addr & 0xff), a);
	pc98snd_delay();
	pc98snd_delay();
	pc98snd_delay();
	outb(val, d);
	pc98snd_delay();
	shadow[addr & 0x1ff] = val;
}

static u8 pc98snd_read_reg_locked(u16 addr)
{
	unsigned long a = (addr & 0x100) ? base2 : base;
	unsigned long d = a + 2;
	u8 v;

	/* The real YM2608 has two physical read-ports:
	 * - Bank 0 register 0x0e: SSG I/O Port A (Joystick inputs)
	 * - Bank 1 register 0x08 (0x108): ADPCM data port (auto-increment)
	 */
	if (addr == 0x00e || addr == 0x108) {
		pc98snd_wait_busy();
		outb((u8)(addr & 0xff), a);
		pc98snd_delay();
		v = inb(d);
		shadow[addr & 0x1ff] = v;
		return v;
	}
	return shadow[addr & 0x1ff];
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                        */
/* ------------------------------------------------------------------ */

static irqreturn_t pc98snd_irq(int irqno, void *dev)
{
	u8 st = inb(base);
	u8 reset = 0;
	unsigned long f;

	if (st & PC98SND_STATUS_TIMERA)
		reset |= PC98SND_TC_RESET_A;
	if (st & PC98SND_STATUS_TIMERB)
		reset |= PC98SND_TC_RESET_B;

	if (!reset)
		return IRQ_NONE;

	/* Acknowledge the timer interrupt: clear the underflow flags and keep
	 * the free-running timers loaded and enabled.  The YM2608 clears both
	 * timer flags reliably only when reset A and reset B are written in the
	 * same 0x27 access, and bits 6-7 carry the FM3 special-mode (CSM/
	 * multi-freq) state, so preserve them from the register shadow. */
	pc98snd_write_reg_locked(0x27,
		(u8)((shadow[0x27] & PC98SND_TC_MODE) | 0x3f));

	spin_lock_irqsave(&lock, f);
	if (reset & PC98SND_TC_RESET_A)
		pending_a++;
	if (reset & PC98SND_TC_RESET_B)
		pending_b++;
	spin_unlock_irqrestore(&lock, f);

	wake_up_interruptible(&wq);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Character device                                                   */
/* ------------------------------------------------------------------ */

static int pc98snd_open(struct inode *inode, struct file *file)
{
	atomic_inc(&open_count);
	return nonseekable_open(inode, file);
}

static int pc98snd_release(struct inode *inode, struct file *file)
{
	unsigned long flags;

	if (!atomic_dec_and_test(&open_count))
		return 0;

	/* Last close: stop the board interrupting.  The sequencer's
	 * music_stop() leaves the timers running, so without this the chip
	 * would keep raising IRQs (and the handler would keep counting) after
	 * the player exits.  Clear the underflow flags, stop the one-shot
	 * timers, mask the board IRQ, and drop any stale counts. */
	local_irq_save(flags);
	pc98snd_write_reg_locked(0x27, 0x30);
	pc98snd_write_reg_locked(0x29, 0x00);
	spin_lock(&lock);
	pending_a = 0;
	pending_b = 0;
	spin_unlock(&lock);
	local_irq_restore(flags);
	return 0;
}

static ssize_t pc98snd_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct pc98snd_reg reg;
	unsigned long flags;

	if (count != sizeof(reg))
		return -EINVAL;
	if (copy_from_user(&reg, buf, sizeof(reg)))
		return -EFAULT;

	local_irq_save(flags);
	pc98snd_write_reg_locked(reg.addr, reg.data);
	local_irq_restore(flags);

	return count;
}

static ssize_t pc98snd_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct pc98snd_events ev;
	unsigned long f;

	/* read() consumes the pending timer-underflow counts. */
	if (count != sizeof(ev))
		return -EINVAL;

	/* Snapshot, copy, then subtract the copied amount back out.  This
	 * preserves a handler increment racing the copy and, on a failed
	 * copy_to_user, drops nothing (the counters are only decremented
	 * after the data is safely in userspace). */
	spin_lock_irqsave(&lock, f);
	ev.timer_a = pending_a;
	ev.timer_b = pending_b;
	spin_unlock_irqrestore(&lock, f);

	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;

	spin_lock_irqsave(&lock, f);
	pending_a -= ev.timer_a;
	pending_b -= ev.timer_b;
	spin_unlock_irqrestore(&lock, f);

	return sizeof(ev);
}

static __poll_t pc98snd_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;
	unsigned long f;

	poll_wait(file, &wq, wait);

	spin_lock_irqsave(&lock, f);
	if (pending_a || pending_b)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&lock, f);

	return mask;
}

static long pc98snd_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	unsigned long flags;

	switch (cmd) {
	case PC98SND_READ_REG: {
		struct pc98snd_reg reg;

		if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
			return -EFAULT;
		local_irq_save(flags);
		reg.data = pc98snd_read_reg_locked(reg.addr);
		local_irq_restore(flags);
		if (copy_to_user((void __user *)arg, &reg, sizeof(reg)))
			return -EFAULT;
		return 0;
	}
	case PC98SND_GET_STATUS: {
		u8 st = inb(base);

		if (copy_to_user((void __user *)arg, &st, 1))
			return -EFAULT;
		return 0;
	}
	case PC98SND_WRITE_REGS: {
		struct pc98snd_batch b;
		u32 i;

		if (copy_from_user(&b, (void __user *)arg, sizeof(b)))
			return -EFAULT;
		if (b.count == 0 || b.count > PC98SND_MAX_BATCH)
			return -EINVAL;

		/* Apply the whole sequence atomically w.r.t. the IRQ handler. */
		local_irq_save(flags);
		for (i = 0; i < b.count; i++)
			pc98snd_write_reg_locked(b.regs[i].addr,
						 b.regs[i].data);
		local_irq_restore(flags);
		return 0;
	}
	case PC98SND_GET_INFO: {
		struct pc98snd_info info;

		info.id = detected_id;
		info._pad = 0;
		info.base = (__u16)base;
		info.irq = irq;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations pc98snd_fops = {
	.owner		= THIS_MODULE,
	.open		= pc98snd_open,
	.release	= pc98snd_release,
	.write		= pc98snd_write,
	.read		= pc98snd_read,
	.poll		= pc98snd_poll,
	.unlocked_ioctl	= pc98snd_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice pc98snd_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "pc98snd0",
	.fops	= &pc98snd_fops,
};

/* ------------------------------------------------------------------ */
/* Detection                                                          */
/* ------------------------------------------------------------------ */

static int pc98snd_detect(unsigned long *base_out)
{
	u8 id = inb(PC98SND_SOUND_ID_PORT) >> 4;

	detected_id = id;

	/* Board IDs from the PC-98 sound-system ID (sound_pc9800.h):
	 *   2 = PC-9801-73  (0x188), 3 = PC-9801-73/76 (0x288)
	 *   4 = PC-9801-86  (0x188), 5 = PC-9801-86  (0x288) */
	switch (id) {
	case 2:
	case 4:
		*base_out = 0x188;
		return 0;
	case 3:
	case 5:
		*base_out = 0x288;
		return 0;
	default:
		return -ENODEV;
	}
}

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                   */
/* ------------------------------------------------------------------ */

static int __init pc98snd_init(void)
{
	unsigned long det;
	int err;

	switch (irq) {
	case 3:
	case 10:
	case 12:
	case 13:
		break;
	default:
		pr_err("pc98snd: IRQ %u is not a valid PC-9801-86 IRQ "
		       "(3/10/12/13)\n", irq);
		return -EINVAL;
	}

	if (pc98snd_detect(&det) != 0) {
		pr_err("pc98snd: no PC-9801-86/73 sound board detected\n");
		return -ENODEV;
	}
	if (base == 0)
		base = det;
	if (base != 0x188 && base != 0x288) {
		pr_err("pc98snd: bad base 0x%lx (valid: 0x188, 0x288)\n",
		       base);
		return -EINVAL;
	}
	base2 = base + 4;

	/* Bank-1 ports are base/base+2 and bank-2 ports are base+4/base+6,
	 * so the full 8-port range (base..base+7) is used. */
	if (!request_region(base, 8, "pc98snd")) {
		pr_err("pc98snd: I/O 0x%lx-0x%lx in use\n", base, base + 7);
		return -EBUSY;
	}

	init_waitqueue_head(&wq);
	spin_lock_init(&lock);
	memset(shadow, 0, sizeof(shadow));

	err = request_irq(irq, pc98snd_irq, 0, "pc98snd", &pc98snd_misc);
	if (err) {
		pr_err("pc98snd: cannot request IRQ %u: %d\n", irq, err);
		goto out_region;
	}

	err = misc_register(&pc98snd_misc);
	if (err) {
		pr_err("pc98snd: misc_register failed: %d\n", err);
		goto out_irq;
	}

	pr_info("pc98snd: PC-9801-86/73 at 0x%lx, IRQ %u -> /dev/%s\n",
		base, irq, pc98snd_misc.name);
	return 0;

out_irq:
	free_irq(irq, &pc98snd_misc);
out_region:
	release_region(base, 8);
	return err;
}

static void __exit pc98snd_exit(void)
{
	unsigned long flags;

	/* Disable the board's timer IRQ and stop the timers before tearing
	 * down the handler. */
	local_irq_save(flags);
	pc98snd_write_reg_locked(0x29, 0x00);
	pc98snd_write_reg_locked(0x27, 0x00);
	local_irq_restore(flags);

	misc_deregister(&pc98snd_misc);
	free_irq(irq, &pc98snd_misc);
	release_region(base, 8);
}

module_init(pc98snd_init);
module_exit(pc98snd_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PC-9801-86/73 (YM2608) sound board driver");
MODULE_AUTHOR("linux-pc98");
