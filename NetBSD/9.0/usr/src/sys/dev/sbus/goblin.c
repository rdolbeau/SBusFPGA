/*	$NetBSD$ */

/*
 * Copyright (c) 2022 Romain Dolbeau <romain@dolbeau.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * color display (goblin) driver.
 *
 * Does not use VBL interrupts
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/tty.h>
#include <sys/conf.h>

#include <sys/bus.h>
#include <machine/autoconf.h>

#include <dev/sun/fbio.h>
#include <dev/sun/fbvar.h>

#include <dev/sun/btreg.h>
#include <dev/sun/btvar.h>
#include <dev/sbus/goblinreg.h>
#include <dev/sbus/goblinvar.h>

#if NWSDISPLAY > 0
#include <dev/wscons/wsconsio.h>
#include <dev/wsfont/wsfont.h>
#include <dev/rasops/rasops.h>

#include "opt_wsemul.h"
#endif

#include "ioconf.h"

static void	goblinunblank(device_t);
static void	goblinloadcmap(struct goblin_softc *, int, int);
static void	goblin_set_video(struct goblin_softc *, int);
static int	goblin_get_video(struct goblin_softc *);

static void jareth_copyrows(void *, int, int, int);
static void jareth_eraserows(void *, int, int, long int);

dev_type_open(goblinopen);
dev_type_close(goblinclose);
dev_type_ioctl(goblinioctl);
dev_type_mmap(goblinmmap);

const struct cdevsw goblin_cdevsw = {
	.d_open = goblinopen,
	.d_close = goblinclose,
	.d_read = noread,
	.d_write = nowrite,
	.d_ioctl = goblinioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = goblinmmap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER
};

/* frame buffer generic driver */
static struct fbdriver goblinfbdriver = {
	goblinunblank, goblinopen, goblinclose, goblinioctl, nopoll,
	goblinmmap, nokqfilter
};

static void gobo_setup_palette(struct goblin_softc *);

struct wsscreen_descr goblin_defaultscreen = {
	"std",
	0, 0,	/* will be filled in -- XXX shouldn't, it's global */
	NULL,		/* textops */
	8, 16,	/* font width/height */
	WSSCREEN_WSCOLORS,	/* capabilities */
	NULL	/* modecookie */
};


static int 	goblin_ioctl(void *, void *, u_long, void *, int, struct lwp *);
static paddr_t	goblin_mmap(void *, void *, off_t, int);
static void	goblin_init_screen(void *, struct vcons_screen *, int, long *);

static int	goblin_putcmap(struct goblin_softc *, struct wsdisplay_cmap *);
static int	goblin_getcmap(struct goblin_softc *, struct wsdisplay_cmap *);
static void goblin_init(struct goblin_softc *);
static void goblin_reset(struct goblin_softc *);

enum jareth_verbosity {
					   jareth_silent = 0,
					   jareth_verbose
};

static int jareth_scroll(struct goblin_softc *sc, enum jareth_verbosity verbose, int y0, int y1, int x0, int w, int n);
static int jareth_fill(struct goblin_softc *sc, enum jareth_verbosity verbose, int y0, int pat, int x0, int w, int n);

static void goblin_set_depth(struct goblin_softc *, int);

struct wsdisplay_accessops goblin_accessops = {
	goblin_ioctl,
	goblin_mmap,
	NULL,	/* alloc_screen */
	NULL,	/* free_screen */
	NULL,	/* show_screen */
	NULL, 	/* load_font */
	NULL,	/* pollc */
	NULL	/* scroll */
};

const struct wsscreen_descr *_goblin_scrlist[] = {
	&goblin_defaultscreen
};

struct wsscreen_list goblin_screenlist = {
	sizeof(_goblin_scrlist) / sizeof(struct wsscreen_descr *),
	_goblin_scrlist
};


extern const u_char rasops_cmap[768];

static struct vcons_screen gobo_console_screen;

void
goblinattach(struct goblin_softc *sc, const char *name, int isconsole)
{
	struct fbdevice *fb = &sc->sc_fb;
	struct wsemuldisplaydev_attach_args aa;
	struct rasops_info *ri = &gobo_console_screen.scr_ri;
	unsigned long defattr;
	volatile struct goblin_fbcontrol *fbc = sc->sc_fbc;

	/* 'boot' Jareth if present */
	if (sc->sc_has_jareth) {
		/* */
	}

	fb->fb_driver = &goblinfbdriver;
	fb->fb_pfour = NULL;
	fb->fb_type.fb_cmsize = 256;
	fb->fb_type.fb_size = sc->sc_size; //fb->fb_type.fb_height * fb->fb_linebytes;
	printf(": %s, %d x %d", name,
		fb->fb_type.fb_width, fb->fb_type.fb_height);

	/* disable VBL interrupts */
	fbc->vbl_mask = GOBOFB_VBL_MASK_OFF;

	/* Enable display in a supported mode */
	fbc->mode = GOBOFB_MODE_8BIT;
	fbc->videoctrl = GOBOFB_VIDEOCTRL_ON;

	if (isconsole) {
		printf(" (console)\n");
	} else
		printf("\n");

	fb_attach(fb, isconsole);

	sc->sc_width = fb->fb_type.fb_width;
	sc->sc_stride = fb->fb_type.fb_width;
	sc->sc_height = fb->fb_type.fb_height;

	wsfont_init();
	
	/* setup rasops and so on for wsdisplay */
	sc->sc_mode = WSDISPLAYIO_MODE_EMUL;
	sc->sc_opens = 0;

	vcons_init(&sc->sc_vd, sc, &goblin_defaultscreen, &goblin_accessops);
	sc->sc_vd.init_screen = goblin_init_screen;
	
	if(isconsole) {
		/* we mess with gobo_console_screen only once */
		goblin_set_depth(sc, 8);
		vcons_init_screen(&sc->sc_vd, &gobo_console_screen, 1,
						  &defattr);
		/* clear the screen */
		memset(sc->sc_fb.fb_pixels, (defattr >> 16) & 0xff,
			   sc->sc_stride * sc->sc_height);
		gobo_console_screen.scr_flags |= VCONS_SCREEN_IS_STATIC;
		
		goblin_defaultscreen.textops = &ri->ri_ops;
		goblin_defaultscreen.capabilities = ri->ri_caps;
		goblin_defaultscreen.nrows = ri->ri_rows;
		goblin_defaultscreen.ncols = ri->ri_cols;
		sc->sc_vd.active = &gobo_console_screen;
		
		wsdisplay_cnattach(&goblin_defaultscreen, ri, 0, 0, defattr);
		vcons_replay_msgbuf(&gobo_console_screen);
	} else {
		/* 
		 * we're not the console so we just clear the screen and don't 
		 * set up any sort of text display
		 */
		memset(sc->sc_fb.fb_pixels, (defattr >> 16) & 0xff,
			   sc->sc_stride * sc->sc_height);
	}
	
	/* Initialize the default color map. */
	gobo_setup_palette(sc);

	/* reset the HW cursor */
	{
		volatile struct goblin_fbcontrol *sc_fbc = sc->sc_fbc;
		sc_fbc->cursor_xy = 0xFFE0FFE0;
		sc_fbc->lut_addr = 0;
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0xFF; // bg color
		sc_fbc->cursor_lut = 0xFF;
		sc_fbc->cursor_lut = 0xFF;
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0; // fb color
		sc_fbc->cursor_lut = 0;
		sc_fbc->cursor_lut = 0;
	}

	aa.scrdata = &goblin_screenlist;
	aa.console = isconsole;
	aa.accessops = &goblin_accessops;
	aa.accesscookie = &sc->sc_vd;
	config_found(sc->sc_dev, &aa, wsemuldisplaydevprint);
}


int
goblinopen(dev_t dev, int flags, int mode, struct lwp *l)
{
	struct goblin_softc *sc = device_lookup_private(&goblin_cd,
							 minor(dev));
	int oldopens;
 
	if (sc == NULL)
		return (ENXIO);

	oldopens = sc->sc_opens++;

	if (oldopens == 0)	/* first open only */
		goblin_init(sc);
	
	return (0);
}

int
goblinclose(dev_t dev, int flags, int mode, struct lwp *l)
{
	struct goblin_softc *sc = device_lookup_private(&goblin_cd,
							 minor(dev));
	int opens;

	opens = --sc->sc_opens;
	if (sc->sc_opens < 0) /* should not happen... */
		opens = sc->sc_opens = 0;

	/*
	 * Restore video state to make the PROM happy, on last close.
	 */
	if (opens == 0) {
		goblin_reset(sc);
	}
	return (0);
}

int
goblinioctl(dev_t dev, u_long cmd, void *data, int flags, struct lwp *l)
{
	struct goblin_softc *sc = device_lookup_private(&goblin_cd,
							 minor(dev));
	struct fbgattr *fba;
	int error;

	switch (cmd) {

	case FBIOGTYPE:
		*(struct fbtype *)data = sc->sc_fb.fb_type;
		break;

	case FBIOGATTR:
		fba = (struct fbgattr *)data;
		fba->real_type = sc->sc_fb.fb_type.fb_type;
		fba->owner = 0;		/* XXX ??? */
		fba->fbtype = sc->sc_fb.fb_type;
		fba->sattr.flags = 0;
		fba->sattr.emu_type = sc->sc_fb.fb_type.fb_type;
		fba->sattr.dev_specific[0] = -1;
		fba->emu_types[0] = sc->sc_fb.fb_type.fb_type;
		fba->emu_types[1] = -1;
		break;

	case FBIOGETCMAP:
#define p ((struct fbcmap *)data)
		return (bt_getcmap(p, &sc->sc_cmap, 256, 1));

	case FBIOPUTCMAP:
		/* copy to software map */
		error = bt_putcmap(p, &sc->sc_cmap, 256, 1);
		if (error)
			return (error);
		/* now blast them into the chip */
		goblinloadcmap(sc, p->index, p->count);
#undef p
		break;

	case FBIOGVIDEO:
		*(int *)data = goblin_get_video(sc);
		break;

	case FBIOSVIDEO:
		goblin_set_video(sc, *(int *)data);
		break;

	case GOBLIN_SET_PIXELMODE: {
		int depth = *(int *)data;

		if (sc->sc_mode == WSDISPLAYIO_MODE_EMUL)
			return EINVAL;

		goblin_set_depth(sc, depth);
		}
		break;

	default:
		return (ENOTTY);
	}
	return (0);
}

/*
 * Undo the effect of an FBIOSVIDEO that turns the video off.
 */
static void
goblinunblank(device_t self)
{
	struct goblin_softc *sc = device_private(self);

#if NWSDISPLAY > 0
	if (sc->sc_mode != WSDISPLAYIO_MODE_EMUL) {
		goblin_set_depth(sc, 8);
		vcons_redraw_screen(sc->sc_vd.active);
		sc->sc_mode = WSDISPLAYIO_MODE_EMUL;
	}
#endif
	goblin_set_video(sc, 1);
}

static void
goblin_set_video(struct goblin_softc *sc, int enable)
{

	if (enable)
		sc->sc_fbc->videoctrl = GOBOFB_VIDEOCTRL_ON;
	else
		sc->sc_fbc->videoctrl = GOBOFB_VIDEOCTRL_OFF;
}

static int
goblin_get_video(struct goblin_softc *sc)
{

	return (sc->sc_fbc->videoctrl == GOBOFB_VIDEOCTRL_ON);
}

/*
 * Load a subset of the current (new) colormap into the DAC.
 * Pretty much the same as the Brooktree DAC in the cg6
 */
static void
goblinloadcmap(struct goblin_softc *sc, int start, int ncolors)
{
	volatile struct goblin_fbcontrol *sc_fbc = sc->sc_fbc;
	u_int *ip, i;
	int count;

	ip = &sc->sc_cmap.cm_chip[BT_D4M3(start)];	/* start/4 * 3 */
	count = BT_D4M3(start + ncolors - 1) - BT_D4M3(start) + 3;
	sc_fbc->lut_addr = BT_D4M4(start) & 0xFF;
	while (--count >= 0) {
		i = *ip++;
		/* hardware that makes one want to pound boards with hammers */
		/* ^^^ from the cg6, need to rework this on the HW and SW side ... */
		sc_fbc->lut = (i >> 24) & 0xff;
		sc_fbc->lut = (i >> 16) & 0xff;
		sc_fbc->lut = (i >>  8) & 0xff;
		sc_fbc->lut = (i      ) & 0xff;
	}
}

/*
 * Return the address that would map the given device at the given
 * offset, allowing for the given protection, or return -1 for error.
 * 'inspired' by the cg6 code
 */
#define	GOBLIN_USER_FBC	      0x70000000
#define	JARETH_USER_REG	      0x70001000
#define	GOBLIN_USER_RAM	      0x70016000
typedef enum {
			  goblin_bank_fbc,
			  goblin_bank_fb,
			  jareth_bank_reg,
} gobo_reg_bank;
struct mmo {
	u_long	mo_uaddr;	/* user (virtual) address */
	u_long	mo_size;	/* size, or 0 for video ram size */
	gobo_reg_bank	mo_reg_bank;	/* which bank to map */
};
paddr_t
goblinmmap(dev_t dev, off_t off, int prot)
{
	struct goblin_softc *sc = device_lookup_private(&goblin_cd,
							 minor(dev));
	struct mmo *mo;
	u_int u, sz, flags;
	static struct mmo mmo[] = {
		{ GOBLIN_USER_RAM,       0, goblin_bank_fb },
		{ GOBLIN_USER_FBC,       1, goblin_bank_fbc },
		{ JARETH_USER_REG,       1, jareth_bank_reg },
	};

	/* device_printf(sc->sc_dev, "requiesting %llx with %d\n", off, prot); */
	
#define NMMO (sizeof mmo / sizeof *mmo)
	if (off & PGOFSET)
		panic("goblinmmap");
	if (off < 0)
		return (-1);
	/*
	 * Entries with size 0 map video RAM (i.e., the size in fb data).
	 *
	 * Since we work in pages, the fact that the map offset table's
	 * sizes are sometimes bizarre (e.g., 1) is effectively ignored:
	 * one byte is as good as one page.
	 */
	for (mo = mmo; mo < &mmo[NMMO]; mo++) {
		if ((u_long)off < mo->mo_uaddr)
			continue;
		u = off - mo->mo_uaddr;
		if (mo->mo_size == 0) {
			flags = BUS_SPACE_MAP_LINEAR |
				BUS_SPACE_MAP_PREFETCHABLE;
			sz = sc->sc_size;
		} else {
			flags = BUS_SPACE_MAP_LINEAR;
			sz = mo->mo_size;
		}
		if (u < sz) {
			switch (mo->mo_reg_bank) {
			case goblin_bank_fb:
				return (bus_space_mmap(sc->sc_bustag,
									   sc->sc_fb_paddr, u,
									   prot, flags));
			case goblin_bank_fbc:
				return (bus_space_mmap(sc->sc_bustag,
									   sc->sc_reg_fbc_paddr, u,
									   prot, flags));
			case jareth_bank_reg:
				return (bus_space_mmap(sc->sc_bustag,
									   sc->sc_jareth_reg_paddr, u,
									   prot, flags));
			}
		}
	}

	device_printf(sc->sc_dev, "Jareth mmap() (from X11, presumably) failed; 0x%08llx, 0x%08x\n", off, prot);
	
	return (-1);
}

static void
gobo_setup_palette(struct goblin_softc *sc)
{
	int i, j;

	j = 0;
	for (i = 0; i < 256; i++) {
		sc->sc_cmap.cm_map[i][0] = rasops_cmap[j];
		j++;
		sc->sc_cmap.cm_map[i][1] = rasops_cmap[j];
		j++;
		sc->sc_cmap.cm_map[i][2] = rasops_cmap[j];
		j++;
	}
	goblinloadcmap(sc, 0, 256);
}

int
goblin_ioctl(void *v, void *vs, u_long cmd, void *data, int flag,
	struct lwp *l)
{
	/* we'll probably need to add more stuff here */
	struct vcons_data *vd = v;
	struct goblin_softc *sc = vd->cookie;
	struct wsdisplay_fbinfo *wdf;
	struct vcons_screen *ms = sc->sc_vd.active;
	struct rasops_info *ri = &ms->scr_ri;
	switch (cmd) {
		case WSDISPLAYIO_GTYPE:
			*(u_int *)data = WSDISPLAY_TYPE_SUNTCX;
			return 0;
		case WSDISPLAYIO_GINFO:
			wdf = (void *)data;
			wdf->height = ri->ri_height;
			wdf->width = ri->ri_width;
			wdf->depth = 8;
			wdf->cmsize = 256;
			return 0;

		case WSDISPLAYIO_GETCMAP:
			return goblin_getcmap(sc, 
			    (struct wsdisplay_cmap *)data);
		case WSDISPLAYIO_PUTCMAP:
			return goblin_putcmap(sc, 
			    (struct wsdisplay_cmap *)data);

		case WSDISPLAYIO_SMODE:
			{
				int new_mode = *(int*)data;
				if (new_mode != sc->sc_mode)
				{
					sc->sc_mode = new_mode;
					if(new_mode == WSDISPLAYIO_MODE_EMUL)
					{
						gobo_setup_palette(sc);
						vcons_redraw_screen(ms);
					}
				}
			}
			return 0;
		case WSDISPLAYIO_GET_FBINFO:
			{
				struct wsdisplayio_fbinfo *fbi = data;

				return wsdisplayio_get_fbinfo(&ms->scr_ri, fbi);
			}
	}
	return EPASSTHROUGH;
}

/* for wsdisplay, just map usable memory */
paddr_t
goblin_mmap(void *v, void *vs, off_t offset, int prot)
{
	struct vcons_data *vd = v;
	struct goblin_softc *sc = vd->cookie;

	if (offset < 0) return -1;
	if (offset >= sc->sc_fb.fb_type.fb_size)
		return -1;

	return bus_space_mmap(sc->sc_bustag,
		sc->sc_fb_paddr, offset,
		prot, BUS_SPACE_MAP_LINEAR);
}

static int
goblin_putcmap(struct goblin_softc *sc, struct wsdisplay_cmap *cm)
{
	u_int index = cm->index;
	u_int count = cm->count;
	int error,i;
	if (index >= 256 || count > 256 || index + count > 256)
		return EINVAL;

	for (i = 0; i < count; i++)
	{
		error = copyin(&cm->red[i],
		    &sc->sc_cmap.cm_map[index + i][0], 1);
		if (error)
			return error;
		error = copyin(&cm->green[i],
		    &sc->sc_cmap.cm_map[index + i][1],
		    1);
		if (error)
			return error;
		error = copyin(&cm->blue[i],
		    &sc->sc_cmap.cm_map[index + i][2], 1);
		if (error)
			return error;
	}
	goblinloadcmap(sc, index, count);

	return 0;
}

static int
goblin_getcmap(struct goblin_softc *sc, struct wsdisplay_cmap *cm)
{
	u_int index = cm->index;
	u_int count = cm->count;
	int error,i;

	if (index >= 256 || count > 256 || index + count > 256)
		return EINVAL;

	for (i = 0; i < count; i++)
	{
		error = copyout(&sc->sc_cmap.cm_map[index + i][0],
		    &cm->red[i], 1);
		if (error)
			return error;
		error = copyout(&sc->sc_cmap.cm_map[index + i][1],
		    &cm->green[i], 1);
		if (error)
			return error;
		error = copyout(&sc->sc_cmap.cm_map[index + i][2],
		    &cm->blue[i], 1);
		if (error)
			return error;
	}

	return 0;
}

void
goblin_init_screen(void *cookie, struct vcons_screen *scr,
    int existing, long *defattr)
{
	struct goblin_softc *sc = cookie;
	struct rasops_info *ri = &scr->scr_ri;

	scr->scr_flags |= VCONS_NO_COPYCOLS;

	ri->ri_depth = 8;
	ri->ri_width = sc->sc_width;
	ri->ri_height = sc->sc_height;
	ri->ri_stride = sc->sc_stride;
	ri->ri_flg = RI_CENTER;

	ri->ri_bits = sc->sc_fb.fb_pixels;

	rasops_init(ri, 0, 0);
	ri->ri_caps = WSSCREEN_WSCOLORS | WSSCREEN_REVERSE;
	rasops_reconfig(ri, sc->sc_height / ri->ri_font->fontheight,
		    sc->sc_width / ri->ri_font->fontwidth);

	ri->ri_hw = scr;
	if (sc->sc_has_jareth) {
		device_printf(sc->sc_dev, "Jareth present\n");
		ri->ri_ops.copyrows = jareth_copyrows;
		ri->ri_ops.eraserows = jareth_eraserows;
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_SRC_PTR, 0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_PTR, 0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_OP, 0x3); // GXcopy by default
		device_printf(sc->sc_dev, "Jareth console acceleration enabled\n");
		/* uint32_t status = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_STATUS); */
		/* uint32_t resv0 = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_RESV0); */
		/* device_printf(sc->sc_dev, "Jareth sanity check: 0x%08x, 0x%08x\n", status, resv0); */
	}
}

static void
goblin_set_depth(struct goblin_softc *sc, int depth)
{
	if (sc->sc_depth == depth)
		return;

	switch (depth) {
		case 8:
			sc->sc_fbc->mode = GOBOFB_MODE_8BIT;
			sc->sc_depth = 8;
			break;
		case 32:
			sc->sc_fbc->mode = GOBOFB_MODE_24BIT;
			sc->sc_depth = 32;
			break;
		default:
			printf("%s: can't change to depth %d\n",
			    device_xname(sc->sc_dev), depth);
	}
}

/* Initialize the framebuffer, storing away useful state for later reset */
static void
goblin_init(struct goblin_softc *sc)
{
	goblin_set_depth(sc, 32);
}

static void
/* Restore the state saved on goblin_init */
goblin_reset(struct goblin_softc *sc)
{
	goblin_set_depth(sc, 8);
	// X11 is supposed to clean up its mess, but just in case...
	if (sc->sc_has_jareth) {
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_SRC_PTR, 0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_PTR, 0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_OP, 0x3); // GXcopy by default
	}
}

static void
jareth_copyrows(void *cookie, int src, int dst, int n)
{
	struct rasops_info *ri = cookie;
	struct vcons_screen *scr = ri->ri_hw;
	struct goblin_softc *sc = scr->scr_cookie;
	
	if (dst == src)
		return;
	if (src < 0) {
		n += src;
		src = 0;
	}
	if (src+n > ri->ri_rows)
		n = ri->ri_rows - src;
	if (dst < 0) {
		n += dst;
		dst = 0;
	}
	if (dst+n > ri->ri_rows)
		n = ri->ri_rows - dst;
	if (n <= 0)
		return;
	
	n *= ri->ri_font->fontheight;
	src *= ri->ri_font->fontheight;
	dst *= ri->ri_font->fontheight;
	
	int x0 = ri->ri_xorigin;
	int y0 = ri->ri_yorigin + src;
	//int x1 = ri->ri_xorigin + ri->ri_emuwidth - 1;
	/* int y1 = ri->ri_yorigin + src + n - 1; */
	/* int x2 = ri->ri_xorigin; */
	int y2 = ri->ri_yorigin + dst;
	/* int x3 = ri->ri_xorigin + ri->ri_emuwidth - 1; */
	/* int y3 = ri->ri_yorigin + dst + n - 1; */

	jareth_scroll(sc, jareth_silent, y0, y2, x0, ri->ri_emuwidth, n);
}

static void
jareth_eraserows(void *cookie, int row, int n, long int attr)
{
	struct rasops_info *ri = cookie;
	struct vcons_screen *scr = ri->ri_hw;
	struct goblin_softc *sc = scr->scr_cookie;
	uint32_t pat;

	if (row < 0) {
		n += row;
		row = 0;
	}
	if (row+n > ri->ri_rows)
		n = ri->ri_rows - row;
	if (n <= 0)
		return;

	pat = ri->ri_devcmap[(attr >> 16) & 0xff];
	pat |= pat << 8;
	pat |= pat << 16;
	
	if ((n == ri->ri_rows) && (ri->ri_flg & RI_FULLCLEAR)) {
		(void)jareth_fill(sc, jareth_silent, 0, pat, 0, ri->ri_width, ri->ri_height);
	} else {
		row *= ri->ri_font->fontheight;
		(void)jareth_fill(sc, jareth_silent, ri->ri_yorigin + row, pat, ri->ri_xorigin, ri->ri_emuwidth, n * ri->ri_font->fontheight);
	}
}

static uint32_t wait_for_jareth(struct goblin_softc *sc) {
	uint32_t status;
	int cnt = 0;
	while ((((status = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_STATUS)) & (1<<WORK_IN_PROGRESS_BIT)) != 0) && (cnt < 500)) {
		cnt += 2;
		delay(cnt);
	}
	return status;
}

static int jareth_scroll(struct goblin_softc *sc, enum jareth_verbosity verbose, int y0, int y1, int x0, int w, int n) {
	uint32_t status;
	
	status = wait_for_jareth(sc);

	if ((status & (1<<WORK_IN_PROGRESS_BIT)) == 0) {
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_SRC_STRIDE, sc->sc_stride);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_STRIDE, sc->sc_stride);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_WIDTH, w);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_HEIGHT, n);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_SRC_X, x0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_X, x0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_SRC_Y, y0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_Y, y1);

		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_CMD, (1<<DO_BLIT_BIT));
	   
		status = wait_for_jareth(sc);
	}
	
	/* if ((status & (1<<WORK_IN_PROGRESS_BIT)) != 0) { */
	/* 	device_printf(sc->sc_dev, "Jareth seems busy/locked (0x%08x)\n", status); */
	/* } */
	
	return 0;
}

static int jareth_fill(struct goblin_softc *sc, enum jareth_verbosity verbose, int y0, int pat, int x0, int w, int n) {
	uint32_t status;
	
	status = wait_for_jareth(sc);
	
	if ((status & (1<<WORK_IN_PROGRESS_BIT)) == 0) {
		//bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_SRC_STRIDE, sc->sc_stride);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_STRIDE, sc->sc_stride);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_WIDTH, w);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_HEIGHT, n);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_X, x0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_DST_Y, y0);
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_FGCOLOR, pat);
	   
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_jareth, GOBOFB_ACCEL_REG_CMD, (1<<DO_FILL_BIT));
	   
		status = wait_for_jareth(sc);
	}
	
	/* if ((status & (1<<WORK_IN_PROGRESS_BIT)) != 0) { */
	/* 	device_printf(sc->sc_dev, "Jareth seems busy/locked (0x%08x)\n", status); */
	/* } */
	   
	return 0;
}
