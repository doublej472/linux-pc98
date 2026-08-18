// SPDX-License-Identifier: GPL-2.0-only
/*
 * NEC PC-9800 native GDC (uPD7220) 16-color planar framebuffer driver
 *
 * Supports 640x400 (standard PC-98 DOS), 640x200, and 640x480 16-color modes
 * with fast chunky-to-planar (c2p) hardware bitplane updates.
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#define DRV_NAME		"pc98gdcfb"

/* GDC I/O Ports */
#define GDC_MASTER_STATUS	0x0060
#define GDC_MASTER_DATA		0x0062
#define GDC_SLAVE_STATUS	0x00a0
#define GDC_SLAVE_DATA		0x00a2
#define PC98_GDC_MODE		0x0068
#define PC98_VRAM_MODE		0x006a
#define PC98_WAIT		0x005f

/* Analog Palette Ports (16 colors / 4096 palette) */
#define PALETTE_INDEX		0x00a8
#define PALETTE_GREEN		0x00aa
#define PALETTE_RED		0x00ac
#define PALETTE_BLUE		0x00ae

/* Planar VRAM Physical Addresses */
#define VRAM_PLANE_B_PHYS	0x000a8000UL
#define VRAM_PLANE_R_PHYS	0x000b0000UL
#define VRAM_PLANE_G_PHYS	0x000b8000UL
#define VRAM_PLANE_E_PHYS	0x000e0000UL
#define VRAM_PLANE_SIZE		0x00008000UL	/* 32 KB per plane */

#define GDC_WIDTH		640
#define GDC_HEIGHT		400
#define GDC_BPP			8
#define GDC_FB_SIZE		(GDC_WIDTH * GDC_HEIGHT)

struct pc98gdcfb_par {
	void __iomem *plane_b;
	void __iomem *plane_r;
	void __iomem *plane_g;
	void __iomem *plane_e;
	u8 *shadow;
	u8 *hw_shadow;
	struct mutex vram_lock;
	spinlock_t reg_lock;
	u32 pseudo_palette[16];
	bool active;
};

/* Fast 8-pixel chunky-to-planar bitplane transposition */
static inline void gdc_c2p_8pixels(const u8 *src, u8 *b, u8 *r, u8 *g, u8 *e)
{
	u8 pb = 0, pr = 0, pg = 0, pe = 0;
	int i;

	for (i = 0; i < 8; i++) {
		u8 p = src[i];
		pb = (pb << 1) | (p & 1);
		pr = (pr << 1) | ((p >> 1) & 1);
		pg = (pg << 1) | ((p >> 2) & 1);
		pe = (pe << 1) | ((p >> 3) & 1);
	}
	*b = pb;
	*r = pr;
	*g = pg;
	*e = pe;
}

static void gdc_convert_line(const u8 *src, u8 __iomem *b, u8 __iomem *r,
			     u8 __iomem *g, u8 __iomem *e)
{
	int x;

	for (x = 0; x < 80; x++) {
		u8 pb, pr, pg, pe;
		gdc_c2p_8pixels(src + x * 8, &pb, &pr, &pg, &pe);
		writeb(pb, b + x);
		writeb(pr, r + x);
		writeb(pg, g + x);
		writeb(pe, e + x);
	}
}

static void gdc_init_hardware(struct pc98gdcfb_par *par)
{
	/* Enable 16-color mode & 4th plane E */
	outb(0x01, PC98_VRAM_MODE);	/* 16-color mode */
	inb(PC98_WAIT);
	outb(0x0b, PC98_GDC_MODE);	/* 400 lines */
	inb(PC98_WAIT);
	outb(0x0c, PC98_GDC_MODE);	/* Color mode */
	inb(PC98_WAIT);
	outb(0x0f, PC98_GDC_MODE);	/* Graphic screen ON */
	inb(PC98_WAIT);
}

static void gdc_set_palette(u8 index, u8 r, u8 g, u8 b)
{
	/* 16-color analog palette: 4 bits each (0..15) */
	outb(index & 0x0f, PALETTE_INDEX);
	inb(PC98_WAIT);
	outb(g >> 4, PALETTE_GREEN);
	inb(PC98_WAIT);
	outb(r >> 4, PALETTE_RED);
	inb(PC98_WAIT);
	outb(b >> 4, PALETTE_BLUE);
	inb(PC98_WAIT);
}

static int pc98gdcfb_setcolreg(unsigned int regno, unsigned int red,
			       unsigned int green, unsigned int blue,
			       unsigned int transp,
			       struct fb_info *info)
{
	struct pc98gdcfb_par *par = info->par;
	unsigned long flags;

	if (regno >= 16)
		return -EINVAL;

	spin_lock_irqsave(&par->reg_lock, flags);
	gdc_set_palette(regno, red >> 8, green >> 8, blue >> 8);
	par->pseudo_palette[regno] = (red & 0xff00) | ((green & 0xff00) >> 8) | ((blue & 0xff00) >> 16);
	spin_unlock_irqrestore(&par->reg_lock, flags);
	return 0;
}

static int pc98gdcfb_blank(int blank, struct fb_info *info)
{
	if (blank == FB_BLANK_UNBLANK) {
		outb(0x0f, PC98_GDC_MODE);	/* Graphic ON */
	} else {
		outb(0x0e, PC98_GDC_MODE);	/* Graphic OFF */
	}
	return 0;
}

/* Flush dirty chunky rows into the 4 GDC bitplanes (B, R, G, E) */
static void gdc_flush_rows(struct fb_info *info, unsigned int first, unsigned int last)
{
	struct pc98gdcfb_par *par = info->par;
	unsigned int y;

	first = min_t(unsigned int, first, GDC_HEIGHT);
	last = min_t(unsigned int, last, GDC_HEIGHT);
	if (first >= last)
		return;

	mutex_lock(&par->vram_lock);
	for (y = first; y < last; y++) {
		const u8 *src = par->shadow + y * GDC_WIDTH;
		u8 *mirror = par->hw_shadow + y * GDC_WIDTH;
		u32 byte_offset = y * 80;	/* 640 / 8 = 80 bytes per row per plane */

		/* Skip scanline if unchanged */
		if (memcmp(src, mirror, GDC_WIDTH) == 0)
			continue;

		gdc_convert_line(src,
				 par->plane_b + byte_offset,
				 par->plane_r + byte_offset,
				 par->plane_g + byte_offset,
				 par->plane_e + byte_offset);

		memcpy(mirror, src, GDC_WIDTH);
	}
	mutex_unlock(&par->vram_lock);
}

static void pc98gdcfb_damage_range(struct fb_info *info, off_t offset, size_t len)
{
	if (!len || offset < 0 || offset >= GDC_FB_SIZE)
		return;
	gdc_flush_rows(info, offset / GDC_WIDTH, DIV_ROUND_UP(offset + len, GDC_WIDTH));
}

static void pc98gdcfb_damage_area(struct fb_info *info, u32 x, u32 y, u32 width, u32 height)
{
	if (!width || !height || x >= GDC_WIDTH || y >= GDC_HEIGHT)
		return;
	gdc_flush_rows(info, y, y + min_t(u32, height, GDC_HEIGHT - y));
}

static void pc98gdcfb_deferred_io(struct fb_info *info, struct list_head *pagelist)
{
	struct fb_deferred_io_pageref *pageref;

	list_for_each_entry(pageref, pagelist, list) {
		size_t offset = min_t(size_t, pageref->offset, GDC_FB_SIZE);
		size_t end = min_t(size_t, offset + PAGE_SIZE, GDC_FB_SIZE);

		gdc_flush_rows(info, offset / GDC_WIDTH, DIV_ROUND_UP(end, GDC_WIDTH));
	}
}

static int pc98gdcfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	var->xres = GDC_WIDTH;
	var->yres = GDC_HEIGHT;
	var->xres_virtual = GDC_WIDTH;
	var->yres_virtual = GDC_HEIGHT;
	var->xoffset = 0;
	var->yoffset = 0;
	var->bits_per_pixel = GDC_BPP;
	var->grayscale = 0;

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

static int pc98gdcfb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	if (cmd == FBIO_WAITFORVSYNC) {
		/* Wait for active display (out of V-Blank) */
		while (inb(GDC_MASTER_STATUS) & 0x20)
			cpu_relax();
		/* Wait for start of next V-Blank */
		while (!(inb(GDC_MASTER_STATUS) & 0x20))
			cpu_relax();
		return 0;
	}
	return -EINVAL;
}

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(pc98gdcfb,
				   pc98gdcfb_damage_range,
				   pc98gdcfb_damage_area)

static const struct fb_ops pc98gdcfb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(pc98gdcfb),
	.fb_check_var	= pc98gdcfb_check_var,
	.fb_setcolreg	= pc98gdcfb_setcolreg,
	.fb_blank	= pc98gdcfb_blank,
	.fb_ioctl	= pc98gdcfb_ioctl,
};

static struct fb_deferred_io pc98gdcfb_defio = {
	.delay			= DIV_ROUND_UP(HZ, 60),
	.sort_pagereflist	= true,
	.deferred_io		= pc98gdcfb_deferred_io,
};

static struct platform_device *pc98gdcfb_device;

static int pc98gdcfb_probe(struct platform_device *pdev)
{
	struct pc98gdcfb_par *par;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(sizeof(*par), &pdev->dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	mutex_init(&par->vram_lock);
	spin_lock_init(&par->reg_lock);

	/* Map the 4 standard PC-98 VRAM bitplanes */
	par->plane_b = ioremap(VRAM_PLANE_B_PHYS, VRAM_PLANE_SIZE);
	par->plane_r = ioremap(VRAM_PLANE_R_PHYS, VRAM_PLANE_SIZE);
	par->plane_g = ioremap(VRAM_PLANE_G_PHYS, VRAM_PLANE_SIZE);
	par->plane_e = ioremap(VRAM_PLANE_E_PHYS, VRAM_PLANE_SIZE);
	if (!par->plane_b || !par->plane_r || !par->plane_g || !par->plane_e) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	par->shadow = vzalloc(GDC_FB_SIZE);
	par->hw_shadow = vzalloc(GDC_FB_SIZE);
	if (!par->shadow || !par->hw_shadow) {
		ret = -ENOMEM;
		goto err_vfree;
	}

	gdc_init_hardware(par);

	strscpy(info->fix.id, "PC98 GDC Planar", sizeof(info->fix.id));
	info->fix.smem_start = 0;
	info->fix.smem_len = GDC_FB_SIZE;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_PSEUDOCOLOR;
	info->fix.line_length = GDC_WIDTH;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = GDC_WIDTH;
	info->var.yres = GDC_HEIGHT;
	info->var.xres_virtual = GDC_WIDTH;
	info->var.yres_virtual = GDC_HEIGHT;
	info->var.bits_per_pixel = GDC_BPP;
	info->var.activate = FB_ACTIVATE_NOW;
	pc98gdcfb_check_var(&info->var, info);

	info->screen_buffer = par->shadow;
	info->screen_size = GDC_FB_SIZE;
	info->fbops = &pc98gdcfb_ops;
	info->flags = FBINFO_VIRTFB;
	info->fbdefio = &pc98gdcfb_defio;
	info->pseudo_palette = par->pseudo_palette;

	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto err_vfree;

	ret = fb_deferred_io_init(info);
	if (ret)
		goto err_cmap;

	ret = register_framebuffer(info);
	if (ret)
		goto err_defio;

	platform_set_drvdata(pdev, info);
	dev_info(&pdev->dev, "fb%d: PC-98 native GDC 16-color planar framebuffer (640x400)\n", info->node);
	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_vfree:
	vfree(par->hw_shadow);
	vfree(par->shadow);
err_unmap:
	if (par->plane_e) iounmap(par->plane_e);
	if (par->plane_g) iounmap(par->plane_g);
	if (par->plane_r) iounmap(par->plane_r);
	if (par->plane_b) iounmap(par->plane_b);
	framebuffer_release(info);
	return ret;
}

static void pc98gdcfb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct pc98gdcfb_par *par;

	if (!info)
		return;
	par = info->par;
	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);
	fb_dealloc_cmap(&info->cmap);
	vfree(par->hw_shadow);
	vfree(par->shadow);
	if (par->plane_e) iounmap(par->plane_e);
	if (par->plane_g) iounmap(par->plane_g);
	if (par->plane_r) iounmap(par->plane_r);
	if (par->plane_b) iounmap(par->plane_b);
	framebuffer_release(info);
}

static struct platform_driver pc98gdcfb_driver = {
	.probe		= pc98gdcfb_probe,
	.remove		= pc98gdcfb_remove,
	.driver		= {
		.name	= DRV_NAME,
	},
};

static int __init pc98gdcfb_init(void)
{
	int ret;

	ret = platform_driver_register(&pc98gdcfb_driver);
	if (ret)
		return ret;

	pc98gdcfb_device = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(pc98gdcfb_device)) {
		platform_driver_unregister(&pc98gdcfb_driver);
		return PTR_ERR(pc98gdcfb_device);
	}
	return 0;
}

static void __exit pc98gdcfb_exit(void)
{
	platform_device_unregister(pc98gdcfb_device);
	platform_driver_unregister(&pc98gdcfb_driver);
}

module_init(pc98gdcfb_init);
module_exit(pc98gdcfb_exit);

MODULE_AUTHOR("PC-9800 Lovers");
MODULE_DESCRIPTION("NEC PC-9800 native GDC (uPD7220) 16-color planar framebuffer driver");
MODULE_LICENSE("GPL");
