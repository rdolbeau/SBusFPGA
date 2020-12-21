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


extern struct cfdriver rdfpga_cd;

struct rdfpga_128bits {
	uint32_t x[4];
};
struct rdfpga_128bits_alt {
	uint64_t x[2];
};

#define RDFPGA_WC   _IOW(0, 1, struct rdfpga_128bits)
#define RDFPGA_WH   _IOW(0, 2, struct rdfpga_128bits)
#define RDFPGA_WI   _IOW(0, 3, struct rdfpga_128bits)
#define RDFPGA_RC   _IOR(0, 4, struct rdfpga_128bits)
#define RDFPGA_WL   _IOW(0, 5, uint32_t)

#define RDFPGA_AESWK   _IOW(0, 10, struct rdfpga_128bits)
#define RDFPGA_AESWD   _IOW(0, 11, struct rdfpga_128bits)
#define RDFPGA_AESRO   _IOR(0, 12, struct rdfpga_128bits)

int
rdfpga_ioctl (dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
        struct rdfpga_softc *sc = device_lookup_private(&rdfpga_cd, minor(dev));
	struct rdfpga_128bits_alt *bits = (struct rdfpga_128bits_alt*)data;
        int err = 0, i, ctr = 0;
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
	        ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL);
		if (ctrl)
		  return EBUSY;
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), bits->x[i] );
		sc->aes_key_refresh = 1;
                break;
        case RDFPGA_AESWD:
	        ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL);
		if (ctrl)
		  return EBUSY;
		for (i = 0 ; i < 2 ; i++)
			bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), bits->x[i] );
		ctrl = RDFPGA_MASK_AES128_START;
		if (sc->aes_key_refresh) {
		  ctrl |= RDFPGA_MASK_AES128_NEWKEY;
		  sc->aes_key_refresh = 0;
		}
	        bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
                break;
        case RDFPGA_AESRO:
	        ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL);
                while (ctrl && (ctr < 3)) {
		    delay(1);
	            ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL);
		    ctr ++;
		}
		if (ctrl)
		  return EBUSY;
		for (i = 0 ; i < 2 ; i++)
			bits->x[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
                break;
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
	    return ENXIO;
	  }
	  /* else { */
	  /*   aprint_normal_dev(sc->sc_dev, "dmamem map: %p\n", kvap); */
	  /* } */
	  
	  if (bus_dmamap_load(sc->sc_dmatag, sc->sc_dmamap, kvap, RDFPGA_VAL_DMA_MAX_SZ, /* kernel space */ NULL,
	  		      BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE)) {
	    aprint_error_dev(sc->sc_dev, "cannot load dma map");
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

	  bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_DMA_ADDR), ctrl);
	  
	  /* aprint_normal_dev(sc->sc_dev, "dma: cmd sent\n"); */

	  res = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_DMA_CTRL));
	  do {
	    ctr ++;
	    delay(2);
	    oldres = res;
	    res = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_DMA_CTRL));
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

	return (strcmp("RDOL,SBusFPGA", sa->sa_name) == 0);
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

	for (i = 0 ; i < 2 ; i++)
	  bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), 0ull);
	sc->aes_key_refresh = 1;

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
/* most of the CTR is stolen from swcrypto */

#define COPYBACK(x, a, b, c, d) \
	(x) == CRYPTO_BUF_MBUF ? m_copyback((struct mbuf *)a,b,c,d) \
	: cuio_copyback((struct uio *)a,b,c,d)
#define COPYDATA(x, a, b, c, d) \
	(x) == CRYPTO_BUF_MBUF ? m_copydata((struct mbuf *)a,b,c,d) \
	: cuio_copydata((struct uio *)a,b,c,d)

#define AESCTR_NONCESIZE	4
#define AESCTR_IVSIZE		8
#define AESCTR_BLOCKSIZE	16

struct rdfpga_aes_ctr_ctx {
	/* need only encryption half */
	//u_int32_t ac_ek[4*(/*RIJNDAEL_MAXNR*/10 + 1)];
	u_int8_t __attribute__ ((aligned(8))) ac_block[AESCTR_BLOCKSIZE];
	int ac_nr;
	struct {
		u_int64_t lastiv;
	} ivgenctx;
	struct rdfpga_softc *sc;
};


static int rdfpga_newses(void*, u_int32_t*, struct cryptoini*);
static int rdfpga_freeses(void*, u_int64_t);
static int rdfpga_process(void*, struct cryptop *, int);

static void rdfpga_aes_ctr_crypt(void *key, u_int8_t *blk);
int rdfpga_aes_ctr_setkey(u_int8_t **sched, const u_int8_t *key, int len);
void rdfpga_aes_ctr_zerokey(u_int8_t **sched);
void rdfpga_aes_ctr_reinit(void *key, const u_int8_t *iv, u_int8_t *ivout);

struct rdfpga_enc_xform {
/*	const struct enc_xform *enc_xform; */
	void (*encrypt)(void *, uint8_t *);
	void (*decrypt)(void *, uint8_t *);
	int  (*setkey)(uint8_t **, const uint8_t *, int);
	void (*zerokey)(uint8_t **);
	void (*reinit)(void *, const uint8_t *, uint8_t *);
};
static const struct rdfpga_enc_xform rdfga_enc_xform_aes_ctr = {
/*	&enc_xform_rdfpga_aes_ctr, */
	rdfpga_aes_ctr_crypt,
	rdfpga_aes_ctr_crypt,
	rdfpga_aes_ctr_setkey,
	rdfpga_aes_ctr_zerokey,
	rdfpga_aes_ctr_reinit
};

static void rdfpga_crypto_init(device_t self, struct rdfpga_softc *sc) {
  sc->cr_id = crypto_get_driverid(0);
  if (sc->cr_id < 0) {
    aprint_error_dev(self, ": crypto_get_driverid failed\n");
    return;
  }
  crypto_register(sc->cr_id, CRYPTO_AES_CTR, 0, 0, rdfpga_newses, rdfpga_freeses, rdfpga_process, sc);

  sc->sid = 0; // no session
}

static int rdfpga_newses(void* arg, u_int32_t* sid, struct cryptoini* cri) {
  struct rdfpga_softc *sc = arg;
  struct cryptoini *c;
  int i, abort = 0, res;
  
  /* aprint_normal_dev(sc->sc_dev, "newses: %p %p %p\n", arg, sid, cri); */
  
  if (sid == NULL || cri == NULL || sc == NULL)
    return (EINVAL);

  if (sc->sid)
    return (ENOMEM);

  i = 0;
  for (c = cri; c != NULL; c = c->cri_next) {
    
    /* aprint_normal_dev(sc->sc_dev, "newses: [%d] %d %d %d\n", i, c->cri_alg, c->cri_klen, c->cri_rnd); */
    
    if (c->cri_alg != CRYPTO_AES_CTR)
      abort = 1;
    
    if (c->cri_klen != 128)
      abort = 1;
    
    /* if (c->cri_rnd != 10)
       abort = 1;*/

    i++;
  }

  if (abort)
    return ENXIO;


  res = rdfpga_aes_ctr_setkey(&sc->sw_kschedule, cri->cri_key, cri->cri_klen / 8);
  if (res) {
    aprint_error_dev(sc->sc_dev, "newses: setkey failed (%d)\n", res);
    return EINVAL;
  }
  ((struct rdfpga_aes_ctr_ctx *)sc->sw_kschedule)->sc = sc;
  
  u_int32_t ctrl;
  while ((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL)) != 0) {
    delay(1);
  }
  memcpy(sc->aesiv, cri->cri_iv, 16);
  memcpy(sc->aeskey, cri->cri_key, 16);
  for (i = 0 ; i < 2 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_KEY + (i*8)), sc->aeskey[i]);
  for (i = 0 ; i < 2 ; i++)
    bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), 0ull);
  /* blank run with a zero-block to force keygen in the AES block */
  ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_NEWKEY;
  bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
  sc->aes_key_refresh = 0;
  
  sc->sid = 0xDEADBEEF;
  *sid = sc->sid;
  
  /* aprint_normal_dev(sc->sc_dev, "iv: 0x%016llx 0x%016llx\n", sc->aesiv[0], sc->aesiv[1]); */

  return 0;
}
static int rdfpga_freeses(void* arg, u_int64_t tid) {
  struct rdfpga_softc *sc = arg;

  
  /* aprint_normal_dev(sc->sc_dev, "freeses\n"); */

  sc->sid ^= 0xDEADBEEF;

  memset(sc->aeskey, 0, sizeof(sc->aeskey));
  memset(sc->aesiv, 0, sizeof(sc->aesiv));

  return 0;
}

#include <crypto/rijndael/rijndael.h>

static void
rdfpga_aes_ctr_crypt(void *key, u_int8_t *blk)
{
	struct rdfpga_aes_ctr_ctx *ctx;
	u_int8_t keystream[AESCTR_BLOCKSIZE];
	//u_int8_t keystream2[AESCTR_BLOCKSIZE];
	int i;
	struct rdfpga_softc *sc;

	ctx = key;
	sc = ctx->sc;

	/* aprint_normal_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt\n"); */
	
	/* increment counter */
	for (i = AESCTR_BLOCKSIZE - 1;
	     i >= AESCTR_NONCESIZE + AESCTR_IVSIZE; i--)
		if (++ctx->ac_block[i]) /* continue on overflow */
			break;
	/* not needed, for validation during dev */
	//rijndaelEncrypt(ctx->ac_ek, ctx->ac_nr, ctx->ac_block, keystream2);
	
	u_int32_t ctrl;
	int ctr;

	/* aprint_normal_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: check avail\n"); */
	ctr = 0;
	while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL)) != 0) &&
	       (ctr < 5)) {
	  delay(1);
	  ctr ++;
	}
	if (ctrl) {
	  aprint_error_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: stuck (%x, %d)\n", ctrl, ctr);
	  return;
	}
	
	/* aprint_normal_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: write data & start\n"); */
	       
	for (i = 0 ; i < 2 ; i++)
	  bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), ((u_int64_t*)ctx->ac_block)[i] );
	ctrl = RDFPGA_MASK_AES128_START;
	bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
	
	/* aprint_normal_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: wait for results\n"); */
	
	ctr = 0;
	while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL)) != 0) &&
	       (ctr < 5)) {
	  delay(1);
	  ctr ++;
	}
	if (ctrl) {
	  aprint_error_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: stuck (%x, %d)\n", ctrl, ctr);
	  return;
	}
	
	/* aprint_normal_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: read results\n"); */
	
	for (i = 0 ; i < 2 ; i++)
	  ((u_int64_t*)keystream)[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
	
	/* aprint_normal_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: xor\n"); */

	for (i = 0; i < AESCTR_BLOCKSIZE; i++) {
	  //if (keystream[i] ^ keystream2[i]) {
	  //  aprint_error_dev(sc->sc_dev, "rdfpga_aes_ctr_crypt: [%d] %02hhx <-> %02hhx\n", i, keystream[i], keystream2[i]);
	  //}
	  
	  blk[i] ^= keystream[i];
	}
	
	//memset(keystream, 0, sizeof(keystream));
}

int
rdfpga_aes_ctr_setkey(u_int8_t **sched, const u_int8_t *key, int len)
{
	struct rdfpga_aes_ctr_ctx *ctx;

	if (len < AESCTR_NONCESIZE)
		return EINVAL;

	ctx = malloc(sizeof(struct rdfpga_aes_ctr_ctx), M_CRYPTO_DATA,
		     M_NOWAIT|M_ZERO);
	if (!ctx)
		return ENOMEM;
	/* not needed, for validation during dev */
	//ctx->ac_nr = rijndaelKeySetupEnc(ctx->ac_ek, (const u_char *)key, 128);
	//if (ctx->ac_nr != 10) { /* wrong key len */
	//	rdfpga_aes_ctr_zerokey((u_int8_t **)&ctx);
	//	return EINVAL;
	//}
	ctx->ac_nr = 10;
	
	memcpy(ctx->ac_block, key + len - AESCTR_NONCESIZE, AESCTR_NONCESIZE);
	/* random start value for simple counter */
	cprng_fast(&ctx->ivgenctx.lastiv, sizeof(ctx->ivgenctx.lastiv));
	*sched = (void *)ctx;
	return 0;
}

void
rdfpga_aes_ctr_zerokey(u_int8_t **sched)
{

	memset(*sched, 0, sizeof(struct rdfpga_aes_ctr_ctx));
	free(*sched, M_CRYPTO_DATA);
	*sched = NULL;
}

void
rdfpga_aes_ctr_reinit(void *key, const u_int8_t *iv, u_int8_t *ivout)
{
	struct rdfpga_aes_ctr_ctx *ctx = key;
  
	/* aprint_normal_dev(ctx->sc->sc_dev, "rdfpga_aes_ctr_reinit\n"); */
	
	if (!iv) {
		ctx->ivgenctx.lastiv++;
		iv = (const u_int8_t *)&ctx->ivgenctx.lastiv;
	}
	if (ivout)
		memcpy(ivout, iv, AESCTR_IVSIZE);
	memcpy(ctx->ac_block + AESCTR_NONCESIZE, iv, AESCTR_IVSIZE);
	/* reset counter */
	memset(ctx->ac_block + AESCTR_NONCESIZE + AESCTR_IVSIZE, 0, 4);
}

static int
rdfpga_encdec_aes128(struct rdfpga_softc *sw, struct cryptodesc *crd, void *bufv, int outtype)
{
	char *buf = bufv;
	unsigned char iv[EALG_MAX_BLOCK_LEN], blk[EALG_MAX_BLOCK_LEN], *idat;
	/* unsigned char *ivp, piv[EALG_MAX_BLOCK_LEN]; */
	//const struct swcr_enc_xform *exf;
	const struct rdfpga_enc_xform *exf = &rdfga_enc_xform_aes_ctr;
	int i, k, blks, ivlen; /* j */
	int count, ind;
	
	/* aprint_normal_dev(sw->sc_dev, "rdfpga_encdec_aes128 (%d)\n", outtype); */

	//exf = sw->sw_exf;
	blks = 16; //exf->enc_xform->blocksize;
	ivlen = 8; //exf->enc_xform->ivsize;
	//KASSERT(exf->reinit ? ivlen <= blks : ivlen == blks);

	/* Check for non-padded data */
	if (crd->crd_len % blks)
		return EINVAL;

	/* Initialize the IV */
	if (crd->crd_flags & CRD_F_ENCRYPT) {
		/* IV explicitly provided ? */
		if (crd->crd_flags & CRD_F_IV_EXPLICIT) {
			memcpy(iv, crd->crd_iv, ivlen);
			if (exf->reinit)
				exf->reinit(sw->sw_kschedule, iv, 0);
		} else if (exf->reinit) {
			exf->reinit(sw->sw_kschedule, 0, iv);
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
		if (exf->reinit)
			exf->reinit(sw->sw_kschedule, iv, 0);
	}

	/* ivp = iv; */

	if (outtype == CRYPTO_BUF_CONTIG) {
		if (exf->reinit) {
			for (i = crd->crd_skip;
			     i < crd->crd_skip + crd->crd_len; i += blks) {
				if (crd->crd_flags & CRD_F_ENCRYPT) {
					exf->encrypt(sw->sw_kschedule, buf + i);
				} else {
					exf->decrypt(sw->sw_kschedule, buf + i);
				}
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
				if (exf->reinit) {
					if (crd->crd_flags & CRD_F_ENCRYPT) {
						exf->encrypt(sw->sw_kschedule,
							     blk);
					} else {
						exf->decrypt(sw->sw_kschedule,
							     blk);
					}
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
				if (exf->reinit) {
					if (crd->crd_flags & CRD_F_ENCRYPT) {
						exf->encrypt(sw->sw_kschedule,
							     idat);
					} else {
						exf->decrypt(sw->sw_kschedule,
							     idat);
					}
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
				if (exf->reinit) {
					if (crd->crd_flags & CRD_F_ENCRYPT) {
						exf->encrypt(sw->sw_kschedule,
							     blk);
					} else {
						exf->decrypt(sw->sw_kschedule,
							     blk);
					}
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

			while (uio->uio_iov[ind].iov_len >= k + blks &&
			    i > 0) {
				if (exf->reinit) {
					if (crd->crd_flags & CRD_F_ENCRYPT) {
						exf->encrypt(sw->sw_kschedule,
							    idat);
					} else {
						exf->decrypt(sw->sw_kschedule,
							    idat);
					}
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
  int type;
  
  if (crp == NULL || crp->crp_callback == NULL || sc == NULL) {
    return (EINVAL);
  }
  
  u_int32_t ctrl;
  int ctr;
  /* aprint_normal_dev(sc->sc_dev, "process: %d %d\n", crp->crp_ilen, crp->crp_olen); */
  if (CRYPTO_SESID2LID(crp->crp_sid) != sc->sid)
    return (EINVAL);

  /* u_int64_t tmp_iv[2]; */
  /* memcpy(tmp_iv, crp->tmp_iv, 16); */
  /* aprint_normal_dev(sc->sc_dev, "prcess: iv: (%p) 0x%016llx 0x%016llx\n", crp->tmp_iv, tmp_iv[0], tmp_iv[1]); */
  
  /*  u_int64_t data[2]; */
 
  if (crp->crp_flags & CRYPTO_F_IMBUF) {
    type = CRYPTO_BUF_MBUF;
  } else if (crp->crp_flags & CRYPTO_F_IOV) {
    type = CRYPTO_BUF_IOV;
  } else {
    type = CRYPTO_BUF_CONTIG;
  }

  ctr = 0;
  while (((ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL)) != 0) &&
	 (ctr < 5)) {
    delay(1);
    ctr ++;
  }
  if (ctrl)
    aprint_error_dev(sc->sc_dev, "process: stuck (%x, %d)\n", ctrl, ctr);
  
  for (crd = crp->crp_desc; crd != NULL; crd = crd->crd_next) {
#if 0
    int len = crd->crd_len;
    if (clen % 16)
      return EINVAL;
#endif
    /* aprint_normal_dev(sc->sc_dev, "process: %p (%d)\n", crd, crd->crd_len); */
    
    int res = rdfpga_encdec_aes128(sc, crd, crp->crp_buf, type);
    if (res)
      return res;
    crypto_done(crp);
    
#if 0
    u_int8_t* buf = ((u_int8_t*)crp->crp_buf) + crd->crd_skip;
  
  while (len >= 16) {
    int ctr = 0, i;
    u_int64_t tmp_iv[2];
    memcpy(tmp_iv, crp->tmp_iv, 16);
    for (i = 0 ; i < 2 ; i++)
      bus_space_write_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_DATA + (i*8)), tmp_iv[i]);
    ctrl = RDFPGA_MASK_AES128_START | RDFPGA_MASK_AES128_NEWKEY;
    bus_space_write_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL, ctrl);
    
    ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL);
    while (ctrl && (ctr < 12)) {
      ctrl = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs, RDFPGA_REG_AES128_CTRL);
      ctr ++;
    }
    if (ctrl)
      return EBUSY;
    for (i = 0 ; i < 2 ; i++)
      data[i] = bus_space_read_8(sc->sc_bustag, sc->sc_bhregs, (RDFPGA_REG_AES128_OUT + (i*8)));
    
    for (i = 0 ; i < 16 ; i++) {
      buf[i] ^= ((u_int8_t*)data)[i];
    }
    len -= 16;
      buf += 16;
  }
  #endif
  }

  return 0;
}
