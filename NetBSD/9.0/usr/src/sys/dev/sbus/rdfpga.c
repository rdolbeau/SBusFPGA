/*	$NetBSD$ */

/*-
 * Copyright (c) 2020 Romain Dolbeau <romain@dolbeau.org>
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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/malloc.h>

#include <sys/bus.h>
#include <machine/autoconf.h>
#include <sys/cpu.h>
#include <sys/conf.h>
#include <sys/mbuf.h>
#include <sys/ioccom.h>

#include <dev/sbus/sbusvar.h>

#include <dev/sbus/rdfpga.h>

#include <machine/param.h>

int	rdfpga_print(void *, const char *);
int	rdfpga_match(device_t, cfdata_t, void *);
void	rdfpga_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(rdfpga, sizeof(struct rdfpga_softc),
    rdfpga_match, rdfpga_attach, NULL, NULL);

dev_type_open(rdfpga_open);
dev_type_close(rdfpga_close);
dev_type_ioctl(rdfpga_ioctl);
dev_type_write(rdfpga_write);

const struct cdevsw rdfpga_cdevsw = {
	.d_open = rdfpga_open,
	.d_close = rdfpga_close,
	.d_read = noread,
	.d_write = rdfpga_write,
	.d_ioctl = rdfpga_ioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = 0
};

static int rdfpga_wait_aes_ready(struct rdfpga_softc *sc) {
  u_int32_t ctrl;
  int ctr;
  
  ctr = 0;
  while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL)) != 0) &&
	 (ctr < 5)) {
    delay(1);
    ctr ++;
  }

  if (ctrl)
    return EBUSY;

  return 0;
}

/* should split GCM/AES */
static int rdfpga_wait_dma_ready(struct rdfpga_softc *sc, const int count) {
  u_int32_t ctrl;
  int ctr;
  
  ctr = 0;
  while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_GCMDMA_CTRL)) != 0) &&
	 (ctr < count)) {
    delay(1);
    ctr ++;
  }

  if (ctrl)
    return EBUSY;
  
  ctr = 0;
  while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AESDMA_CTRL)) != 0) &&
	 (ctr < count)) {
    delay(1);
    ctr ++;
  }

  if (ctrl)
    return EBUSY;
  
  ctr = 0;
  while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AESDMAW_CTRL)) != 0) &&
	 (ctr < count)) {
    delay(1);
    ctr ++;
  }

  if (ctrl)
    return EBUSY;

  return 0;
}


extern struct cfdriver rdfpga_cd;

struct rdfpga_128bits {
	uint32_t x[4];
};
struct rdfpga_128bits_alt {
	uint64_t x[2];
};
struct rdfpga_256bits {
	uint64_t x[4];
};

#define RDFPGA_WC   _IOW(0, 1, struct rdfpga_128bits)
#define RDFPGA_WH   _IOW(0, 2, struct rdfpga_128bits)
#define RDFPGA_WI   _IOW(0, 3, struct rdfpga_128bits)
#define RDFPGA_RC   _IOR(0, 4, struct rdfpga_128bits)
#define RDFPGA_WL   _IOW(0, 5, uint32_t)

#define RDFPGA_AESWK   _IOW(0, 10, struct rdfpga_128bits)
#define RDFPGA_AESWD   _IOW(0, 11, struct rdfpga_128bits)
#define RDFPGA_AESRO   _IOR(0, 12, struct rdfpga_128bits)
#define RDFPGA_AESWK256   _IOW(0, 13, struct rdfpga_256bits)
#define RDFPGA_AESGCMF   _IOWR(0, 14, struct rdfpga_128bits)
#define RDFPGA_AESGCMN   _IOR(0, 15, struct rdfpga_128bits)

#if 0
#define RDFPGA_AESRD   _IOR(0, 100, struct rdfpga_128bits) /* fixme: remove */
#endif

int
rdfpga_ioctl (dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
        struct rdfpga_softc *sc = device_lookup_private(&rdfpga_cd, minor(dev));
	struct rdfpga_128bits_alt *bits = (struct rdfpga_128bits_alt*)data;
	struct rdfpga_256bits *bits256 = (struct rdfpga_256bits*)data;
        int err = 0, i;
	uint32_t ctrl;

        switch (cmd) {
	  /* GCM */
        case RDFPGA_WC:
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_C + (i*8)), bits->x[i] );
                break;
        case RDFPGA_WH:
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_H + (i*8)), bits->x[i] );
                break;
        case RDFPGA_WI:
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_I + (i*8)), bits->x[i] );
                break;
        case RDFPGA_RC:
		for (i = 0 ; i < 2 ; i++)
			bits->x[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_C + (i*8)));
                break;
        case RDFPGA_WL:
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_LED, *(uint32_t*)data);
                break;
	  /* AES */
        case RDFPGA_AESWK:
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), bits->x[i] );
		sc->aes_key_refresh = 0xFFFF;
		sc->aes_key_bits = 0;
                break;
        case RDFPGA_AESWK256:
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 4 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), bits256->x[i] );
		sc->aes_key_refresh = 0xFFFF;
		sc->aes_key_bits = 1;
                break;
        case RDFPGA_AESWD:
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), bits->x[i] );
		ctrl = RDFPGA_MASK_AES128_START;
		if (sc->aes_key_refresh != 0x8000) {
		  ctrl |= RDFPGA_MASK_AES128_NEWKEY;
		  sc->aes_key_refresh = 0x8000;
		}
		if (sc->aes_key_bits == 1) {
		  ctrl |= RDFPGA_MASK_AES128_AES256;
		}
	        bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
                break;
        case RDFPGA_AESRO:
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
			bits->x[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
                break;
        case RDFPGA_AESGCMF:
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
		  bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), bits->x[i] );
		ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_GCMPOSTINC;
		if (sc->aes_key_refresh != 0x8000) {
		  ctrl |= RDFPGA_MASK_AES128_NEWKEY;
		  sc->aes_key_refresh = 0x8000;
		}
		if (sc->aes_key_bits == 1) {
		  ctrl |= RDFPGA_MASK_AES128_AES256;
		}
	        bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
			bits->x[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
                break;
        case RDFPGA_AESGCMN:
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_GCMPOSTINC;
		if (sc->aes_key_refresh != 0x8000) {
		  ctrl |= RDFPGA_MASK_AES128_NEWKEY;
		  sc->aes_key_refresh = 0x8000;
		}
		if (sc->aes_key_bits == 1) {
		  ctrl |= RDFPGA_MASK_AES128_AES256;
		}
	        bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
			bits->x[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
                break;
#if 0
        case RDFPGA_AESRD: /* fixme: disable */
		if ((err = rdfpga_wait_aes_ready(sc)) != 0)
		  return err;
		for (i = 0 ; i < 2 ; i++)
			bits->x[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)));
                break;
#endif
        default:
                err = EINVAL;
                break;
        }
        return(err);
}


int
rdfpga_open(dev_t dev, int flags, int mode, struct lwp *l)
{
#if 0
        struct rdfpga_softc *sc = device_lookup_private(&rdfpga_cd, minor(dev));
	int i;
	for (i = 0 ; i < 4 ; i++)
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_C + (i*4)), 0);
	for (i = 0 ; i < 4 ; i++)
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_H + (i*4)), 0);
	for (i = 0 ; i < 4 ; i++)
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_I + (i*4)), 0);
#endif
	return (0);
}

int
rdfpga_close(dev_t dev, int flags, int mode, struct lwp *l)
{
#if 0
        struct rdfpga_softc *sc = device_lookup_private(&rdfpga_cd, minor(dev));
	int i;
	for (i = 0 ; i < 4 ; i++)
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_C + (i*4)), 0);
	for (i = 0 ; i < 4 ; i++)
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_H + (i*4)), 0);
	for (i = 0 ; i < 4 ; i++)
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_I + (i*4)), 0);
#endif
	return (0);
}

int
rdfpga_write(dev_t dev, struct uio *uio, int flags)
{
        struct rdfpga_softc *sc = device_lookup_private(&rdfpga_cd, minor(dev));
	int error = 0, ctr = 0, res, oldres;
	
	/* aprint_normal_dev(sc->sc_dev, "dma uio: %zu in %d\n", uio->uio_resid, uio->uio_iovcnt); */

	if (uio->uio_resid >= 16 && uio->uio_iovcnt == 1) {
	  bus_dma_segment_t segs;
	  int rsegs;
	  if (bus_dmamem_alloc(sc->sc_dmatag, RDFPGA_VAL_DMA_MAX_SZ, 64, 64, &segs, 1, &rsegs, BUS_DMA_NOWAIT | BUS_DMA_STREAMING)) {
	     aprint_error_dev(sc->sc_dev, "cannot allocate DVMA memory");
	    return ENXIO;
	  }
	  /* else { */
	  /*   aprint_normal_dev(sc->sc_dev, "dmamem alloc: %d\n", rsegs); */
	  /* } */

	  void* kvap;
	  if (bus_dmamem_map(sc->sc_dmatag, &segs, 1, RDFPGA_VAL_DMA_MAX_SZ, &kvap, BUS_DMA_NOWAIT)) {
	    aprint_error_dev(sc->sc_dev, "cannot allocate DVMA address");
	    bus_dmamem_free(sc->sc_dmatag, &segs, 1);
	    return ENXIO;
	  }
	  /* else { */
	  /*   aprint_normal_dev(sc->sc_dev, "dmamem map: %p\n", kvap); */
	  /* } */
	  
	  if (bus_dmamap_load(sc->sc_dmatag, sc->sc_dmamap, kvap, RDFPGA_VAL_DMA_MAX_SZ, /* kernel space */ NULL,
	  		      BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE)) {
	    aprint_error_dev(sc->sc_dev, "cannot load dma map");
	    bus_dmamem_unmap(sc->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ);
	    bus_dmamem_free(sc->sc_dmatag, &segs, 1);
	    return ENXIO;
	  }
	  /* else { */
	  /*   aprint_normal_dev(sc->sc_dev, "dmamap: %lu %lu %d\n", sc->sc_dmamap->dm_maxsegsz, sc->sc_dmamap->dm_mapsize, sc->sc_dmamap->dm_nsegs); */
	  /* } */

	while (!error && uio->uio_resid >= 16 && uio->uio_iovcnt == 1) {
	  uint64_t ctrl;
	  uint32_t nblock = uio->uio_resid/16;
	  if (nblock > 4096)
	    nblock = 4096;

	  /* no implemented on sparc ? */
	  /* if (bus_dmamap_load_uio(sc->sc_dmatag, sc->sc_dmamap, uio, BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE)) { */
	  /*   aprint_error_dev(sc->sc_dev, "cannot allocate DVMA address"); */
	  /*   return ENXIO; */
	  /* } else { */
	  /*   aprint_normal_dev(sc->sc_dev, "dma: %lu %lu %d\n", sc->sc_dmamap->dm_maxsegsz, sc->sc_dmamap->dm_mapsize, sc->sc_dmamap->dm_nsegs); */
	  /* } */
	  
	  /* uint64_t buf[4]; */
	  /* if ((error = uiomove(buf, 32, uio)) != 0) */
	  /*   break; */
	  
	  /* if (bus_dmamap_load(sc->sc_dmatag, sc->sc_dmamap, buf, 32, /\* kernel space *\/ NULL, */
	  /* 		      BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE)) { */
	  /*   aprint_error_dev(sc->sc_dev, "cannot allocate DVMA address"); */
	  /*   return ENXIO; */
	  /* } else { */
	  /*   aprint_normal_dev(sc->sc_dev, "dma: %lu %lu %d\n", sc->sc_dmamap->dm_maxsegsz, sc->sc_dmamap->dm_mapsize, sc->sc_dmamap->dm_nsegs); */
	  /* } */

	  /* aprint_normal_dev(sc->sc_dev, "dmamem about to alloc for %d blocks...\n", nblock); */
	 

	  if ((error = uiomove(kvap, nblock*16, uio)) != 0)
	    break;
	  
	  /* aprint_normal_dev(sc->sc_dev, "uimove: left %zu in %d\n", uio->uio_resid, uio->uio_iovcnt); */
	  
	  bus_dmamap_sync(sc->sc_dmatag, sc->sc_dmamap, 0, nblock*16, BUS_DMASYNC_PREWRITE);
	  
	  /* aprint_normal_dev(sc->sc_dev, "dma: synced\n"); */

	  ctrl = ((uint64_t)(RDFPGA_MASK_DMA_CTRL_START | ((nblock-1) & RDFPGA_MASK_DMA_CTRL_BLKCNT))) | ((uint64_t)(uint32_t)(sc->sc_dmamap->dm_segs[0].ds_addr)) << 32;
	  
	  /* aprint_normal_dev(sc->sc_dev, "trying 0x%016llx\n", ctrl); */

	  bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCMDMA_ADDR), ctrl);
	  
	  /* aprint_normal_dev(sc->sc_dev, "dma: cmd sent\n"); */

	  res = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCMDMA_CTRL));
	  do {
	    ctr ++;
	    delay(2);
	    oldres = res;
	    res = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCMDMA_CTRL));
	  } while ((res & RDFPGA_MASK_DMA_CTRL_START) && !(res & RDFPGA_MASK_DMA_CTRL_ERR) && (res != oldres) && (ctr < 10000));

	  if ((res & RDFPGA_MASK_DMA_CTRL_START) || (res & RDFPGA_MASK_DMA_CTRL_ERR)) {
	    aprint_error_dev(sc->sc_dev, "read 0x%08x (%d try)\n", res, ctr);
	    error = ENXIO;
	  }

	  /* if (sc->sc_dmamap->dm_nsegs > 0) { */
	  bus_dmamap_sync(sc->sc_dmatag, sc->sc_dmamap, 0, nblock*16, BUS_DMASYNC_POSTWRITE);
	  /* aprint_normal_dev(sc->sc_dev, "dma: synced (2)\n"); */
	}
	
	  
	  bus_dmamap_unload(sc->sc_dmatag, sc->sc_dmamap);
	  /* aprint_normal_dev(sc->sc_dev, "dma: unloaded\n"); */
	  
	  bus_dmamem_unmap(sc->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ);
	  /* aprint_normal_dev(sc->sc_dev, "dma: unmapped\n"); */
	  
	  bus_dmamem_free(sc->sc_dmatag, &segs, 1);
	  /* aprint_normal_dev(sc->sc_dev, "dma: freed\n"); */
	}

	/* if (uio->uio_resid > 0) */
	/*   aprint_normal_dev(sc->sc_dev, "%zd bytes left after DMA\n", uio->uio_resid); */
	
	while (!error && uio->uio_resid > 0) {
		uint64_t bp[2] = {0, 0};
		size_t len = uimin(16, uio->uio_resid);

		if ((error = uiomove(bp, len, uio)) != 0)
			break;

		bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_I + 0), bp[0]);
		bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_GCM_I + 8), bp[1]);
	}

	return (error);
}

int
rdfpga_print(void *aux, const char *busname)
{

	sbus_print(aux, busname);
	return (UNCONF);
}

int
rdfpga_match(device_t parent, cfdata_t cf, void *aux)
{
	struct sbus_attach_args *sa = (struct sbus_attach_args *)aux;

	return (strcmp("RDOL,cryptoengine", sa->sa_name) == 0);
}

static void rdfpga_crypto_init(device_t self, struct rdfpga_softc *sc);

/*
 * Attach all the sub-devices we can find
 */
void
rdfpga_attach(device_t parent, device_t self, void *aux)
{
	struct sbus_attach_args *sa = aux;
	struct rdfpga_softc *sc = device_private(self);
	struct sbus_softc *sbsc = device_private(parent);
	int node;
	int sbusburst;
	int i;
	/* bus_dma_tag_t	dt = sa->sa_dmatag; */

	sc->sc_bustag = sa->sa_bustag;
	sc->sc_dmatag = sa->sa_dmatag;
		
	sc->sc_dev = self;

	if (sbus_bus_map(sc->sc_bustag, sa->sa_slot, sa->sa_offset, sa->sa_size,
			 BUS_SPACE_MAP_LINEAR, &sc->sc_bhregs) != 0) {
		aprint_error(": cannot map registers\n");
		return;
	}

	//sc->sc_buffer = bus_space_vaddr(sc->sc_bustag, sc->sc_bhregs);
	sc->sc_bufsiz = sa->sa_size;

	node = sc->sc_node = sa->sa_node;

	/*
	 * Get transfer burst size from PROM
	 */
	sbusburst = sbsc->sc_burst;
	if (sbusburst == 0)
		sbusburst = SBUS_BURST_32 - 1; /* 1->16 */

	sc->sc_burst = prom_getpropint(node, "burst-sizes", -1);
	if (sc->sc_burst == -1)
		/* take SBus burst sizes */
		sc->sc_burst = sbusburst;

	/* Clamp at parent's burst sizes */
	sc->sc_burst &= sbusburst;

	aprint_normal("\n");
	aprint_normal_dev(self, "nid 0x%x, bustag %p, burst 0x%x (parent 0x%0x)\n",
			  sc->sc_node,
			  sc->sc_bustag,
			  sc->sc_burst,
			  sbsc->sc_burst);

	/* change blink pattern to marching 2 */
	
	bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_LED , 0xc0300c03);

	/* DMA */

	/* Allocate a dmamap */
	if (bus_dmamap_create(sc->sc_dmatag, RDFPGA_VAL_DMA_MAX_SZ, 1, RDFPGA_VAL_DMA_MAX_SZ, 0, BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &sc->sc_dmamap) != 0) {
		aprint_error_dev(self, ": DMA map create failed\n");
	} else {
		aprint_normal_dev(self, "dmamap: %lu %lu %d (%p)\n", sc->sc_dmamap->dm_maxsegsz, sc->sc_dmamap->dm_mapsize, sc->sc_dmamap->dm_nsegs, sc->sc_dmatag->_dmamap_load);
	}

	for (i = 0 ; i < 4 ; i++)
	  bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), 0ull);
	sc->aes_key_refresh = 0xFFFF;

	rdfpga_crypto_init(self, sc);
}
/*
crypto_register(u_int32_t driverid, int alg, u_int16_t maxoplen,
	    u_int32_t flags,
	    int (*newses)(void*, u_int32_t*, struct cryptoini*),
	    int (*freeses)(void*, u_int64_t),
	    int (*process)(void*, struct cryptop *, int),
	    void *arg);
*/

#include <opencrypto/cryptodev.h>
#include <sys/cprng.h>
#include <crypto/rijndael/rijndael.h>
/* most of the code is stolen from swcrypto */

#define COPYBACK(x, a, b, c, d) \
	(x) == CRYPTO_BUF_MBUF ? m_copyback((struct mbuf *)a,b,c,d) \
	: cuio_copyback((struct uio *)a,b,c,d)
#define COPYDATA(x, a, b, c, d) \
	(x) == CRYPTO_BUF_MBUF ? m_copydata((struct mbuf *)a,b,c,d) \
	: cuio_copydata((struct uio *)a,b,c,d)

static int rdfpga_newses(void*, u_int32_t*, struct cryptoini*);
static int rdfpga_freeses(void*, u_int64_t);
static int rdfpga_process(void*, struct cryptop *, int);

static void rdfpga_rijndael128_encrypt(void *key, u_int8_t *blk);
static void rdfpga_rijndael128_decrypt(void *key, u_int8_t *blk);
static int  rdfpga_rijndael128_setkey(u_int8_t **sched, const u_int8_t *key, int len);
static void rdfpga_rijndael128_zerokey(u_int8_t **sched);
static int rdfpga_encdec_aes128cbc(struct rdfpga_softc *sw, const u_int8_t thesid, struct cryptodesc *crd, void *bufv, int outtype);


typedef struct {
  int	decrypt;
  int	Nr;		/* key-length-dependent number of rounds */
  uint32_t ek[4 * (RIJNDAEL_MAXNR + 1)];	/* encrypt key schedule */
  uint32_t dk[4 * (RIJNDAEL_MAXNR + 1)];	/* decrypt key schedule */
  
  struct rdfpga_softc *sc;
  int readback;
  int cbc;
} rdfpga_rijndael_ctx;

struct rdfpga_enc_xform {
/*	const struct enc_xform *enc_xform; */
	void (*encrypt)(void *, uint8_t *);
	void (*decrypt)(void *, uint8_t *);
	int  (*setkey)(uint8_t **, const uint8_t *, int);
	void (*zerokey)(uint8_t **);
	void (*reinit)(void *, const uint8_t *, uint8_t *);
};
static const struct rdfpga_enc_xform rdfpga_enc_xform_rijndael128 = {
	/* &enc_xform_rijndael128, */
	rdfpga_rijndael128_encrypt,
	rdfpga_rijndael128_decrypt,
	rdfpga_rijndael128_setkey,
	rdfpga_rijndael128_zerokey,
	NULL
};

static void rdfpga_crypto_init(device_t self, struct rdfpga_softc *sc) {
  sc->cr_id = crypto_get_driverid(0);
  if (sc->cr_id < 0) {
    aprint_error_dev(self, ": crypto_get_driverid failed\n");
    return;
  }
  crypto_register(sc->cr_id, CRYPTO_AES_CBC, 0, 0, rdfpga_newses, rdfpga_freeses, rdfpga_process, sc);

  sc->sid = 0; // no session
}

static int rdfpga_newses(void* arg, u_int32_t* sid, struct cryptoini* cri) {
  struct rdfpga_softc *sc = arg;
  struct cryptoini *c;
  int i, abort = 0, res;
  u_int32_t ctrl;
  u_int8_t thesid;
  
  /* aprint_normal_dev(sc->sc_dev, "newses: %p %p %p\n", arg, sid, cri); */
  
  if (sid == NULL || cri == NULL || sc == NULL)
    return (EINVAL);

  if (sc->sid == 0xFFFF)
    return (ENOMEM);

  i = 0;
  for (c = cri; c != NULL; c = c->cri_next) {
    
    /* aprint_normal_dev(sc->sc_dev, "newses: [%d] %d %d %d\n", i, c->cri_alg, c->cri_klen, c->cri_rnd); */
    
    if (c->cri_alg != CRYPTO_AES_CBC)
      abort = 1;
    
    if ((c->cri_klen != 128) && (c->cri_klen != 256))
      abort = 1;
    
    /* if (c->cri_rnd != 10)
       abort = 1;*/

    i++;
  }

  if (abort)
    return ENXIO;

  thesid = 0;
  while ((sc->sid & (1<<thesid)) && thesid <= 15) {
    thesid ++;
  }
  if (thesid > 15) {
    // oups...
    aprint_error_dev(sc->sc_dev, "newses: 0x%04hx is full ?\n", sc->sid);
    return (ENOMEM);
  }

  sc->sid |= 1<<thesid;
  
  res = rdfpga_rijndael128_setkey(&sc->sessions[thesid].sw_kschedule, cri->cri_key, cri->cri_klen / 8);
  if (res) {
    aprint_error_dev(sc->sc_dev, "newses: setkey failed (%d)\n", res);
    return EINVAL;
  }
  ((rdfpga_rijndael_ctx *)sc->sessions[thesid].sw_kschedule)->sc = sc;
  ((rdfpga_rijndael_ctx *)sc->sessions[thesid].sw_kschedule)->readback = 1;
  ((rdfpga_rijndael_ctx *)sc->sessions[thesid].sw_kschedule)->cbc = 0;
  sc->sessions[thesid].klen = cri->cri_klen;
  
  memcpy(sc->sessions[thesid].aesiv, cri->cri_iv, 16);
  memcpy(sc->sessions[thesid].aeskey, cri->cri_key, cri->cri_klen / 8);
  
  if ((res = rdfpga_wait_aes_ready(sc)) != 0)
    return res;
  
  for (i = 0 ; i < cri->cri_klen / 64 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), sc->sessions[thesid].aeskey[i]);
  for (i = 0 ; i < 2 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), 0ull);
  
  /* blank run with a zero-block to force keygen in the AES block */
  ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_NEWKEY;
  if (cri->cri_klen == 256)
    ctrl |= RDFPGA_MASK_AES128_AES256;
  bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
  sc->aes_key_refresh = thesid;
  
  *sid = thesid;
  
  aprint_normal_dev(sc->sc_dev, "newses: %p %p (%d) %p, current usage 0x%04hx\n", arg, sid, *sid, cri, sc->sid);

  return 0;
}
static int rdfpga_freeses(void* arg, u_int64_t tid) {
  struct rdfpga_softc *sc = arg;
  u_int8_t thesid = ((u_int8_t)tid) & 0xff;
  u_int16_t oldsid = sc->sid;
  sc->sid &= ~(1<<thesid);
  
  aprint_normal_dev(sc->sc_dev, "freeses %hhd (from 0x%04hx to 0x%04hx)\n", thesid, oldsid, sc->sid);

  memset(sc->sessions[thesid].aeskey, 0, sizeof(sc->sessions[thesid].aeskey));
  memset(sc->sessions[thesid].aesiv, 0, sizeof(sc->sessions[thesid].aesiv));

  return 0;
}

static void
rdfpga_rijndael128_encrypt(void *key, u_int8_t *blk)
{
  u_int32_t ctrl;
  u_int64_t data[2];
  u_int64_t *ptr;
  int i;
  rdfpga_rijndael_ctx* ctx;
  struct rdfpga_softc *sc;
  
  ctx = key;
  sc = ctx->sc;

  /* alignment constraint */
  if (!(((u_int32_t)blk) & 0x7)) {
    ptr = (u_int64_t*)blk;
  } else {
    memcpy(data, blk, 16);
    ptr = data;
  }
  
  if (rdfpga_wait_aes_ready(sc)) {
    aprint_error_dev(sc->sc_dev, "rdfpga_rijndael128_crypt: stuck\n");
    return;
  }
  
  /* aprint_normal_dev(sc->sc_dev, "rdfpga_rijndael128_crypt: write data & start\n"); */
  
  for (i = 0 ; i < 2 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), ptr[i] );
  ctrl = RDFPGA_MASK_AES128_START;
  if (ctx->cbc)
    ctrl |= RDFPGA_MASK_AES128_CBCMOD;
  bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);

  if (ctx->readback) {
    if (rdfpga_wait_aes_ready(sc)) {
      aprint_error_dev(sc->sc_dev, "rdfpga_rijndael128_crypt: stuck\n");
      return;
    }
    
    for (i = 0 ; i < 2 ; i++)
      ptr[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
    
    if (!(((u_int32_t)blk) & 0x7)) {
      /* nothing */
    } else {
      memcpy(blk, data, 16);
    }
  }
}

static int
rdfpga_rijndael128_writeivforcbc(void *key, u_int8_t *blk)
{
  u_int64_t data[2];
  u_int64_t *ptr;
  int i, res;
  rdfpga_rijndael_ctx* ctx;
  struct rdfpga_softc *sc;
  
  ctx = key;
  sc = ctx->sc;

  /* alignment constraint */
  if (!(((u_int32_t)blk) & 0x7)) {
    ptr = (u_int64_t*)blk;
  } else {
    memcpy(data, blk, 16);
    ptr = data;
  }
  
  if ((res = rdfpga_wait_aes_ready(sc)) != 0)
    return res;

  for (i = 0 ; i < 2 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)), ptr[i]);

  return 0;
}

static void
rdfpga_rijndael128_decrypt(void *key, u_int8_t *blk)
{
  /* ugly cast */
  //	rijndael_decrypt((rijndael_ctx *) key, (u_char *) blk, (u_char *) blk);
    u_int32_t ctrl;
  u_int64_t data[2];
  u_int64_t *ptr;
  int i;
  rdfpga_rijndael_ctx* ctx;
  struct rdfpga_softc *sc;
  
  ctx = key;
  sc = ctx->sc;

  /* alignment constraint */
  if (!(((u_int32_t)blk) & 0x7)) {
    ptr = (u_int64_t*)blk;
  } else {
    memcpy(data, blk, 16);
    ptr = data;
  }
  
  if (rdfpga_wait_aes_ready(sc)) {
    aprint_error_dev(sc->sc_dev, "rdfpga_rijndael128_crypt: stuck\n");
    return;
  }
  
  /* aprint_normal_dev(sc->sc_dev, "rdfpga_rijndael128_crypt: write data & start\n"); */
  
  for (i = 0 ; i < 2 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), ptr[i] );
  ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_DEC;
  //  if (ctx->cbc)
  //    ctrl |= RDFPGA_MASK_AES128_CBCMOD;
  bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);

  if (ctx->readback) {
    if (rdfpga_wait_aes_ready(sc)) {
      aprint_error_dev(sc->sc_dev, "rdfpga_rijndael128_crypt: stuck\n");
      return;
    }
    
    for (i = 0 ; i < 2 ; i++)
      ptr[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
    
    if (!(((u_int32_t)blk) & 0x7)) {
      /* nothing */
    } else {
      memcpy(blk, data, 16);
    }
  }
}

static int
rdfpga_rijndael128_setkey(u_int8_t **sched, const u_int8_t *key, int len)
{

	if (len != 16 && len != 24 && len != 32)
		return EINVAL;
	*sched = malloc(sizeof(rdfpga_rijndael_ctx), M_CRYPTO_DATA,
	    M_NOWAIT|M_ZERO);
	if (*sched == NULL)
		return ENOMEM;
  /* ugly cast */
	rijndael_set_key((rijndael_ctx *) *sched, key, len * 8);
	return 0;
}

static void
rdfpga_rijndael128_zerokey(u_int8_t **sched)
{
	memset(*sched, 0, sizeof(rdfpga_rijndael_ctx));
	free(*sched, M_CRYPTO_DATA);
	*sched = NULL;
}

static int
rdfpga_encdec_aes128cbc(struct rdfpga_softc *sw, const u_int8_t thesid, struct cryptodesc *crd, void *bufv, int outtype)
{
	char *buf = bufv;
	unsigned char iv[EALG_MAX_BLOCK_LEN], __attribute__ ((aligned(8))) blk[EALG_MAX_BLOCK_LEN], *idat;
	unsigned char *ivp, piv[EALG_MAX_BLOCK_LEN];
	//const struct swcr_enc_xform *exf;
	const struct rdfpga_enc_xform *exf = &rdfpga_enc_xform_rijndael128;
	int i, k, j, blks, ivlen;
	int count, ind;
	rdfpga_rijndael_ctx* ctx = (rdfpga_rijndael_ctx*)sw->sessions[thesid].sw_kschedule;

	//exf = sw->sw_exf;
	blks = 16; //exf->enc_xform->blocksize;
	ivlen = 16; //exf->enc_xform->ivsize;
	/* KASSERT(exf->reinit ? ivlen <= blks : ivlen == blks); */

	/* Check for non-padded data */
	if (crd->crd_len % blks)
		return EINVAL;

	/* Initialize the IV */
	if (crd->crd_flags & CRD_F_ENCRYPT) {
		/* IV explicitly provided ? */
		if (crd->crd_flags & CRD_F_IV_EXPLICIT) {
			memcpy(iv, crd->crd_iv, ivlen);
		} else {
			/* Get random IV */
			for (i = 0;
			    i + sizeof (u_int32_t) <= EALG_MAX_BLOCK_LEN;
			    i += sizeof (u_int32_t)) {
				u_int32_t temp = cprng_fast32();

				memcpy(iv + i, &temp, sizeof(u_int32_t));
			}
			/*
			 * What if the block size is not a multiple
			 * of sizeof (u_int32_t), which is the size of
			 * what arc4random() returns ?
			 */
			if (EALG_MAX_BLOCK_LEN % sizeof (u_int32_t) != 0) {
				u_int32_t temp = cprng_fast32();

				bcopy (&temp, iv + i,
				    EALG_MAX_BLOCK_LEN - i);
			}
		}

		/* Do we need to write the IV */
		if (!(crd->crd_flags & CRD_F_IV_PRESENT)) {
			COPYBACK(outtype, buf, crd->crd_inject, ivlen, iv);
		}

	} else {	/* Decryption */
			/* IV explicitly provided ? */
		if (crd->crd_flags & CRD_F_IV_EXPLICIT)
			memcpy(iv, crd->crd_iv, ivlen);
		else {
			/* Get IV off buf */
			COPYDATA(outtype, buf, crd->crd_inject, ivlen, iv);
		}
	}

	ivp = iv;

	if (outtype == CRYPTO_BUF_CONTIG) {
		if (crd->crd_flags & CRD_F_ENCRYPT) {
			for (i = crd->crd_skip;
			    i < crd->crd_skip + crd->crd_len; i += blks) {
				/* XOR with the IV/previous block, as appropriate. */
				if (i == crd->crd_skip)
					for (k = 0; k < blks; k++)
						buf[i + k] ^= ivp[k];
				else
					for (k = 0; k < blks; k++)
						buf[i + k] ^= buf[i + k - blks];
				exf->encrypt(sw->sessions[thesid].sw_kschedule, buf + i);
			}
		} else {		/* Decrypt */
			/*
			 * Start at the end, so we don't need to keep the encrypted
			 * block as the IV for the next block.
			 */
			for (i = crd->crd_skip + crd->crd_len - blks;
			    i >= crd->crd_skip; i -= blks) {
				exf->decrypt(sw->sessions[thesid].sw_kschedule, buf + i);

				/* XOR with the IV/previous block, as appropriate */
				if (i == crd->crd_skip)
					for (k = 0; k < blks; k++)
						buf[i + k] ^= ivp[k];
				else
					for (k = 0; k < blks; k++)
						buf[i + k] ^= buf[i + k - blks];
			}
		}

		return 0;
	} else if (outtype == CRYPTO_BUF_MBUF) {
		struct mbuf *m = (struct mbuf *) buf;

		/* Find beginning of data */
		m = m_getptr(m, crd->crd_skip, &k);
		if (m == NULL)
			return EINVAL;

		i = crd->crd_len;

		while (i > 0) {
			/*
			 * If there's insufficient data at the end of
			 * an mbuf, we have to do some copying.
			 */
			if (m->m_len < k + blks && m->m_len != k) {
				m_copydata(m, k, blks, blk);

				/* Actual encryption/decryption */
				if (crd->crd_flags & CRD_F_ENCRYPT) {
					/* XOR with previous block */
					for (j = 0; j < blks; j++)
						blk[j] ^= ivp[j];

					exf->encrypt(sw->sessions[thesid].sw_kschedule, blk);

					/*
					 * Keep encrypted block for XOR'ing
					 * with next block
					 */
					memcpy(iv, blk, blks);
					ivp = iv;
				} else {	/* decrypt */
					/*
					 * Keep encrypted block for XOR'ing
					 * with next block
					 */
					if (ivp == iv)
						memcpy(piv, blk, blks);
					else
						memcpy(iv, blk, blks);

					exf->decrypt(sw->sessions[thesid].sw_kschedule, blk);

					/* XOR with previous block */
					for (j = 0; j < blks; j++)
						blk[j] ^= ivp[j];

					if (ivp == iv)
						memcpy(iv, piv, blks);
					else
						ivp = iv;
				}

				/* Copy back decrypted block */
				m_copyback(m, k, blks, blk);

				/* Advance pointer */
				m = m_getptr(m, k + blks, &k);
				if (m == NULL)
					return EINVAL;

				i -= blks;

				/* Could be done... */
				if (i == 0)
					break;
			}

			/* Skip possibly empty mbufs */
			if (k == m->m_len) {
				for (m = m->m_next; m && m->m_len == 0;
				    m = m->m_next)
					;
				k = 0;
			}

			/* Sanity check */
			if (m == NULL)
				return EINVAL;

			/*
			 * Warning: idat may point to garbage here, but
			 * we only use it in the while() loop, only if
			 * there are indeed enough data.
			 */
			idat = mtod(m, unsigned char *) + k;

			while (m->m_len >= k + blks && i > 0) {
				if (crd->crd_flags & CRD_F_ENCRYPT) {
					/* XOR with previous block/IV */
					for (j = 0; j < blks; j++)
						idat[j] ^= ivp[j];

					exf->encrypt(sw->sessions[thesid].sw_kschedule, idat);
					ivp = idat;
				} else {	/* decrypt */
					/*
					 * Keep encrypted block to be used
					 * in next block's processing.
					 */
					if (ivp == iv)
						memcpy(piv, idat, blks);
					else
						memcpy(iv, idat, blks);

					exf->decrypt(sw->sessions[thesid].sw_kschedule, idat);

					/* XOR with previous block/IV */
					for (j = 0; j < blks; j++)
						idat[j] ^= ivp[j];

					if (ivp == iv)
						memcpy(iv, piv, blks);
					else
						ivp = iv;
				}

				idat += blks;
				k += blks;
				i -= blks;
			}
		}

		return 0; /* Done with mbuf encryption/decryption */
	} else if (outtype == CRYPTO_BUF_IOV) {
		struct uio *uio = (struct uio *) buf;

		/* Find beginning of data */
		count = crd->crd_skip;
		ind = cuio_getptr(uio, count, &k);
		if (ind == -1)
			return EINVAL;

		i = crd->crd_len;

		while (i > 0) {
			/*
			 * If there's insufficient data at the end,
			 * we have to do some copying.
			 */
			if (uio->uio_iov[ind].iov_len < k + blks &&
			    uio->uio_iov[ind].iov_len != k) {
				cuio_copydata(uio, k, blks, blk);

				/* Actual encryption/decryption */
				if (crd->crd_flags & CRD_F_ENCRYPT) {
					/* XOR with previous block */
					if (!ctx->cbc) {
					  for (j = 0; j < blks; j++)
					    blk[j] ^= ivp[j];
					}
					exf->encrypt(sw->sessions[thesid].sw_kschedule, blk);
					ctx->cbc = 1;
					/*
					 * Keep encrypted block for XOR'ing
					 * with next block
					 */
					if (!ctx->cbc) {
					  memcpy(iv, blk, blks);
					  ivp = iv;
					}
				} else {	/* decrypt */
					/*
					 * Keep encrypted block for XOR'ing
					 * with next block
					 */
					if (ivp == iv)
						memcpy(piv, blk, blks);
					else
						memcpy(iv, blk, blks);

					exf->decrypt(sw->sessions[thesid].sw_kschedule, blk);

					/* XOR with previous block */
					for (j = 0; j < blks; j++)
						blk[j] ^= ivp[j];

					if (ivp == iv)
						memcpy(iv, piv, blks);
					else
						ivp = iv;
				}

				/* Copy back decrypted block */
				cuio_copyback(uio, k, blks, blk);

				count += blks;

				/* Advance pointer */
				ind = cuio_getptr(uio, count, &k);
				if (ind == -1)
					return (EINVAL);

				i -= blks;

				/* Could be done... */
				if (i == 0)
					break;
			}

			/*
			 * Warning: idat may point to garbage here, but
			 * we only use it in the while() loop, only if
			 * there are indeed enough data.
			 */
			idat = ((char *)uio->uio_iov[ind].iov_base) + k;

			if (!ctx->cbc && (crd->crd_flags & CRD_F_ENCRYPT)) {
			  if (rdfpga_rijndael128_writeivforcbc(sw->sessions[thesid].sw_kschedule, ivp)) {
			    aprint_error_dev(sw->sc_dev, "rdfpga_rijndael128_crypt: stuck\n");
			  } else {
			    ctx->cbc = 1;
			  }
			}
/* ********************************************************************************** */
			if ((crd->crd_flags & CRD_F_ENCRYPT) && ctx->cbc && ((uio->uio_iov[ind].iov_len - k) >= 32)) {
			  bus_dma_segment_t segs;
			  int rsegs;
			  size_t tocopy = uio->uio_iov[ind].iov_len - k;
			  uint64_t ctrl;
			  /* int error; */
			  
			  tocopy &= ~(size_t)0x0F;
			  if (tocopy > RDFPGA_VAL_DMA_MAX_SZ)
			    tocopy = RDFPGA_VAL_DMA_MAX_SZ;

			  /* aprint_normal_dev(sw->sc_dev, "AES DMA: %zd @ %p (%d) [from %d]\n", tocopy, idat, ind, k); */
			  
			  if (bus_dmamem_alloc(sw->sc_dmatag, RDFPGA_VAL_DMA_MAX_SZ, 64, 64, &segs, 1, &rsegs, BUS_DMA_NOWAIT | BUS_DMA_STREAMING)) {
			    aprint_error_dev(sw->sc_dev, "cannot allocate DVMA memory");
			    goto afterdma;
			  }
			  void* kvap;
			  if (bus_dmamem_map(sw->sc_dmatag, &segs, 1, RDFPGA_VAL_DMA_MAX_SZ, &kvap, BUS_DMA_NOWAIT)) {
			    aprint_error_dev(sw->sc_dev, "cannot allocate DVMA address");
			    bus_dmamem_free(sw->sc_dmatag, &segs, 1);
			    goto afterdma;
			  }
			  if (bus_dmamap_load(sw->sc_dmatag, sw->sc_dmamap, kvap, RDFPGA_VAL_DMA_MAX_SZ, /* kernel space */ NULL,
					      BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE | BUS_DMA_READ)) {
			    aprint_error_dev(sw->sc_dev, "cannot load dma map");
			    bus_dmamem_unmap(sw->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ);
			    bus_dmamem_free(sw->sc_dmatag, &segs, 1);
			    goto afterdma;
			  }
			  /* if ((error = uiomove(kvap, tocopy, uio)) != 0) { */
			  /*   aprint_error_dev(sw->sc_dev, "cannot copy from uio space"); */
			  /*   bus_dmamap_unload(sw->sc_dmatag, sw->sc_dmamap); */
			  /*   bus_dmamem_unmap(sw->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ); */
			  /*   bus_dmamem_free(sw->sc_dmatag, &segs, 1); */
			  /*   goto afterdma; */
			  /* } */
			  memcpy(kvap, idat, tocopy);
			  
			  bus_dmamap_sync(sw->sc_dmatag, sw->sc_dmamap, 0, tocopy, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
			  /* start write */
			  ctrl = ((uint64_t)(RDFPGA_MASK_DMA_CTRL_START | ((tocopy/16)-1))) | ((uint64_t)(uint32_t)(sw->sc_dmamap->dm_segs[0].ds_addr)) << 32;
			  bus_space_write_8(sw->sc_bustag, sw->sc_bhregs, (RDFPGA_REG_AESDMAW_ADDR), ctrl);
			  /* start read */
			  ctrl = ((uint64_t)(RDFPGA_MASK_DMA_CTRL_START | RDFPGA_MASK_DMA_CTRL_CBC | ((tocopy/16)-1))) | ((uint64_t)(uint32_t)(sw->sc_dmamap->dm_segs[0].ds_addr)) << 32;
			  bus_space_write_8(sw->sc_bustag, sw->sc_bhregs, (RDFPGA_REG_AESDMA_ADDR), ctrl);
			  rdfpga_wait_dma_ready(sw, 50000);
			  bus_dmamap_sync(sw->sc_dmatag, sw->sc_dmamap, 0, tocopy, BUS_DMASYNC_POSTWRITE | BUS_DMASYNC_POSTREAD);

			  memcpy(idat, kvap, tocopy);
			  
			  bus_dmamap_unload(sw->sc_dmatag, sw->sc_dmamap);
			  bus_dmamem_unmap(sw->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ);
			  bus_dmamem_free(sw->sc_dmatag, &segs, 1);

			  idat += tocopy;
			  count += tocopy;
			  k += tocopy;
			  i -= tocopy;
			}
/* ********************************************************************************** */
			if (!(crd->crd_flags & CRD_F_ENCRYPT) && ((uio->uio_iov[ind].iov_len - k) >= 32)) {
			  bus_dma_segment_t segs;
			  int rsegs;
			  size_t tocopy = uio->uio_iov[ind].iov_len - k;
			  uint64_t ctrl;
			  /* int error; */
			  
			  tocopy &= ~(size_t)0x0F;
			  if (tocopy > RDFPGA_VAL_DMA_MAX_SZ)
			    tocopy = RDFPGA_VAL_DMA_MAX_SZ;

			  /* aprint_normal_dev(sw->sc_dev, "AES DMA: %zd @ %p (%d) [from %d]\n", tocopy, idat, ind, k); */
			  
			  if (bus_dmamem_alloc(sw->sc_dmatag, RDFPGA_VAL_DMA_MAX_SZ, 64, 64, &segs, 1, &rsegs, BUS_DMA_NOWAIT | BUS_DMA_STREAMING)) {
			    aprint_error_dev(sw->sc_dev, "cannot allocate DVMA memory");
			    goto afterdma;
			  }
			  void* kvap;
			  if (bus_dmamem_map(sw->sc_dmatag, &segs, 1, RDFPGA_VAL_DMA_MAX_SZ, &kvap, BUS_DMA_NOWAIT)) {
			    aprint_error_dev(sw->sc_dev, "cannot allocate DVMA address");
			    bus_dmamem_free(sw->sc_dmatag, &segs, 1);
			    goto afterdma;
			  }
			  if (bus_dmamap_load(sw->sc_dmatag, sw->sc_dmamap, kvap, RDFPGA_VAL_DMA_MAX_SZ, /* kernel space */ NULL,
					      BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE | BUS_DMA_READ)) {
			    aprint_error_dev(sw->sc_dev, "cannot load dma map");
			    bus_dmamem_unmap(sw->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ);
			    bus_dmamem_free(sw->sc_dmatag, &segs, 1);
			    goto afterdma;
			  }
			  /* if ((error = uiomove(kvap, tocopy, uio)) != 0) { */
			  /*   aprint_error_dev(sw->sc_dev, "cannot copy from uio space"); */
			  /*   bus_dmamap_unload(sw->sc_dmatag, sw->sc_dmamap); */
			  /*   bus_dmamem_unmap(sw->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ); */
			  /*   bus_dmamem_free(sw->sc_dmatag, &segs, 1); */
			  /*   goto afterdma; */
			  /* } */
			  memcpy(kvap, idat, tocopy);
			  
			  bus_dmamap_sync(sw->sc_dmatag, sw->sc_dmamap, 0, tocopy, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
			  /* start write */
			  ctrl = ((uint64_t)(RDFPGA_MASK_DMA_CTRL_START | RDFPGA_MASK_DMA_CTRL_DEC | ((tocopy/16)-1))) | ((uint64_t)(uint32_t)(sw->sc_dmamap->dm_segs[0].ds_addr)) << 32;
			  bus_space_write_8(sw->sc_bustag, sw->sc_bhregs, (RDFPGA_REG_AESDMAW_ADDR), ctrl);
			  /* start read */
			  ctrl = ((uint64_t)(RDFPGA_MASK_DMA_CTRL_START | RDFPGA_MASK_DMA_CTRL_DEC | ((tocopy/16)-1))) | ((uint64_t)(uint32_t)(sw->sc_dmamap->dm_segs[0].ds_addr)) << 32;
			  bus_space_write_8(sw->sc_bustag, sw->sc_bhregs, (RDFPGA_REG_AESDMA_ADDR), ctrl);
			  rdfpga_wait_dma_ready(sw, 50000);
			  bus_dmamap_sync(sw->sc_dmatag, sw->sc_dmamap, 0, tocopy, BUS_DMASYNC_POSTWRITE | BUS_DMASYNC_POSTREAD);

			  //memcpy(idat, kvap, tocopy);
			  #if 0
			  for (j = 0 ; j < blks ; j++) {
			    piv[j] = idat[(tocopy-1)-blks];
			  }
			  for (j = tocopy-1 ; j >= blks ; j --) {
			    idat[j] = kvap[j] ^ idat[j - blks];
			  }
			  for (j = 0 ; j < blks ; j++) {
			    idat[j] = kvap[j] ^ ivp[j];
			    ivp[j] = piv[j];
			  }
			  #else
#define u32blks ((blks)/sizeof(u_int32_t))
#define p32(x) ((u_int32_t*)x)
			  for (j = 0 ; j < blks/sizeof(u_int32_t) ; j++) {
			    p32(piv)[j] = p32(idat)[(tocopy/sizeof(u_int32_t))-1-u32blks];
			  }
			  for (j = (tocopy/sizeof(u_int32_t))-1 ; j >= u32blks ; j --) {
			    p32(idat)[j] = p32(kvap)[j] ^ p32(idat)[j - u32blks];
			  }
			  for (j = 0 ; j < u32blks ; j++) {
			    p32(idat)[j] = p32(kvap)[j] ^ p32(ivp)[j];
			    p32(ivp)[j] = p32(piv)[j];
			  }
#undef p32
#undef u32blks
			  #endif
			  
			  bus_dmamap_unload(sw->sc_dmatag, sw->sc_dmamap);
			  bus_dmamem_unmap(sw->sc_dmatag, kvap, RDFPGA_VAL_DMA_MAX_SZ);
			  bus_dmamem_free(sw->sc_dmatag, &segs, 1);

			  idat += tocopy;
			  count += tocopy;
			  k += tocopy;
			  i -= tocopy;
			}
			afterdma:
/* ********************************************************************************** */

			while (uio->uio_iov[ind].iov_len >= k + blks &&
			    i > 0) {
				if (crd->crd_flags & CRD_F_ENCRYPT) {
					/* XOR with previous block/IV */
					if (!ctx->cbc) {
					  for (j = 0; j < blks; j++)
					    idat[j] ^= ivp[j];
					}

					exf->encrypt(sw->sessions[thesid].sw_kschedule, idat);
					ctx->cbc = 1;
					ivp = idat;
				} else {	/* decrypt */
					/*
					 * Keep encrypted block to be used
					 * in next block's processing.
					 */
					if (ivp == iv)
						memcpy(piv, idat, blks);
					else
						memcpy(iv, idat, blks);

					exf->decrypt(sw->sessions[thesid].sw_kschedule, idat);

					/* XOR with previous block/IV */
					for (j = 0; j < blks; j++)
						idat[j] ^= ivp[j];

					if (ivp == iv)
						memcpy(iv, piv, blks);
					else
						ivp = iv;
				}

				idat += blks;
				count += blks;
				k += blks;
				i -= blks;
			}
		}
		return 0; /* Done with mbuf encryption/decryption */
	}

	/* Unreachable */
	return EINVAL;
}

static int rdfpga_process(void* arg, struct cryptop * crp, int hint) {
  struct rdfpga_softc *sc = arg;
  struct cryptodesc *crd;
  u_int8_t thesid;
  int type;
  int res;
  
  if (crp == NULL || crp->crp_callback == NULL || sc == NULL) {
    return (EINVAL);
  }

  thesid = ((u_int8_t)crp->crp_sid) & 0xff;
  
  if (!(sc->sid & (1<<thesid)))
    return (EINVAL);
 
  if (crp->crp_flags & CRYPTO_F_IMBUF) {
    type = CRYPTO_BUF_MBUF;
  } else if (crp->crp_flags & CRYPTO_F_IOV) {
    type = CRYPTO_BUF_IOV;
  } else {
    type = CRYPTO_BUF_CONTIG;
  }

  /* if (res = rdfpga_wait_aes_ready(sc)) */
  /*   return res; */

  if (sc->aes_key_refresh != thesid) {
    int i;
    aprint_normal_dev(sc->sc_dev, "refreshing key for session %hhu\n", thesid); 
    if ((res = rdfpga_wait_aes_ready(sc)) != 0)
      return res;
    for (i = 0 ; i < sc->sessions[thesid].klen / 64 ; i++)
      bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), sc->sessions[thesid].aeskey[i]);
    for (i = 0 ; i < 2 ; i++)
      bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), 0ull);
    
    u_int32_t ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_NEWKEY;
    if (sc->sessions[thesid].klen == 256)
      ctrl |= RDFPGA_MASK_AES128_AES256;
    bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
    sc->aes_key_refresh = thesid;
    ((rdfpga_rijndael_ctx *)sc->sessions[thesid].sw_kschedule)->cbc = 0;
  }
  
  for (crd = crp->crp_desc; crd != NULL; crd = crd->crd_next) {
    res = rdfpga_encdec_aes128cbc(sc, thesid, crd, crp->crp_buf, type);
    if (res)
      return res;
    crypto_done(crp);
  }

  return 0;
}
