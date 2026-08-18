// SPDX-License-Identifier: GPL-2.0-only
/*
 * NEC PC-9821 built-in Trident TGUI96xx framebuffer
 * Hardware 2D Accelerated + Multi-Resolution (640x480, 800x600, 1024x768, 1280x1024)
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#define DRV_NAME		"pc98tridentfb"

#define PCI_VENDOR_TRIDENT	0x1023
#define PCI_DEVICE_TGUI9660	0x9660

#define TG_VGA_BASE		0x03c0
#define TG_CRTC_COLOR		0x03d4
#define TG_STATUS_COLOR		0x03da
#define TG_CRTC_MONO		0x03b4
#define TG_STATUS_MONO		0x03ba
#define TG_VCLK			0x43c8
#define TG_SDAC			0x83c8

#define PC98_GDC_MODE		0x0068
#define PC98_WAIT		0x005f
#define PC98_RELAY		0x0fac

#define TG_BPP			8
#define TG_MAX_WIDTH		1280
#define TG_MAX_HEIGHT		1024
#define TG_MAX_PITCH		2048
#define TG_MAX_FB_SIZE		(TG_MAX_PITCH * TG_MAX_HEIGHT)
#define TG_BAR1_MIN_SIZE	0x10000
#define TG_VERIFY_PASSES	4

/* 2D Accelerator Constants */
#define ROP_P			0x06
#define ROP_S			0x0C
#define OLDCLR			0x2138
#define OLDSRC			0x213C
#define OLDDST			0x2140
#define OLDDIM			0x2144
#define OLDCMD			0x214A
#define DRAWFL			0x214C

#define point(x, y)		(((y) << 16) | (x))

static char *mode_option = "640x480";
module_param(mode_option, charp, 0444);
MODULE_PARM_DESC(mode_option, "Initial video mode e.g. '640x480', '800x600', '1024x768', '1280x1024'");

static unsigned long fb_phys;
module_param(fb_phys, ulong, 0444);
MODULE_PARM_DESC(fb_phys,
		 "Physical framebuffer override (0 selects BAR0 automatically)");

static bool allow_pc98_wakeup;
module_param(allow_pc98_wakeup, bool, 0444);
MODULE_PARM_DESC(allow_pc98_wakeup,
		 "Allow the last-resort port 0x94 wakeup (can touch the FDC)");

static bool noaccel;
module_param(noaccel, bool, 0444);
MODULE_PARM_DESC(noaccel, "Disable 2D hardware acceleration engine");

struct tg_mode_entry {
	const char *name;
	u32 xres;
	u32 yres;
	u32 pitch;
	unsigned long vclk_khz;
	u8 misc;
	u8 crtc[25];
};

static const struct tg_mode_entry tg_modes[] = {
	{
		.name = "640x400",
		.xres = 640,
		.yres = 400,
		.pitch = 1024,
		.vclk_khz = 25175,
		.misc = 0x63,
		.crtc = {
			0x5f, 0x4f, 0x50, 0x02, 0x52, 0x9e, 0xbf, 0x1f,
			0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x9c, 0x0e, 0x8f, 0x80, 0x00, 0x90, 0xbf, 0xc3,
			0xff,
		},
	},
	{
		.name = "640x480",
		.xres = 640,
		.yres = 480,
		.pitch = 1024,
		.vclk_khz = 25175,
		.misc = 0xeb,
		.crtc = {
			0x5f, 0x4f, 0x50, 0x02, 0x52, 0x9e, 0x0b, 0x3e,
			0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xea, 0x0c, 0xdf, 0x80, 0x00, 0xe0, 0x0b, 0xc3,
			0xff,
		},
	},
	{
		.name = "1280x480",
		.xres = 1280,
		.yres = 480,
		.pitch = 2048,
		.vclk_khz = 50350,
		.misc = 0xeb,
		.crtc = {
			0xc7, 0x9f, 0xa0, 0x02, 0xa4, 0x9e, 0x0b, 0x3e,
			0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xea, 0x0c, 0xdf, 0x00, 0x00, 0xe0, 0x0b, 0xc3,
			0xff,
		},
	},
	{
		.name = "1280x400",
		.xres = 1280,
		.yres = 400,
		.pitch = 2048,
		.vclk_khz = 50350,
		.misc = 0x63,
		.crtc = {
			0xc7, 0x9f, 0xa0, 0x02, 0xa4, 0x9e, 0xbf, 0x1f,
			0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x9c, 0x0e, 0x8f, 0x00, 0x00, 0x90, 0xbf, 0xc3,
			0xff,
		},
	},
	{
		.name = "800x600",
		.xres = 800,
		.yres = 600,
		.pitch = 1024,
		.vclk_khz = 40000,
		.misc = 0xef,
		.crtc = {
			0x7f, 0x63, 0x64, 0x02, 0x6a, 0x1d, 0x73, 0xf0,
			0x00, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x58, 0x0c, 0x57, 0x80, 0x00, 0x58, 0x73, 0xc3,
			0xff,
		},
	},
	{
		.name = "1024x768",
		.xres = 1024,
		.yres = 768,
		.pitch = 1024,
		.vclk_khz = 65000,
		.misc = 0xef,
		.crtc = {
			0xa3, 0x7f, 0x80, 0x04, 0x85, 0x14, 0x25, 0xf5,
			0x00, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x02, 0x0a, 0xff, 0x80, 0x00, 0x00, 0x25, 0xc3,
			0xff,
		},
	},
	{
		.name = "1280x1024",
		.xres = 1280,
		.yres = 1024,
		.pitch = 2048,
		.vclk_khz = 108000,
		.misc = 0xef,
		.crtc = {
			0xd7, 0x9f, 0xa0, 0x00, 0xa7, 0x9c, 0x27, 0xf5,
			0x00, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x03, 0x09, 0xff, 0x00, 0x00, 0x00, 0x27, 0xc3,
			0xff,
		},
	},
};

struct pc98trident_saved {
	u8 misc;
	u8 sr[0x10];
	u8 sr0d_old;
	u8 sr0d_new;
	u8 sr0e_new;
	u8 crtc[0x51];
	u8 gfx[0x70];
	u8 attr[0x15];
	u8 hidden_dac;
	u8 dac_mask;
	u8 dac[256 * 3];
	u8 vclk_lo;
	u8 vclk_hi;
	u8 sdac04;
	bool valid;
};

struct pc98tridentfb {
	struct pci_dev *pdev;
	struct fb_info *info;
	const struct tg_mode_entry *current_mode;
	void __iomem *regs;
	void __iomem *vram;
	u8 *shadow;
	u8 *hw_shadow;
	int wc_cookie;
	resource_size_t regs_phys;
	resource_size_t fb_phys;
	resource_size_t vram_size;
	u16 crtc;
	u16 status;
	u8 chip_rev;
	bool mmio;
	bool pio_claimed;
	bool bar1_claimed;
	bool bar0_claimed;
	bool fixed_fb_claimed;
	bool relay_active;
	spinlock_t reg_lock;
	struct mutex vram_lock;
	struct pc98trident_saved saved;
};

static inline u8 tg_read(struct pc98tridentfb *tfb, unsigned int port)
{
	if (tfb->mmio)
		return readb(tfb->regs + port);
	return inb(port);
}

static inline void tg_write(struct pc98tridentfb *tfb, unsigned int port,
			    u8 value)
{
	if (tfb->mmio)
		writeb(value, tfb->regs + port);
	else
		outb(value, port);
}

static inline void tg_select_crtc(struct pc98tridentfb *tfb, u8 misc)
{
	if (misc & 0x01) {
		tfb->crtc = TG_CRTC_COLOR;
		tfb->status = TG_STATUS_COLOR;
	} else {
		tfb->crtc = TG_CRTC_MONO;
		tfb->status = TG_STATUS_MONO;
	}
}

static inline u8 tg_misc_read(struct pc98tridentfb *tfb)
{
	return tg_read(tfb, TG_VGA_BASE + 0x0c);
}

static inline void tg_misc_write(struct pc98tridentfb *tfb, u8 value)
{
	tg_write(tfb, TG_VGA_BASE + 0x02, value);
	tg_select_crtc(tfb, value);
}

static inline u8 tg_seq_read(struct pc98tridentfb *tfb, u8 index)
{
	tg_write(tfb, TG_VGA_BASE + 4, index);
	return tg_read(tfb, TG_VGA_BASE + 5);
}

static inline void tg_seq_write(struct pc98tridentfb *tfb, u8 index, u8 value)
{
	tg_write(tfb, TG_VGA_BASE + 4, index);
	tg_write(tfb, TG_VGA_BASE + 5, value);
}

static inline u8 tg_crtc_read(struct pc98tridentfb *tfb, u8 index)
{
	tg_write(tfb, tfb->crtc, index);
	return tg_read(tfb, tfb->crtc + 1);
}

static inline void tg_crtc_write(struct pc98tridentfb *tfb, u8 index, u8 value)
{
	tg_write(tfb, tfb->crtc, index);
	tg_write(tfb, tfb->crtc + 1, value);
}

static inline u8 tg_gfx_read(struct pc98tridentfb *tfb, u8 index)
{
	tg_write(tfb, TG_VGA_BASE + 0x0e, index);
	return tg_read(tfb, TG_VGA_BASE + 0x0f);
}

static inline void tg_gfx_write(struct pc98tridentfb *tfb, u8 index, u8 value)
{
	tg_write(tfb, TG_VGA_BASE + 0x0e, index);
	tg_write(tfb, TG_VGA_BASE + 0x0f, value);
}

static inline u8 tg_attr_read(struct pc98tridentfb *tfb, u8 index)
{
	tg_read(tfb, tfb->status);
	tg_write(tfb, TG_VGA_BASE, index);
	return tg_read(tfb, TG_VGA_BASE + 1);
}

static inline void tg_attr_write(struct pc98tridentfb *tfb, u8 index, u8 value)
{
	tg_read(tfb, tfb->status);
	tg_write(tfb, TG_VGA_BASE, index);
	tg_write(tfb, TG_VGA_BASE, value);
}

static u8 tg_hidden_dac_read(struct pc98tridentfb *tfb)
{
	u8 value;

	tg_read(tfb, TG_VGA_BASE + 8);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 6);
	value = tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 8);
	return value;
}

static void tg_hidden_dac_write(struct pc98tridentfb *tfb, u8 value)
{
	tg_read(tfb, TG_VGA_BASE + 8);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_read(tfb, TG_VGA_BASE + 6);
	tg_write(tfb, TG_VGA_BASE + 6, value);
	tg_read(tfb, TG_VGA_BASE + 8);
}

static u8 tg_sdac_read(struct pc98tridentfb *tfb, u8 index)
{
	tg_write(tfb, TG_SDAC, index);
	return tg_read(tfb, TG_SDAC - 2);
}

static void tg_sdac_write(struct pc98tridentfb *tfb, u8 index, u8 value)
{
	tg_write(tfb, TG_SDAC, index);
	tg_write(tfb, TG_SDAC - 2, value);
}

static void tg_switch_old(struct pc98tridentfb *tfb)
{
	u8 value = tg_seq_read(tfb, 0x0b);

	tg_seq_write(tfb, 0x0b, value);
}

static void tg_switch_new(struct pc98tridentfb *tfb)
{
	tg_seq_read(tfb, 0x0b);
}

static bool tg_regs_alive(struct pc98tridentfb *tfb)
{
	u8 id, sr0, sr1;

	tg_switch_old(tfb);
	id = tg_seq_read(tfb, 0x0b);
	sr0 = tg_seq_read(tfb, 0x00);
	sr1 = tg_seq_read(tfb, 0x01);
	if (id == 0xff && sr0 == 0xff && sr1 == 0xff)
		return false;
	return id == 0xd3;
}

static int tg_claim_pio(struct pc98tridentfb *tfb)
{
	if (!request_region(0x03a0, 0x40, DRV_NAME))
		return -EBUSY;
	if (!request_region(TG_VCLK - 2, 4, DRV_NAME))
		goto err_vga;
	if (!request_region(TG_SDAC - 2, 3, DRV_NAME))
		goto err_vclk;
	tfb->pio_claimed = true;
	return 0;

err_vclk:
	release_region(TG_SDAC - 2, 3);
err_vga:
	release_region(0x03a0, 0x40);
	return -EBUSY;
}

static void tg_release_pio(struct pc98tridentfb *tfb)
{
	if (!tfb->pio_claimed)
		return;
	release_region(TG_SDAC - 2, 3);
	release_region(TG_VCLK - 2, 4);
	release_region(0x03a0, 0x40);
	tfb->pio_claimed = false;
}

static void tg_wakeup_at(void)
{
	u8 value = inb(TG_VGA_BASE + 3);

	outb(value == 0xff ? 0x01 : value | 0x01, TG_VGA_BASE + 3);
}

static void tg_wakeup_pc98(void)
{
	u8 value;

	outb(0x00, 0x0094);
	outb(0x01, 0x0102);
	outb(0x20, 0x0094);
	value = inb(TG_VGA_BASE + 3);
	outb(value == 0xff ? 0x01 : value | 0x01, TG_VGA_BASE + 3);
}

static int tg_select_access_path(struct pc98tridentfb *tfb)
{
	struct pci_dev *pdev = tfb->pdev;
	int ret;

	ret = tg_claim_pio(tfb);
	if (!ret) {
		tfb->mmio = false;
		tg_select_crtc(tfb, 1);
		if (tg_regs_alive(tfb)) {
			dev_info(&pdev->dev, "using native VGA I/O registers\n");
			return 0;
		}
	}

	if (pci_resource_len(pdev, 1) >= TG_BAR1_MIN_SIZE) {
		ret = pci_request_region(pdev, 1, DRV_NAME);
		if (!ret) {
			tfb->bar1_claimed = true;
			tfb->regs_phys = pci_resource_start(pdev, 1);
			tfb->regs = pci_iomap(pdev, 1, 0);
			if (tfb->regs) {
				tfb->mmio = true;
				tg_select_crtc(tfb, 1);
				if (tg_regs_alive(tfb)) {
					tg_release_pio(tfb);
					dev_info(&pdev->dev,
						 "using BAR1 register MMIO at %pa\n",
						 &tfb->regs_phys);
					return 0;
				}
				pci_iounmap(pdev, tfb->regs);
				tfb->regs = NULL;
			}
			pci_release_region(pdev, 1);
			tfb->bar1_claimed = false;
		}
	}

	if (!tfb->pio_claimed)
		return -ENODEV;
	tfb->mmio = false;
	tg_wakeup_at();
	if (tg_regs_alive(tfb))
		return 0;
	if (allow_pc98_wakeup) {
		tg_wakeup_pc98();
		if (tg_regs_alive(tfb))
			return 0;
	}

	dev_err(&pdev->dev, "no live PIO or BAR1 register path\n");
	return -ENODEV;
}

static int tg_fingerprint(struct pc98tridentfb *tfb)
{
	u8 id, old0e, signature;

	tg_switch_old(tfb);
	id = tg_seq_read(tfb, 0x0b);
	tfb->chip_rev = tg_seq_read(tfb, 0x09);
	old0e = tg_seq_read(tfb, 0x0e);
	tg_seq_write(tfb, 0x0e, 0x00);
	signature = tg_seq_read(tfb, 0x0e);
	tg_seq_write(tfb, 0x0e, old0e ^ 0x02);

	if (id != 0xd3 || (signature & 0x0f) != 0x02) {
		dev_err(&tfb->pdev->dev,
			"fingerprint failed: SR0B=%02x SR09=%02x SR0E=%02x\n",
			id, tfb->chip_rev, signature);
		return -ENODEV;
	}
	dev_info(&tfb->pdev->dev,
		 "TGUI96xx fingerprint: SR0B=%02x SR09=%02x\n",
		 id, tfb->chip_rev);
	return 0;
}

static resource_size_t tg_vram_size(struct pc98tridentfb *tfb)
{
	u8 value;

	tg_switch_new(tfb);
	tg_select_crtc(tfb, tg_misc_read(tfb));
	value = tg_crtc_read(tfb, 0x1f) & 0x0f;
	switch (value) {
	case 0x01:
		return SZ_512K;
	case 0x03:
		return SZ_1M;
	case 0x07:
		return SZ_2M;
	case 0x0f:
		return SZ_4M;
	default:
		dev_warn(&tfb->pdev->dev,
			 "unknown CR1F VRAM code %02x; assuming 2 MiB\n",
			 value);
		return SZ_2M;
	}
}

static u8 tg_encode_cr21(resource_size_t phys)
{
	u8 val = 0x20; /* Bit 5: enable linear aperture */

	val |= (phys >> 28) & 0x0f;        /* Bits 3..0: phys bits 31..28 */
	val |= ((phys >> 24) & 0x03) << 6;  /* Bits 7..6: phys bits 25..24 */
	return val;
}

static int tg_map_vram(struct pc98tridentfb *tfb)
{
	struct pci_dev *pdev = tfb->pdev;
	resource_size_t bar0 = pci_resource_start(pdev, 0);
	resource_size_t bar0_len = pci_resource_len(pdev, 0);
	int ret;

	if (fb_phys)
		tfb->fb_phys = fb_phys;
	else
		tfb->fb_phys = bar0 ? bar0 : 0x20000000;

	if (bar0 && tfb->fb_phys == bar0 && bar0_len >= tfb->vram_size) {
		ret = pci_request_region(pdev, 0, DRV_NAME);
		if (ret)
			return ret;
		tfb->bar0_claimed = true;
		tfb->vram = ioremap_wc(tfb->fb_phys, tfb->vram_size);
		tfb->wc_cookie = arch_phys_wc_add(tfb->fb_phys, bar0_len);
	} else {
		if (!request_mem_region(tfb->fb_phys, tfb->vram_size,
					DRV_NAME))
			return -EBUSY;
		tfb->fixed_fb_claimed = true;
		tfb->vram = ioremap_wc(tfb->fb_phys, tfb->vram_size);
	}
	if (!tfb->vram)
		goto err_release_region;

	dev_info(&pdev->dev, "framebuffer at %pa, %pa bytes (WC enabled, BAR0=%pa)\n",
		 &tfb->fb_phys, &tfb->vram_size, &bar0);
	return 0;

err_release_region:
	if (tfb->bar0_claimed) {
		pci_release_region(pdev, 0);
		tfb->bar0_claimed = false;
	}
	if (tfb->fixed_fb_claimed) {
		release_mem_region(tfb->fb_phys, tfb->vram_size);
		tfb->fixed_fb_claimed = false;
	}
	return -ENOMEM;
}

static void tg_unmap_vram(struct pc98tridentfb *tfb)
{
	if (tfb->wc_cookie >= 0) {
		arch_phys_wc_del(tfb->wc_cookie);
		tfb->wc_cookie = -1;
	}
	if (tfb->vram) {
		iounmap(tfb->vram);
		tfb->vram = NULL;
	}
	if (tfb->bar0_claimed) {
		pci_release_region(tfb->pdev, 0);
		tfb->bar0_claimed = false;
	}
	if (tfb->fixed_fb_claimed) {
		release_mem_region(tfb->fb_phys, tfb->vram_size);
		tfb->fixed_fb_claimed = false;
	}
}

static void tg_save_state(struct pc98tridentfb *tfb)
{
	struct pc98trident_saved *sv = &tfb->saved;
	int i;

	sv->misc = tg_misc_read(tfb);
	tg_select_crtc(tfb, sv->misc);
	tg_switch_new(tfb);
	sv->sr0d_new = tg_seq_read(tfb, 0x0d);
	sv->sr0e_new = tg_seq_read(tfb, 0x0e);
	tg_switch_old(tfb);
	sv->sr0d_old = tg_seq_read(tfb, 0x0d);
	tg_switch_new(tfb);
	for (i = 0; i < ARRAY_SIZE(sv->sr); i++)
		if (i != 0x0b && i != 0x0d && i != 0x0e)
			sv->sr[i] = tg_seq_read(tfb, i);
	for (i = 0; i < ARRAY_SIZE(sv->crtc); i++)
		sv->crtc[i] = tg_crtc_read(tfb, i);
	for (i = 0; i < ARRAY_SIZE(sv->gfx); i++)
		sv->gfx[i] = tg_gfx_read(tfb, i);
	for (i = 0; i < ARRAY_SIZE(sv->attr); i++)
		sv->attr[i] = tg_attr_read(tfb, i);
	sv->hidden_dac = tg_hidden_dac_read(tfb);
	sv->dac_mask = tg_read(tfb, TG_VGA_BASE + 6);
	tg_write(tfb, TG_VGA_BASE + 7, 0);
	for (i = 0; i < ARRAY_SIZE(sv->dac); i++)
		sv->dac[i] = tg_read(tfb, TG_VGA_BASE + 9);
	sv->vclk_lo = tg_read(tfb, TG_VCLK);
	sv->vclk_hi = tg_read(tfb, TG_VCLK + 1);
	sv->sdac04 = tg_sdac_read(tfb, 0x04);
	sv->valid = true;
}

static void tg_restore_state(struct pc98tridentfb *tfb)
{
	struct pc98trident_saved *sv = &tfb->saved;
	int i;

	if (!sv->valid)
		return;
	tg_switch_new(tfb);
	for (i = 0; i < ARRAY_SIZE(sv->sr); i++) {
		if (i == 0x01 || i == 0x0b || i == 0x0d || i == 0x0e)
			continue;
		tg_seq_write(tfb, i, sv->sr[i]);
	}
	tg_switch_old(tfb);
	tg_seq_write(tfb, 0x0d, sv->sr0d_old);
	tg_switch_new(tfb);
	tg_seq_write(tfb, 0x0d, sv->sr0d_new);
	tg_misc_write(tfb, sv->misc);
	tg_crtc_write(tfb, 0x11, sv->crtc[0x11] & 0x7f);
	for (i = 0; i < ARRAY_SIZE(sv->crtc); i++)
		if (i != 0x11)
			tg_crtc_write(tfb, i, sv->crtc[i]);
	tg_crtc_write(tfb, 0x11, sv->crtc[0x11]);
	for (i = 0; i < ARRAY_SIZE(sv->gfx); i++)
		tg_gfx_write(tfb, i, sv->gfx[i]);
	for (i = 0; i < ARRAY_SIZE(sv->attr); i++)
		tg_attr_write(tfb, i, sv->attr[i]);
	tg_read(tfb, tfb->status);
	tg_write(tfb, TG_VGA_BASE, 0x20);
	tg_write(tfb, TG_VGA_BASE + 8, 0);
	for (i = 0; i < ARRAY_SIZE(sv->dac); i++)
		tg_write(tfb, TG_VGA_BASE + 9, sv->dac[i]);
	tg_hidden_dac_write(tfb, sv->hidden_dac);
	tg_write(tfb, TG_VGA_BASE + 6, sv->dac_mask);
	tg_write(tfb, TG_VCLK, sv->vclk_lo);
	tg_write(tfb, TG_VCLK + 1, sv->vclk_hi);
	tg_sdac_write(tfb, 0x04, sv->sdac04);
	tg_seq_write(tfb, 0x0e, sv->sr0e_new ^ 0x02);
	tg_seq_write(tfb, 0x01, sv->sr[0x01]);
	sv->valid = false;
}

static void tg_load_palette(struct pc98tridentfb *tfb)
{
	unsigned int i;

	tg_write(tfb, TG_VGA_BASE + 6, 0xff);
	tg_write(tfb, TG_VGA_BASE + 8, 0);
	for (i = 0; i < 256; i++) {
		u8 r = (i >> 5) & 7;
		u8 g = (i >> 2) & 7;
		u8 b = i & 3;

		tg_write(tfb, TG_VGA_BASE + 9, r * 63 / 7);
		tg_write(tfb, TG_VGA_BASE + 9, g * 63 / 7);
		tg_write(tfb, TG_VGA_BASE + 9, b * 63 / 3);
	}
}

/* Set dotclock frequency via Trident PLL (TG_VCLK) */
static void set_vclk(struct pc98tridentfb *tfb, unsigned long freq_khz)
{
	int m, n, k;
	unsigned long fi, d, di;
	unsigned char best_m = 0, best_n = 0, best_k = 0;
	unsigned char shift = 1;

	d = 20000;
	for (k = shift; k >= 0; k--) {
		for (m = 1; m < 32; m++) {
			n = ((m + 2) << shift) - 8;
			for (n = (n < 0 ? 0 : n); n < 122; n++) {
				fi = ((14318L * (n + 8)) / (m + 2)) >> k;
				di = abs(fi - freq_khz);
				if (di < d || (di == d && k == best_k)) {
					d = di;
					best_m = m;
					best_n = n;
					best_k = k;
				}
			}
		}
	}
	tg_write(tfb, TG_VCLK, (best_m & 0x1f) | ((best_k & 3) << 5));
	tg_write(tfb, TG_VCLK + 1, best_n);
}

/* Set memory clock (MCLK) frequency to boost internal VRAM bandwidth */
static void set_mclk(struct pc98tridentfb *tfb, unsigned long freq_khz)
{
	int m, n, k;
	unsigned long fi, d, di;
	unsigned char best_m = 0, best_n = 0, best_k = 0;
	unsigned char shift = 1;

	d = 20000;
	for (k = shift; k >= 0; k--) {
		for (m = 1; m < 32; m++) {
			n = ((m + 2) << shift) - 8;
			for (n = (n < 0 ? 0 : n); n < 122; n++) {
				fi = ((14318L * (n + 8)) / (m + 2)) >> k;
				di = abs(fi - freq_khz);
				if (di < d || (di == d && k == best_k)) {
					d = di;
					best_m = m;
					best_n = n;
					best_k = k;
				}
			}
		}
	}
	tg_write(tfb, TG_VCLK - 2, 0x02);
	tg_write(tfb, TG_VCLK, (best_m & 0x1f) | ((best_k & 3) << 5));
	tg_write(tfb, TG_VCLK + 1, best_n);
}

/* 2D Hardware Acceleration Helpers */
static void tgui_wait_engine(struct pc98tridentfb *tfb)
{
	int count = 100000;

	if (!tfb->mmio)
		return;
	while ((readb(tfb->regs + 0x2120) & 0x80) && --count)
		cpu_relax();
}

static void tgui_init_accel(struct pc98tridentfb *tfb, int pitch)
{
	unsigned char x;

	if (!tfb->mmio)
		return;

	/* disable clipping */
	writel(0, tfb->regs + 0x2148);
	writel(point(4095, 2047), tfb->regs + 0x214C);

	switch (pitch) {
	case 2048:
		x = 0x08;
		break;
	case 1024:
	default:
		x = 0x04;
		break;
	}
	writeb(x, tfb->regs + 0x2122);
}

static void tgui_fill_rect(struct pc98tridentfb *tfb,
			   u32 x, u32 y, u32 w, u32 h, u32 color)
{
	if (!tfb->mmio || !w || !h)
		return;

	tgui_wait_engine(tfb);
	writeb(ROP_P, tfb->regs + 0x2127);
	writel(color, tfb->regs + OLDCLR);
	writel(0x4020, tfb->regs + DRAWFL);
	writel(point(w - 1, h - 1), tfb->regs + OLDDIM);
	writel(point(x, y), tfb->regs + OLDDST);
	writeb(1, tfb->regs + OLDCMD);
}

static void tgui_copy_rect(struct pc98tridentfb *tfb,
			   u32 x1, u32 y1, u32 x2, u32 y2, u32 w, u32 h)
{
	int flags = 0;
	u16 x1_tmp, x2_tmp, y1_tmp, y2_tmp;

	if (!tfb->mmio || !w || !h)
		return;

	if ((x1 < x2) && (y1 == y2)) {
		flags |= 0x0200;
		x1_tmp = x1 + w - 1;
		x2_tmp = x2 + w - 1;
	} else {
		x1_tmp = x1;
		x2_tmp = x2;
	}

	if (y1 < y2) {
		flags |= 0x0100;
		y1_tmp = y1 + h - 1;
		y2_tmp = y2 + h - 1;
	} else {
		y1_tmp = y1;
		y2_tmp = y2;
	}

	tgui_wait_engine(tfb);
	writeb(ROP_S, tfb->regs + 0x2127);
	writel(point(x1_tmp, y1_tmp), tfb->regs + OLDSRC);
	writel(point(x2_tmp, y2_tmp), tfb->regs + OLDDST);
	writel(point(w - 1, h - 1), tfb->regs + OLDDIM);
	writel(flags | 0x2000, tfb->regs + DRAWFL);
	writeb(1, tfb->regs + OLDCMD);
}

static void tgui_image_blit(struct pc98tridentfb *tfb, const char *data,
			    u32 x, u32 y, u32 w, u32 h, u32 c, u32 b)
{
	unsigned int size = ((w + 31) >> 5) * h;

	if (!tfb->mmio || !w || !h)
		return;

	tgui_wait_engine(tfb);
	writeb(ROP_P, tfb->regs + 0x2127);
	writel(c, tfb->regs + OLDCLR);
	writel(b, tfb->regs + 0x2134);
	writel(0x0040 | 0x4000, tfb->regs + DRAWFL);
	writel(point(w - 1, h - 1), tfb->regs + OLDDIM);
	writel(point(x, y), tfb->regs + OLDDST);
	writeb(1, tfb->regs + OLDCMD);

	iowrite32_rep(tfb->regs + 0x2100, data, size);
}

static inline void tgui_draw_line(struct pc98tridentfb *tfb,
				  u32 x1, u32 y1, u32 x2, u32 y2, u32 color)
{
	int dx = (int)x2 - (int)x1;
	int dy = (int)y2 - (int)y1;
	int ax = abs(dx);
	int ay = abs(dy);
	int flags = 0;

	if (!tfb->mmio)
		return;

	if (dx < 0) flags |= 0x0200;
	if (dy < 0) flags |= 0x0100;

	tgui_wait_engine(tfb);
	writeb(ROP_P, tfb->regs + 0x2127);
	writel(color, tfb->regs + OLDCLR);
	writel(point(x1, y1), tfb->regs + OLDDST);
	writel(point(ax, ay), tfb->regs + OLDDIM);
	writel(flags | 0x1000, tfb->regs + DRAWFL); /* Line Draw command */
	writeb(2, tfb->regs + OLDCMD);
}

/* Address of first shown pixel in display memory */
static void set_screen_start(struct pc98tridentfb *tfb, int base)
{
	u8 tmp;

	tg_crtc_write(tfb, 0x0c, (base >> 8) & 0xff);
	tg_crtc_write(tfb, 0x0d, base & 0xff);
	tmp = tg_crtc_read(tfb, 0x1e) & 0xdf;
	tg_crtc_write(tfb, 0x1e, tmp | ((base & 0x10000) >> 11));
	tmp = tg_crtc_read(tfb, 0x27) & 0xf8;
	tg_crtc_write(tfb, 0x27, tmp | ((base & 0xe0000) >> 17));
}

static int pc98tridentfb_pan_display(struct fb_var_screeninfo *var,
				     struct fb_info *info)
{
	struct pc98tridentfb *tfb = info->par;
	int offset;

	offset = (var->xoffset + (var->yoffset * var->xres_virtual)) * var->bits_per_pixel / 32;
	mutex_lock(&tfb->vram_lock);
	set_screen_start(tfb, offset);
	mutex_unlock(&tfb->vram_lock);
	return 0;
}

static const struct tg_mode_entry *tg_find_mode(u32 xres, u32 yres)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tg_modes); i++) {
		if (tg_modes[i].xres == xres && tg_modes[i].yres == yres)
			return &tg_modes[i];
	}
	return &tg_modes[0];
}

static void tg_set_mode(struct pc98tridentfb *tfb, const struct tg_mode_entry *m)
{
	unsigned int i;
	u16 offset = m->pitch / 8;

	tfb->current_mode = m;

	tg_switch_new(tfb);
	tg_seq_write(tfb, 0x00, 0x03);
	tg_seq_write(tfb, 0x01, 0x21);
	tg_seq_write(tfb, 0x0e, 0x82);
	tg_seq_write(tfb, 0x02, 0x0f);
	tg_seq_write(tfb, 0x03, 0x00);
	tg_seq_write(tfb, 0x04, 0x0e);
	tg_switch_old(tfb);
	tg_seq_write(tfb, 0x0d, 0x20);
	tg_switch_new(tfb);
	tg_seq_write(tfb, 0x0d, 0x00);

	set_vclk(tfb, m->vclk_khz);
	set_mclk(tfb, 60000); /* Program 60 MHz MCLK for 50ns/60ns EDO VRAM */
	tg_misc_write(tfb, m->misc);
	tg_crtc_write(tfb, 0x11, tg_crtc_read(tfb, 0x11) & 0x7f);
	for (i = 0; i < ARRAY_SIZE(m->crtc); i++)
		tg_crtc_write(tfb, i,
			      i == 0x13 ? offset : m->crtc[i]);
	tg_crtc_write(tfb, 0x27, (tg_crtc_read(tfb, 0x27) & 0x07) | 0x08);
	tg_crtc_write(tfb, 0x2b, 0);
	tg_crtc_write(tfb, 0x21, tg_encode_cr21(tfb->fb_phys));
	tg_crtc_write(tfb, 0x29, (tg_crtc_read(tfb, 0x29) & 0xcf) |
			      ((offset & 0x300) >> 4));
	
	/* Enable MMIO and PCI read/write bursting + write buffer */
	if (tfb->mmio)
		tg_crtc_write(tfb, 0x39, tg_crtc_read(tfb, 0x39) | 0x07);
	else
		tg_crtc_write(tfb, 0x39, tg_crtc_read(tfb, 0x39) & ~0x07);

	/* Enable PCI Retry on FIFO full + optimal watermark */
	tg_crtc_write(tfb, 0x55, (tg_crtc_read(tfb, 0x55) & ~0x70) | 0x31);

	/* Performance control: enable 32-bit CPU-VRAM path */
	tg_crtc_write(tfb, 0x2f, tg_crtc_read(tfb, 0x2f) | 0x10);

	/* Enable CPU write buffer and continuous 2D GE clock */
	tg_crtc_write(tfb, 0x38, tg_crtc_read(tfb, 0x38) | 0x08);
	tg_crtc_write(tfb, 0x3a, tg_crtc_read(tfb, 0x3a) | 0x10);

	/* Enable 64-bit interleaved memory bus mode on 2MB/4MB VRAM */
	if (tfb->vram_size >= 0x200000)
		tg_crtc_write(tfb, 0x5e, tg_crtc_read(tfb, 0x5e) | 0x01);

	tg_crtc_write(tfb, 0x50, 0);

	for (i = 0; i <= 4; i++)
		tg_gfx_write(tfb, i, 0);
	tg_gfx_write(tfb, 0x05, 0x40);
	tg_gfx_write(tfb, 0x06, 0x05);
	tg_gfx_write(tfb, 0x07, 0x0f);
	tg_gfx_write(tfb, 0x08, 0xff);

	tg_read(tfb, tfb->status);
	for (i = 0; i < 16; i++) {
		tg_write(tfb, TG_VGA_BASE, i);
		tg_write(tfb, TG_VGA_BASE, i);
	}
	tg_attr_write(tfb, 0x10, 0x41);
	tg_attr_write(tfb, 0x11, 0x00);
	tg_attr_write(tfb, 0x12, 0x0f);
	tg_attr_write(tfb, 0x13, 0x00);
	tg_attr_write(tfb, 0x14, 0x00);
	tg_read(tfb, tfb->status);
	tg_write(tfb, TG_VGA_BASE, 0x20);
	tg_hidden_dac_write(tfb, 0x00);
	tg_load_palette(tfb);

	/* Ra43 fetch state */
	tg_crtc_write(tfb, 0x1e, 0x80);
	tg_crtc_write(tfb, 0x2a, tfb->saved.crtc[0x2a]);
	tg_gfx_write(tfb, 0x0f, (tfb->saved.gfx[0x0f] & 0xf0) | 0x12);
	tg_gfx_write(tfb, 0x2f, 0x24);

	if (!noaccel)
		tgui_init_accel(tfb, m->pitch);
}

static void tg_relay_to_trident(struct pc98tridentfb *tfb)
{
	u8 value;

	outb(0x0e, PC98_GDC_MODE);
	inb(PC98_WAIT);
	inb(PC98_WAIT);
	tg_gfx_write(tfb, 0x21, tg_gfx_read(tfb, 0x21) & ~0x20);
	tg_crtc_write(tfb, 0x23, tg_crtc_read(tfb, 0x23) & ~0x20);
	tg_crtc_write(tfb, 0x29, tg_crtc_read(tfb, 0x29) | 0x04);
	value = tg_sdac_read(tfb, 0x04) | 0x06;
	tg_sdac_write(tfb, 0x04, value);
	usleep_range(1000, 2000);
	tg_sdac_write(tfb, 0x04, value | 0x08);
	tg_gfx_write(tfb, 0x23, tg_gfx_read(tfb, 0x23) & ~0x03);
	tg_sdac_write(tfb, 0x04, value | 0x09);
	tg_seq_write(tfb, 0x01, tg_seq_read(tfb, 0x01) & ~0x10);
	outb(0x02, PC98_RELAY);
	tfb->relay_active = true;
}

static void tg_relay_to_gdc(struct pc98tridentfb *tfb)
{
	if (!tfb->relay_active)
		return;
	outb(0x00, PC98_RELAY);
	tg_seq_write(tfb, 0x01, tg_seq_read(tfb, 0x01) | 0x10);
	tg_sdac_write(tfb, 0x04, tg_sdac_read(tfb, 0x04) & ~0x0f);
	tg_gfx_write(tfb, 0x23, (tg_gfx_read(tfb, 0x23) & ~0x03) | 0x01);
	tg_crtc_write(tfb, 0x29, tg_crtc_read(tfb, 0x29) & ~0x04);
	tg_crtc_write(tfb, 0x23, tg_crtc_read(tfb, 0x23) | 0x20);
	tg_gfx_write(tfb, 0x21, tg_gfx_read(tfb, 0x21) | 0x20);
	outb(0x0f, PC98_GDC_MODE);
	tfb->relay_active = false;
}

static int pc98tridentfb_check_var(struct fb_var_screeninfo *var,
				   struct fb_info *info)
{
	const struct tg_mode_entry *m = tg_find_mode(var->xres, var->yres);

	var->xres = m->xres;
	var->yres = m->yres;
	if (var->xres_virtual < m->xres)
		var->xres_virtual = m->xres;
	if (var->yres_virtual < m->yres)
		var->yres_virtual = m->yres;
	var->xoffset = 0;
	var->yoffset = 0;
	var->bits_per_pixel = TG_BPP;
	var->grayscale = 0;

	/* 8-bit Pseudocolor colormap */
	var->red.offset = 0;
	var->red.length = 8;
	var->green.offset = 0;
	var->green.length = 8;
	var->blue.offset = 0;
	var->blue.length = 8;
	var->transp.offset = 0;
	var->transp.length = 0;

	return 0;
}

static int pc98tridentfb_set_par(struct fb_info *info)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tg_find_mode(info->var.xres, info->var.yres);

	mutex_lock(&tfb->vram_lock);
	info->fix.line_length = m->pitch;
	info->fix.smem_len = m->pitch * m->yres;
	tg_set_mode(tfb, m);
	mutex_unlock(&tfb->vram_lock);
	return 0;
}

static int pc98tridentfb_setcolreg(unsigned int regno, unsigned int red,
				   unsigned int green, unsigned int blue,
				   unsigned int transp,
				   struct fb_info *info)
{
	struct pc98tridentfb *tfb = info->par;
	unsigned long flags;

	if (regno >= 256)
		return -EINVAL;
	spin_lock_irqsave(&tfb->reg_lock, flags);
	tg_write(tfb, TG_VGA_BASE + 8, regno);
	tg_write(tfb, TG_VGA_BASE + 9, red >> 10);
	tg_write(tfb, TG_VGA_BASE + 9, green >> 10);
	tg_write(tfb, TG_VGA_BASE + 9, blue >> 10);
	spin_unlock_irqrestore(&tfb->reg_lock, flags);
	return 0;
}

static int pc98tridentfb_blank(int blank, struct fb_info *info)
{
	struct pc98tridentfb *tfb = info->par;
	unsigned long flags;
	u8 value;

	spin_lock_irqsave(&tfb->reg_lock, flags);
	value = tg_seq_read(tfb, 0x01);
	if (blank == FB_BLANK_UNBLANK)
		value &= ~0x20;
	else
		value |= 0x20;
	tg_seq_write(tfb, 0x01, value);
	spin_unlock_irqrestore(&tfb->reg_lock, flags);
	return 0;
}

/* Fast 32-bit hardware burst string copy */
static inline void tg_burst_copy(void __iomem *dst, const u32 *src, size_t ndwords)
{
	asm volatile (
		"cld\n\t"
		"rep movsl\n\t"
		: "+D" (dst), "+S" (src), "+c" (ndwords)
		:
		: "memory"
	);
}

/*
 * Paced write with fast repair verification:
 * 1. Paces writes in 16-dword (64-byte) burst windows with read flush.
 * 2. Runs up to TG_VERIFY_PASSES reading back only to catch and repair
 *    any rare dropped dword on the Ra43 PCI bridge.
 */
static int tg_store_verified(struct pc98tridentfb *tfb, unsigned long offset,
			     const u8 *src, size_t nbytes)
{
	u8 __iomem *dst = tfb->vram + offset;
	const u32 *src32 = (const u32 *)src;
	size_t ndwords = nbytes / sizeof(*src32);
	unsigned int pass;
	size_t i;
	bool bad;

	if (WARN_ON_ONCE((offset | nbytes) & (sizeof(*src32) - 1)))
		return -EINVAL;

	/* Stream writes in 16-dword (64-byte) bursts paced with read flush */
	for (i = 0; i < ndwords; i += 16) {
		size_t chunk = min_t(size_t, 16, ndwords - i);
		tg_burst_copy(dst + i * sizeof(u32), src32 + i, chunk);
		(void)readl(dst + (i + chunk - 1) * sizeof(u32));
	}

	for (pass = 0; pass < TG_VERIFY_PASSES; pass++) {
		bad = false;
		for (i = 0; i < ndwords; i++) {
			if (readl(dst + i * sizeof(*src32)) == src32[i])
				continue;
			writel(src32[i], dst + i * sizeof(*src32));
			(void)readl(dst + i * sizeof(*src32));
			bad = true;
		}
		if (!bad)
			return pass;
	}

	return -EIO;
}

static void tg_flush_rows(struct fb_info *info, unsigned int first,
			  unsigned int last)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];
	unsigned int failed = 0;
	unsigned int y;

	first = min(first, (unsigned int)m->yres);
	last = min(last, (unsigned int)m->yres);
	if (first >= last)
		return;

	mutex_lock(&tfb->vram_lock);
	for (y = first; y < last; y++) {
		unsigned long offset = y * m->pitch;
		u8 *src = tfb->shadow + offset;
		u8 *mirror = tfb->hw_shadow + offset;

		/* Differential skip: in L2 cache RAM, skip if scanline is unchanged */
		if (memcmp(src, mirror, m->xres) == 0)
			continue;

		if (tg_store_verified(tfb, offset, src, m->xres) < 0)
			failed++;
		else
			memcpy(mirror, src, m->xres);
	}
	mutex_unlock(&tfb->vram_lock);

	if (failed)
		dev_warn_ratelimited(&tfb->pdev->dev,
				     "VRAM verification failed on %u row(s)\n",
				     failed);
}

static void pc98tridentfb_damage_range(struct fb_info *info, off_t offset,
				       size_t len)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];
	size_t end;

	if (!len || offset < 0 || offset >= TG_MAX_FB_SIZE)
		return;
	end = offset + min_t(size_t, len, TG_MAX_FB_SIZE - offset);
	tg_flush_rows(info, offset / m->pitch,
		      DIV_ROUND_UP(end, m->pitch));
}

static void pc98tridentfb_damage_area(struct fb_info *info, u32 x, u32 y,
				      u32 width, u32 height)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];

	if (!width || !height || x >= m->xres || y >= m->yres)
		return;
	tg_flush_rows(info, y, y + min_t(u32, height, m->yres - y));
}

static void pc98tridentfb_deferred_io(struct fb_info *info,
				      struct list_head *pagelist)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];
	struct fb_deferred_io_pageref *pageref;

	list_for_each_entry(pageref, pagelist, list) {
		size_t offset = min_t(size_t, pageref->offset, TG_MAX_FB_SIZE);
		size_t end = min_t(size_t, offset + PAGE_SIZE, TG_MAX_FB_SIZE);

		tg_flush_rows(info, offset / m->pitch,
			      DIV_ROUND_UP(end, m->pitch));
	}
}

/* Hardware accelerated operations */
static void pc98tridentfb_fillrect(struct fb_info *info,
				   const struct fb_fillrect *rect)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];

	if (!noaccel && tfb->mmio && rect->rop == ROP_COPY) {
		mutex_lock(&tfb->vram_lock);
		tgui_fill_rect(tfb, rect->dx, rect->dy,
			       rect->width, rect->height, rect->color);

		/* Sync hw_shadow mirror so deferred IO does not re-push filled rect over PCI */
		if (tfb->hw_shadow) {
			u32 y;
			for (y = rect->dy; y < rect->dy + rect->height && y < m->yres; y++) {
				unsigned long offset = y * m->pitch + rect->dx;
				if (rect->dx + rect->width <= m->xres)
					memset(tfb->hw_shadow + offset, rect->color, rect->width);
			}
		}
		mutex_unlock(&tfb->vram_lock);
		sys_fillrect(info, rect);
	} else {
		sys_fillrect(info, rect);
	}
}

static void pc98tridentfb_copyarea(struct fb_info *info,
				   const struct fb_copyarea *area)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];

	if (!noaccel && tfb->mmio) {
		mutex_lock(&tfb->vram_lock);
		tgui_copy_rect(tfb, area->sx, area->sy,
			       area->dx, area->dy,
			       area->width, area->height);

		/* Sync hw_shadow mirror so deferred IO does not re-push copied area over PCI */
		if (tfb->hw_shadow) {
			u32 y;
			for (y = 0; y < area->height && (area->dy + y) < m->yres; y++) {
				unsigned long dst_off = (area->dy + y) * m->pitch + area->dx;
				unsigned long src_off = (area->sy + y) * m->pitch + area->sx;
				if (area->dx + area->width <= m->xres && area->sx + area->width <= m->xres)
					memcpy(tfb->hw_shadow + dst_off, tfb->hw_shadow + src_off, area->width);
			}
		}
		mutex_unlock(&tfb->vram_lock);
		sys_copyarea(info, area);
	} else {
		sys_copyarea(info, area);
	}
}

static void pc98tridentfb_imageblit(struct fb_info *info,
				    const struct fb_image *img)
{
	struct pc98tridentfb *tfb = info->par;
	const struct tg_mode_entry *m = tfb->current_mode ? tfb->current_mode : &tg_modes[0];

	if (!noaccel && tfb->mmio && img->depth == 1) {
		u32 col = img->fg_color;
		u32 bgcol = img->bg_color;

		mutex_lock(&tfb->vram_lock);
		tgui_image_blit(tfb, img->data, img->dx, img->dy,
				img->width, img->height, col, bgcol);
		mutex_unlock(&tfb->vram_lock);
		sys_imageblit(info, img);
		if (tfb->hw_shadow && img->dx + img->width <= m->xres) {
			u32 y;
			for (y = 0; y < img->height && (img->dy + y) < m->yres; y++) {
				unsigned long offset = (img->dy + y) * m->pitch + img->dx;
				memcpy(tfb->hw_shadow + offset, tfb->shadow + offset, img->width);
			}
		}
	} else {
		sys_imageblit(info, img);
	}
}

static int pc98tridentfb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	struct pc98tridentfb *tfb = info->par;

	if (noaccel || !tfb->mmio)
		return -EINVAL;

	mutex_lock(&tfb->vram_lock);
	if (cursor->set & FB_CUR_SETPOS) {
		tg_crtc_write(tfb, 0x40, cursor->image.dx & 0xff);
		tg_crtc_write(tfb, 0x41, (cursor->image.dx >> 8) & 0x07);
		tg_crtc_write(tfb, 0x42, cursor->image.dy & 0xff);
		tg_crtc_write(tfb, 0x43, (cursor->image.dy >> 8) & 0x07);
	}
	if (cursor->set & FB_CUR_SETHOT) {
		tg_crtc_write(tfb, 0x46, cursor->hot.x & 0x3f);
		tg_crtc_write(tfb, 0x47, cursor->hot.y & 0x3f);
	}
	if (cursor->set & FB_CUR_SETCMAP) {
		u32 fg = cursor->image.fg_color;
		u32 bg = cursor->image.bg_color;
		tg_crtc_write(tfb, 0x49, (bg >> 16) & 0xff);
		tg_crtc_write(tfb, 0x4a, (bg >> 8) & 0xff);
		tg_crtc_write(tfb, 0x4b, bg & 0xff);
		tg_crtc_write(tfb, 0x4c, (fg >> 16) & 0xff);
		tg_crtc_write(tfb, 0x4d, (fg >> 8) & 0xff);
		tg_crtc_write(tfb, 0x4e, fg & 0xff);
	}
	if (cursor->enable) {
		/* Set cursor location to top 4 KB of VRAM (offset = 0x3FF000 >> 10) */
		u32 loc = (tfb->vram_size - 0x1000) >> 10;
		tg_crtc_write(tfb, 0x44, loc & 0xff);
		tg_crtc_write(tfb, 0x45, (loc >> 8) & 0x0f);
		/* Enable 64x64 Hardware Cursor */
		tg_crtc_write(tfb, 0x48, 0x07);
	} else {
		tg_crtc_write(tfb, 0x48, 0x00);
	}
	mutex_unlock(&tfb->vram_lock);
	return 0;
}

static int pc98tridentfb_sync(struct fb_info *info)
{
	struct pc98tridentfb *tfb = info->par;

	if (!noaccel && tfb->mmio) {
		mutex_lock(&tfb->vram_lock);
		tgui_wait_engine(tfb);
		mutex_unlock(&tfb->vram_lock);
	}
	return 0;
}

static int pc98tridentfb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	struct pc98tridentfb *tfb = info->par;

	if (cmd == FBIO_WAITFORVSYNC) {
		unsigned int count = 200000;
		while ((tg_read(tfb, tfb->status) & 0x08) && --count)
			cpu_relax();
		count = 200000;
		while (!(tg_read(tfb, tfb->status) & 0x08) && --count)
			cpu_relax();
		return 0;
	}
	return -EINVAL;
}

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(pc98tridentfb,
				   pc98tridentfb_damage_range,
				   pc98tridentfb_damage_area)

static const struct fb_ops pc98tridentfb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(pc98tridentfb),
	.fb_check_var	= pc98tridentfb_check_var,
	.fb_set_par	= pc98tridentfb_set_par,
	.fb_setcolreg	= pc98tridentfb_setcolreg,
	.fb_blank	= pc98tridentfb_blank,
	.fb_pan_display	= pc98tridentfb_pan_display,
	.fb_fillrect	= pc98tridentfb_fillrect,
	.fb_copyarea	= pc98tridentfb_copyarea,
	.fb_imageblit	= pc98tridentfb_imageblit,
	.fb_cursor	= pc98tridentfb_cursor,
	.fb_sync	= pc98tridentfb_sync,
	.fb_ioctl	= pc98tridentfb_ioctl,
};

static struct fb_deferred_io pc98tridentfb_defio = {
	.delay			= DIV_ROUND_UP(HZ, 60),
	.sort_pagereflist	= true,
	.deferred_io		= pc98tridentfb_deferred_io,
};

static void tg_release_access_path(struct pc98tridentfb *tfb)
{
	if (tfb->regs)
		pci_iounmap(tfb->pdev, tfb->regs);
	if (tfb->bar1_claimed)
		pci_release_region(tfb->pdev, 1);
	tfb->regs = NULL;
	tfb->bar1_claimed = false;
	tg_release_pio(tfb);
}

static int pc98tridentfb_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	struct pc98tridentfb *tfb;
	struct fb_info *info;
	const struct tg_mode_entry *initial_mode;
	int ret;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);
	pci_try_set_mwi(pdev);
	pci_write_config_byte(pdev, PCI_LATENCY_TIMER, 128);

	/* Enable Fast Back-to-Back PCI transactions */
	{
		u16 pci_cmd;
		pci_read_config_word(pdev, PCI_COMMAND, &pci_cmd);
		pci_cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_FAST_BACK;
		pci_write_config_word(pdev, PCI_COMMAND, pci_cmd);
	}

	/* Optimize Trident PCI Master Control (Offset 0x40) */
	{
		u8 pci_ctrl;
		pci_read_config_byte(pdev, 0x40, &pci_ctrl);
		pci_write_config_byte(pdev, 0x40, pci_ctrl | 0x07);
	}

	info = framebuffer_alloc(sizeof(*tfb), &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto err_disable;
	}
	tfb = info->par;
	tfb->pdev = pdev;
	tfb->info = info;
	tfb->wc_cookie = -1;
	tfb->crtc = TG_CRTC_COLOR;
	tfb->status = TG_STATUS_COLOR;
	spin_lock_init(&tfb->reg_lock);
	mutex_init(&tfb->vram_lock);

	ret = tg_select_access_path(tfb);
	if (ret)
		goto err_release_info;
	ret = tg_fingerprint(tfb);
	if (ret)
		goto err_access;
	tfb->vram_size = tg_vram_size(tfb);
	tg_save_state(tfb);
	ret = tg_map_vram(tfb);
	if (ret)
		goto err_restore;
	tfb->shadow = vzalloc(TG_MAX_FB_SIZE);
	if (!tfb->shadow) {
		ret = -ENOMEM;
		goto err_unwind_video;
	}
	tfb->hw_shadow = vzalloc(TG_MAX_FB_SIZE);
	if (!tfb->hw_shadow) {
		ret = -ENOMEM;
		goto err_unwind_shadow;
	}

	initial_mode = &tg_modes[0];
	if (mode_option) {
		int i;
		for (i = 0; i < ARRAY_SIZE(tg_modes); i++) {
			if (strstr(mode_option, tg_modes[i].name)) {
				initial_mode = &tg_modes[i];
				break;
			}
		}
	}

	tg_set_mode(tfb, initial_mode);
	tg_relay_to_trident(tfb);
	tg_flush_rows(info, 0, initial_mode->yres);
	tg_seq_write(tfb, 0x01, 0x01);

	strscpy(info->fix.id, "PC98 TGUI96xx", sizeof(info->fix.id));
	info->fix.smem_start = 0;
	info->fix.smem_len = initial_mode->pitch * initial_mode->yres;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_PSEUDOCOLOR;
	info->fix.line_length = initial_mode->pitch;
	info->fix.accel = FB_ACCEL_TRIDENT_TGUI;
	info->fix.ypanstep = 1;
	info->fix.ywrapstep = 0;
	info->var.xres = initial_mode->xres;
	info->var.yres = initial_mode->yres;
	info->var.xres_virtual = initial_mode->xres;
	info->var.yres_virtual = initial_mode->yres;
	info->var.bits_per_pixel = TG_BPP;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;
	pc98tridentfb_check_var(&info->var, info);
	info->screen_buffer = tfb->shadow;
	info->screen_size = TG_MAX_FB_SIZE;
	info->fbops = &pc98tridentfb_ops;
	info->flags = FBINFO_VIRTFB | FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT | FBINFO_HWACCEL_IMAGEBLIT | FBINFO_HWACCEL_YPAN;
	info->fbdefio = &pc98tridentfb_defio;

	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		goto err_unwind_hw_shadow;
	ret = fb_deferred_io_init(info);
	if (ret)
		goto err_cmap;
	ret = register_framebuffer(info);
	if (ret)
		goto err_defio;

	pci_set_drvdata(pdev, info);
	dev_info(&pdev->dev,
		 "fb%d: PC-98 TGUI96xx %ux%ux8 (2D accel %s, multi-mode), pitch %u\n",
		 info->node, initial_mode->xres, initial_mode->yres,
		 (!noaccel && tfb->mmio) ? "enabled" : "disabled", initial_mode->pitch);
	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_unwind_hw_shadow:
	vfree(tfb->hw_shadow);
	tfb->hw_shadow = NULL;
err_unwind_shadow:
	vfree(tfb->shadow);
	tfb->shadow = NULL;
err_unwind_video:
	tg_relay_to_gdc(tfb);
	tg_unmap_vram(tfb);
err_restore:
	tg_restore_state(tfb);
err_access:
	tg_release_access_path(tfb);
err_release_info:
	framebuffer_release(info);
err_disable:
	pci_disable_device(pdev);
	return ret;
}

static void pc98tridentfb_remove(struct pci_dev *pdev)
{
	struct fb_info *info = pci_get_drvdata(pdev);
	struct pc98tridentfb *tfb;

	if (!info)
		return;
	tfb = info->par;
	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);
	fb_dealloc_cmap(&info->cmap);
	vfree(tfb->hw_shadow);
	tfb->hw_shadow = NULL;
	vfree(tfb->shadow);
	tfb->shadow = NULL;
	tg_relay_to_gdc(tfb);
	tg_unmap_vram(tfb);
	tg_restore_state(tfb);
	tg_release_access_path(tfb);
	framebuffer_release(info);
	pci_disable_device(pdev);
}

static const struct pci_device_id pc98tridentfb_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_TRIDENT, PCI_DEVICE_TGUI9660) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pc98tridentfb_pci_tbl);

static struct pci_driver pc98tridentfb_driver = {
	.name		= DRV_NAME,
	.id_table	= pc98tridentfb_pci_tbl,
	.probe		= pc98tridentfb_probe,
	.remove		= pc98tridentfb_remove,
};

module_pci_driver(pc98tridentfb_driver);

MODULE_AUTHOR("PC-9800 Lovers");
MODULE_DESCRIPTION("NEC PC-9821 Trident TGUI96xx framebuffer driver (Multi-Mode)");
MODULE_LICENSE("GPL");
