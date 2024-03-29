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
#include <sys/ioccom.h>

#include <sys/mman.h>
#include <sys/param.h>
#include <uvm/uvm_extern.h>
#include <sys/kmem.h>

#include <dev/sbus/sbusvar.h>

#include <dev/sbus/sbusfpga_curve25519engine.h>

#include <machine/param.h>

int	sbusfpga_curve25519engine_print(void *, const char *);
int	sbusfpga_curve25519engine_match(device_t, cfdata_t, void *);
void	sbusfpga_curve25519engine_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(sbusfpga_c29e, sizeof(struct sbusfpga_curve25519engine_softc),
    sbusfpga_curve25519engine_match, sbusfpga_curve25519engine_attach, NULL, NULL);

dev_type_open(sbusfpga_curve25519engine_open);
dev_type_close(sbusfpga_curve25519engine_close);
dev_type_ioctl(sbusfpga_curve25519engine_ioctl);
dev_type_mmap(sbusfpga_curve25519engine_mmap);



const struct cdevsw sbusfpga_c29e_cdevsw = {
	.d_open = sbusfpga_curve25519engine_open,
	.d_close = sbusfpga_curve25519engine_close,
	.d_read = noread,
	.d_write = nowrite,
	.d_ioctl = sbusfpga_curve25519engine_ioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = sbusfpga_curve25519engine_mmap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = 0
};

extern struct cfdriver sbusfpga_c29e_cd;

struct sbusfpga_curve25519engine_montgomeryjob {
	/* uint32_t x0_u[8]; */
	/* uint32_t x0_w[8]; */
	/* uint32_t x1_u[8]; */
	/* uint32_t x1_w[8]; */
	uint32_t affine_u[8];
	uint32_t scalar[8];
};
struct sbusfpga_curve25519engine_aesjob {
	uint32_t data[8];
	uint32_t keys[120];
};

static int init_programs(struct sbusfpga_curve25519engine_softc *sc);
static int write_inputs(struct sbusfpga_curve25519engine_softc *sc, struct sbusfpga_curve25519engine_montgomeryjob *job, const int window);
static int start_job(struct sbusfpga_curve25519engine_softc *sc);
static int wait_job(struct sbusfpga_curve25519engine_softc *sc, uint32_t param);
static int read_outputs(struct sbusfpga_curve25519engine_softc *sc, struct sbusfpga_curve25519engine_montgomeryjob *job, const int window);
static int dma_init(struct sbusfpga_curve25519engine_softc *sc);

static int power_on(struct sbusfpga_curve25519engine_softc *sc);
static int power_off(struct sbusfpga_curve25519engine_softc *sc);

int
sbusfpga_curve25519engine_open(dev_t dev, int flags, int mode, struct lwp *l)
{
	int unit = minor(dev) & (MAX_SESSION - 1);
	int driver = unit & ~(MAX_SESSION - 1);
	struct sbusfpga_curve25519engine_softc *sc = device_lookup_private(&sbusfpga_c29e_cd, driver);

	if (sc == NULL)
		return ENODEV;

	if ((unit != 0) && ((sc->active_sessions & (1 << unit)) == 0)) {
		return ENODEV;
	}
	
	/* first we need to turn the engine power on ... */
	power_on(sc);
	
	return (0);
}

int
sbusfpga_curve25519engine_close(dev_t dev, int flags, int mode, struct lwp *l)
{
	int unit = minor(dev) & (MAX_SESSION - 1);
	int driver = unit & ~(MAX_SESSION - 1);
	struct sbusfpga_curve25519engine_softc *sc = device_lookup_private(&sbusfpga_c29e_cd, driver);

	if (sc == NULL)
		return ENODEV;

	if ((unit != 0) && (sc->active_sessions & (1 << unit))) {
		device_printf(sc->sc_dev, "warning: close() on active session\n");
		sc->active_sessions &= ~(1 << unit);
		sc->mapped_sessions &= ~(1 << unit);
	}

	if (sc->active_sessions == 0)
		power_off(sc);
	
	return (0);
}

int
sbusfpga_curve25519engine_print(void *aux, const char *busname)
{

	sbus_print(aux, busname);
	return (UNCONF);
}

int
sbusfpga_curve25519engine_match(device_t parent, cfdata_t cf, void *aux)
{
	struct sbus_attach_args *sa = (struct sbus_attach_args *)aux;

	return (strcmp("betrustedc25519e", sa->sa_name) == 0);
}

static const uint32_t program_ec25519[134] = {0x00640840, 0x00680800, 0x006c0600, 0x00700840, 0x004c0a80, 0x00480800, 0x007407cc, 0x007c07cb, 0x0049d483, 0x0079b643, 0x0079e482, 0x00659783, 0x006db783, 0x0079c683, 0x0079e482, 0x0069a783, 0x0071c783, 0x00480740, 0x0001a645, 0x00780008, 0x0001e006, 0x0069a8c6, 0x0005a645, 0x00780048, 0x0005e046, 0x0009c6c5, 0x00780088, 0x0009e086, 0x0071c8c6, 0x000dc6c5, 0x007800c8, 0x000de0c6, 0x00100007, 0x00141047, 0x007458c6, 0x0019d105, 0x00780188, 0x0019e186, 0x001c3007, 0x00202047, 0x002481c5, 0x00780248, 0x0025e246, 0x007488c6, 0x0029d1c5, 0x00780288, 0x0029e286, 0x006c9247, 0x0030a287, 0x00346907, 0x00645107, 0x003c5345, 0x007803c8, 0x003de3c6, 0x0068f187, 0x0070c607, 0x010004c9, 0x004e14c6, 0xe5800809, 0x0079b643, 0x0079e482, 0x00659783, 0x006db783, 0x0079c683, 0x0079e482, 0x0069a783, 0x0071c783, 0x00740640, 0x00780680, 0x0001e787, 0x00040007, 0x00041047, 0x00081787, 0x000c2007, 0x001030c7, 0x00144087, 0x00700940, 0x00185147, 0x00721706, 0x01000709, 0x00186187, 0xfe000809, 0x001c5187, 0x00700980, 0x002071c7, 0x00721706, 0x01000709, 0x00208207, 0xfe000809, 0x00247207, 0x007009c0, 0x00289247, 0x00721706, 0x01000709, 0x0028a287, 0xfe000809, 0x002c9287, 0x00700980, 0x0030b2c7, 0x00721706, 0x01000709, 0x0030c307, 0xfe000809, 0x00347307, 0x00700a00, 0x0038d347, 0x00721706, 0x01000709, 0x0038e387, 0xfe000809, 0x003cd387, 0x00700a40, 0x0040f3c7, 0x00721706, 0x01000709, 0x00410407, 0xfe000809, 0x0044f407, 0x00700a00, 0x00491447, 0x00721706, 0x01000709, 0x00492487, 0xfe000809, 0x004cd487, 0x00700940, 0x005134c7, 0x00721706, 0x01000709, 0x00514507, 0xfe000809, 0x00543507, 0x007d5747, 0x0000000a };

static const uint32_t program_gcm[20] = {0x0010100d, 0x0094100d, 0x0118100d, 0x019c100d, 0x00186143, 0x00160191, 0x00186811, 0x001c61c3, 0x00105103, 0x008441ce, 0x0082010e, 0x00080010, 0x008e008f, 0x0112008f, 0x0396008f, 0x00083083, 0x00105103, 0x00084083, 0x00001083, 0x0000000a };

static const uint32_t program_aes[16] = {0x0001f003,0x0005e012,0x0001d052,0x0005c012,0x0001b052,0x0005a012,0x00019052,0x00058012,0x00017052,0x00056012,0x00015052,0x00054012,0x00013052,0x00052012,0x00811052,0x0000000a };

static const uint32_t program_gcm_pfx[30] = {0x01400411,0x00080840,0x00040800,0x0001f043,0x0005e012,0x0001d052,0x0005c012,0x0001b052,0x0005a012,0x00019052,0x00058012,0x00017052,0x00056012,0x00015052,0x00054012,0x00013052,0x00052012,0x00811052,0x03800089,0x003c0000,0x01400411,0x0042b405,0x01400411,0x00080800,0x00040400,0xf4800809,0x00380000,0x01bc03d1,0x003cf3d1,0x00340800 };

static const uint32_t program_gcm_ad[29] = {0x0d800309,0x000000d3,0x01800011,0x00000011,0x0000d003,0x000f00c5,0x00321306,0x0010f00d,0x0094f00d,0x0118f00d,0x019cf00d,0x00186143,0x00160191,0x00186811,0x001c61c3,0x00105103,0x008441ce,0x0082010e,0x00080010,0x009a008f,0x0112008f,0x0396008f,0x00086083,0x00105103,0x00084083,0x00341083,0x00800309,0xf2800809,0x0000000a };

static const uint32_t program_gcm_aes[50] = {0x18000309,0x01400411,0x0042b405,0x01400411,0x0001f403,0x0005e012,0x0001d052,0x0005c012,0x0001b052,0x0005a012,0x00019052,0x00058012,0x00017052,0x00056012,0x00015052,0x00054012,0x00013052,0x00052012,0x00851052,0x000000d3,0x00001003,0x00ac02d3,0x01800011,0x00000011,0x0000d003,0x000f00c5,0x002f02c5,0x00321306,0x0010f00d,0x0094f00d,0x0118f00d,0x019cf00d,0x00186143,0x00160191,0x00186811,0x001c61c3,0x00105103,0x008441ce,0x0082010e,0x00080010,0x009a008f,0x0112008f,0x0396008f,0x00086083,0x00105103,0x00084083,0x00341083,0x00800309,0xe8000809,0x0000000a };

static const uint32_t program_gcm_finish[71] = {0x16000309,0x01400411,0x0042b405,0x01400411,0x0001f403,0x0005e012,0x0001d052,0x0005c012,0x0001b052,0x0005a012,0x00019052,0x00058012,0x00017052,0x00056012,0x00015052,0x00054012,0x00013052,0x00052012,0x00851052,0x0004a054,0x000000d3,0x00001003,0x00ac02d3,0x01800011,0x00000011,0x0000d003,0x0010f00d,0x0094f00d,0x0118f00d,0x019cf00d,0x00186143,0x00160191,0x00186811,0x001c61c3,0x00105103,0x008441ce,0x0082010e,0x00080010,0x009a008f,0x0112008f,0x0396008f,0x00086083,0x00105103,0x00084083,0x00341083,0x01a40251,0x00249251,0x0000d243,0x0010f00d,0x0094f00d,0x0118f00d,0x019cf00d,0x00186143,0x00160191,0x00186811,0x001c61c3,0x00105103,0x008441ce,0x0082010e,0x00080010,0x009a008f,0x0112008f,0x0396008f,0x00086083,0x00105103,0x00084083,0x00341083,0x01b40351,0x0034d351,0x0020e343,0x0000000a };

// second and third are for testing and shall be removed
static const uint32_t* programs[8] = { program_ec25519, program_gcm, program_aes, program_gcm_pfx, program_gcm_ad, program_gcm_aes, program_gcm_finish, NULL };
static const uint32_t program_len[8] = { 134, 20, 16, 30, 29, 50, 71, 0 };
static       uint32_t program_offset[8];

/*
 * Attach all the sub-devices we can find
 */
void
sbusfpga_curve25519engine_attach(device_t parent, device_t self, void *aux)
{
	struct sbus_attach_args *sa = aux;
	struct sbusfpga_curve25519engine_softc *sc = device_private(self);
	struct sbus_softc *sbsc = device_private(parent);
	int node;
	int sbusburst;
		
	sc->sc_bustag = sa->sa_bustag;
	sc->sc_dmatag = sa->sa_dmatag;
	sc->sc_dev = self;

	aprint_normal("\n");

	if (sa->sa_nreg < 3) {
		aprint_error(": Not enough registers spaces\n");
		return;
	}

	/* map registers */
	if (sbus_bus_map(sc->sc_bustag,
					 sa->sa_reg[0].oa_space /* sa_slot */,
					 sa->sa_reg[0].oa_base /* sa_offset */,
					 sa->sa_reg[0].oa_size /* sa_size */,
					 BUS_SPACE_MAP_LINEAR,
					 &sc->sc_bhregs_curve25519engine) != 0) {
		aprint_error(": cannot map Curve25519Engine registers\n");
		return;
	} else {
		aprint_normal_dev(self, "Curve25519Engine registers @ %p\n", (void*)sc->sc_bhregs_curve25519engine);
	}
	/* map microcode */
	if (sbus_bus_map(sc->sc_bustag,
					 sa->sa_reg[1].oa_space /* sa_slot */,
					 sa->sa_reg[1].oa_base /* sa_offset */,
					 sa->sa_reg[1].oa_size /* sa_size */,
					 BUS_SPACE_MAP_LINEAR,
					 &sc->sc_bhregs_microcode) != 0) {
		aprint_error(": cannot map Curve25519Engine microcode\n");
		return;
	} else {
		aprint_normal_dev(self, "Curve25519Engine microcode @ %p\n", (void*)sc->sc_bhregs_microcode);
	}
	/* map register file */
	if (sbus_bus_map(sc->sc_bustag,
					 sa->sa_reg[2].oa_space /* sa_slot */,
					 sa->sa_reg[2].oa_base /* sa_offset */,
					 sa->sa_reg[2].oa_size /* sa_size */,
					 BUS_SPACE_MAP_LINEAR,
					 &sc->sc_bhregs_regfile) != 0) {
		aprint_error(": cannot map Curve25519Engine regfile\n");
		return;
	} else {
		aprint_normal_dev(self, "Curve25519Engine regfile @ %p\n", (void*)sc->sc_bhregs_regfile);
	}
	sc->sc_bufsiz_curve25519engine = sa->sa_reg[0].oa_size;
	sc->sc_bufsiz_microcode = sa->sa_reg[1].oa_size;
	sc->sc_bufsiz_regfile = sa->sa_reg[2].oa_size;

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

	/* first we need to turn the engine power on ... */
	power_on(sc);

	if (init_programs(sc)) {
		if (init_programs(sc)) {
			aprint_normal_dev(sc->sc_dev, "INIT - FAILED\n");
			sc->initialized = 0;
		} else {
			sc->initialized = 1;
		}	
	} else {
		sc->initialized = 1;
	}

	power_off(sc);

	sc->active_sessions = 0;
	sc->mapped_sessions = 0;

	if (!dma_init(sc)) {
		// ouch
		sc->active_sessions = 0xFFFFFFFF;
		sc->mapped_sessions = 0xFFFFFFFF;
	}
}

#define CONFIG_CSR_DATA_WIDTH 32
#include "dev/sbus/sbusfpga_csr_curve25519engine.h"

#define REG_BASE(reg) (base + (reg * 32))
#define SUBREG_ADDR(reg, off) (REG_BASE(reg) + (off)*4)

#include <sys/cprng.h>
//cprng_strong32()
struct sbusfpga_curve25519engine_session {
	uint32_t session;
	uint32_t cookie;
};
struct sbusfpga_curve25519engine_session_len {
	uint32_t session;
	uint32_t cookie;
	uint32_t len;
};
struct sbusfpga_curve25519engine_session_len_data {
	uint32_t session;
	uint32_t cookie;
	uint32_t len;
	uint32_t data[8];
	uint32_t keys[60];
};
struct sbusfpga_curve25519engine_session_len_final {
	uint32_t session;
	uint32_t cookie;
	uint32_t len;
	uint32_t data[8];
};

#define CHECKSESSION(ses)												\
	do {																\
		if ((ses->session >= MAX_ACTIVE_SESSION) || (ses->session >= MAX_SESSION)) \
			return EINVAL;												\
		if (sc->sessions_cookies[ses->session] == 0)					\
			return EINVAL;												\
		if (sc->sessions_cookies[ses->session] != ses->cookie)			\
			return EINVAL;												\
		if (ses->session != unit)										\
			return EINVAL;												\
		if ((sc->active_sessions & (1 << ses->session)) == 0)			\
			return EINVAL;												\
	} while (0)

#define SBUSFPGA_DO_MONTGOMERYJOB   _IOWR(0, 0, struct sbusfpga_curve25519engine_montgomeryjob)
#define SBUSFPGA_EC25519_CHECKGCM   _IOW(0, 1, struct sbusfpga_curve25519engine_montgomeryjob)
#define SBUSFPGA_EC25519_CHECKAES   _IOW(0, 2, struct sbusfpga_curve25519engine_aesjob)

#define SBUSFPGA_EC25519_GETSESSION    _IOR(1, 0, struct sbusfpga_curve25519engine_session)
#define SBUSFPGA_EC25519_OPENSESSION   _IOW(1, 1, struct sbusfpga_curve25519engine_session)
#define SBUSFPGA_EC25519_CLOSESESSION  _IOW(1, 2, struct sbusfpga_curve25519engine_session)
#define SBUSFPGA_EC25519_GCMPFX        _IOW(1, 3, struct sbusfpga_curve25519engine_session_len_data)
#define SBUSFPGA_EC25519_GCMAD         _IOW(1, 4, struct sbusfpga_curve25519engine_session_len)
#define SBUSFPGA_EC25519_GCMAES        _IOW(1, 5, struct sbusfpga_curve25519engine_session_len)
#define SBUSFPGA_EC25519_GCMFINISH     _IOWR(1, 6, struct sbusfpga_curve25519engine_session_len_final)

static int get_session(struct sbusfpga_curve25519engine_softc *sc) {
	int i;
	/* don't use 0, we use it for testing */
	/* also minor 0 is used to request session, 1-7 to open/close/map using session # */
	for (i = 1 ; (i < MAX_ACTIVE_SESSION) && (i < MAX_SESSION) ; i++) {
		if (((sc->active_sessions & (1<<i)) == 0) && ((sc->mapped_sessions & (1<<i)) == 0)) {
			sc->active_sessions |= (1<<i);
			return i;
		}
	}
	return -1;
}

int
sbusfpga_curve25519engine_ioctl (dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	int unit = minor(dev) & (MAX_SESSION - 1);
	int driver = unit & ~(MAX_SESSION - 1);
	struct sbusfpga_curve25519engine_softc *sc = device_lookup_private(&sbusfpga_c29e_cd, driver);
	int err = 0;

	if (sc == NULL) {
		return ENODEV;
	}

	if (!sc->initialized) {
		if (init_programs(sc)) {
			return ENXIO;
		} else {
			sc->initialized = 1;
		}
	}
	switch (cmd) {
	case SBUSFPGA_DO_MONTGOMERYJOB: {
		if (unit != 0)
			return ENOTTY;
		
		struct sbusfpga_curve25519engine_montgomeryjob* job = (struct sbusfpga_curve25519engine_montgomeryjob*)data;
		curve25519engine_mpstart_write(sc, program_offset[0]); /* EC25519 */
		curve25519engine_mplen_write(sc, program_len[0]); /* EC25519 */
	
		err = write_inputs(sc, job, 0);
		if (err)
			return err;
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, 1);
		if (err)
			return err;
		err = read_outputs(sc, job, 0);
		if (err)
			return err;
	}
		break;
	case SBUSFPGA_EC25519_CHECKGCM: {
		if (unit != 0)
			return ENOTTY;
		
		const uint32_t base = 0;
		struct sbusfpga_curve25519engine_montgomeryjob* job = (struct sbusfpga_curve25519engine_montgomeryjob*)data;
		int reg, i;
		
		curve25519engine_mpstart_write(sc, program_offset[1]); /* GCM */
		curve25519engine_mplen_write(sc, program_len[1]); /* GCM */
		for (i = 0 ; i < 8 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(0,i), job->affine_u[i]);
		}
		for (i = 0 ; i < 8 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(1,i), job->scalar[i]);
		}
		
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, 1);
		/* if (err) */
		/* 	return err; */

		for (reg = 0 ; reg < 32 ; reg++) {
			uint32_t buf[8];
			for (i = 0 ; i < 8 ; i ++) {
				buf[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i));
			}
			device_printf(sc->sc_dev, "GCM %d: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x \n", reg,
						  buf[0],  buf[1],  buf[2],  buf[3], buf[4],  buf[5],  buf[6],  buf[7]);
		}
	}
		break;
	case SBUSFPGA_EC25519_CHECKAES: {
		if (unit != 0)
			return ENOTTY;
		
		const uint32_t base = 0;
		struct sbusfpga_curve25519engine_aesjob* job = (struct sbusfpga_curve25519engine_aesjob*)data;
		int reg, i;
		
		curve25519engine_mpstart_write(sc, program_offset[2]); /* AES */
		curve25519engine_mplen_write(sc, program_len[2]); /* AES */
		for (i = 0 ; i < 8 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(0,i), job->data[i]);
		}
		for (reg = 31 ; reg > 16 ; reg--) {
			for (i = 0 ; i < 8 ; i ++) {
				bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i), job->keys[i+8*(31-reg)]);
			}
		}
		
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, 1);
		/* if (err) */
		/* 	return err; */

		for (reg = 0 ; reg < 32 ; reg++) {
			uint32_t buf[8];
			for (i = 0 ; i < 8 ; i ++) {
				buf[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i));
			}
			device_printf(sc->sc_dev, "AES %d: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x \n", reg,
						  buf[0],  buf[1],  buf[2],  buf[3], buf[4],  buf[5],  buf[6],  buf[7]);
		}
	}
		break;
		
	case SBUSFPGA_EC25519_GCMPFX: {
		if (unit == 0)
			return ENOTTY;

		/* FIXME: need a lock!!! */
		
		const uint32_t base = unit * 0x400;
		struct sbusfpga_curve25519engine_session_len_data* job = (struct sbusfpga_curve25519engine_session_len_data*)data;
		int reg, i;
		void* rd_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096)       );
		//void* wr_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096) + 2048);

		CHECKSESSION(job);

		if (job->len > 128) {
			device_printf(sc->sc_dev, "job->len too big: %u", job->len);
			return EINVAL;
		}
		
		curve25519engine_mpstart_write(sc, program_offset[3]); /* GCM_PFX */
		curve25519engine_mplen_write(sc, program_len[3] + program_len[4]); /* GCM_PFX + GCM_AD */
		curve25519engine_window_write(sc, unit); /* to each session its own register file */

		/* read_addr */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(3,i), (i == 0) ? ((uint32_t)rd_ptr) : 0);
		}
		/* write_len */
		for (i = 0 ; i < 8 ; i ++) { // all the way to 8 to make sure we have zero in every bit checked by BRZ
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(12,i), (i == 0) ? ((uint32_t)job->len) : 0);
		}
		/* data */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(16,i), job->data[i]);
		}
		for (reg = 31 ; reg > 16 ; reg--) {
			for (i = 0 ; i < 4 ; i ++) {
				bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i), job->keys[i+4*(31-reg)]);
			}
		}
		
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, job->len);
		if (err)
			return err;

#if 0
		for (reg = 0 ; reg < 32 ; reg++) {
			uint32_t buf[8];
			for (i = 0 ; i < 8 ; i ++) {
				buf[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i));
			}
			device_printf(sc->sc_dev, "GCM_PFX %d: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x \n", reg,
						  buf[0],  buf[1],  buf[2],  buf[3], buf[4],  buf[5],  buf[6],  buf[7]);
		}
#endif
	}
		break;
		
	case SBUSFPGA_EC25519_GCMAD: {
		if (unit == 0)
			return ENOTTY;

		/* FIXME: need a lock!!! */
		
		const uint32_t base = unit * 0x400;
		struct sbusfpga_curve25519engine_session_len* job = (struct sbusfpga_curve25519engine_session_len*)data;
		int i;
		void* rd_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096)       );
		//void* wr_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096) + 2048);

		CHECKSESSION(job);

		if (job->len > 128)
			return EINVAL;
		
		curve25519engine_mpstart_write(sc, program_offset[4]); /* GCM_AES */
		curve25519engine_mplen_write(sc, program_len[4]); /* GCM_AES */
		curve25519engine_window_write(sc, unit); /* to each session its own register file */

		/* read_addr */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(3,i), (i == 0) ? ((uint32_t)rd_ptr) : 0);
		}
		/* write_len */
		for (i = 0 ; i < 8 ; i ++) { // all the way to 8 to make sure we have zero in every bit checked by BRZ
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(12,i), (i == 0) ? ((uint32_t)job->len) : 0);
		}
		
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, job->len);
		if (err)
			return err;

#if 0
		int reg;
		for (reg = 0 ; reg < 32 ; reg++) {
			uint32_t buf[8];
			for (i = 0 ; i < 8 ; i ++) {
				buf[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i));
			}
			device_printf(sc->sc_dev, "GCM_AD %d: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x \n", reg,
						  buf[0],  buf[1],  buf[2],  buf[3], buf[4],  buf[5],  buf[6],  buf[7]);
		}
#endif
	}
		break;
		
	case SBUSFPGA_EC25519_GCMAES: {
		if (unit == 0)
			return ENOTTY;

		/* FIXME: need a lock!!! */
		
		const uint32_t base = unit * 0x400;
		struct sbusfpga_curve25519engine_session_len* job = (struct sbusfpga_curve25519engine_session_len*)data;
		int i;
		void* rd_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096)       );
		void* wr_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096) + 2048);

		CHECKSESSION(job);

		if (job->len > 128)
			return EINVAL;
		
		curve25519engine_mpstart_write(sc, program_offset[5]); /* GCM_AES */
		curve25519engine_mplen_write(sc, program_len[5]); /* GCM_AES */
		curve25519engine_window_write(sc, unit); /* to each session its own register file */

		/* read_addr */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(3,i), (i == 0) ? ((uint32_t)rd_ptr) : 0);
		}
		/* write_addr */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(11,i), (i == 0) ? ((uint32_t)wr_ptr) : 0);
		}
		/* write_len */
		for (i = 0 ; i < 8 ; i ++) { // all the way to 8 to make sure we have zero in every bit checked by BRZ
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(12,i), (i == 0) ? ((uint32_t)job->len) : 0);
		}
		
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, job->len);
		if (err)
			return err;
#if 0
		int reg;
		for (reg = 0 ; reg < 32 ; reg++) {
			uint32_t buf[8];
			for (i = 0 ; i < 8 ; i ++) {
				buf[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i));
			}
			device_printf(sc->sc_dev, "GCM_AES %d: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x \n", reg,
						  buf[0],  buf[1],  buf[2],  buf[3], buf[4],  buf[5],  buf[6],  buf[7]);
		}
#endif
	}
		break;

		
	case SBUSFPGA_EC25519_GCMFINISH: {
		if (unit == 0)
			return ENOTTY;

		/* FIXME: need a lock!!! */
		
		const uint32_t base = unit * 0x400;
		struct sbusfpga_curve25519engine_session_len_final* job = (struct sbusfpga_curve25519engine_session_len_final*)data;
		int i;
		void* rd_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096)       );
		void* wr_ptr = (void*)(((vaddr_t)sc->sc_dmamap->dm_segs[0].ds_addr) + (unit * 4096) + 2048);

		CHECKSESSION(job);

		if (job->len > 15)
			return EINVAL;
		
		curve25519engine_mpstart_write(sc, program_offset[6]); /* GCM_FINISH */
		curve25519engine_mplen_write(sc, program_len[6]); /* GCM_FINISH */
		curve25519engine_window_write(sc, unit); /* to each session its own register file */

		/* read_addr */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(3,i), (i == 0) ? ((uint32_t)rd_ptr) : 0);
		}
		/* write_addr */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(11,i), (i == 0) ? ((uint32_t)wr_ptr) : 0);
		}
		/* write_len */
		for (i = 0 ; i < 8 ; i ++) { // all the way to 8 to make sure we have zero in every bit checked by BRZ
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(12,i), (i == 0) ? ((uint32_t)job->len) : 0);
		}
		/* final block */
		for (i = 0 ; i < 4 ; i ++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(9,i), job->data[i]);
		}
		/* create and generate MMASK */
		for (i = 0 ; i < 4 ; i ++) {
			uint32_t mask;
			int idx = i;
			if (job->len <= (idx*4)) {
				mask = 0;
			} else if (job->len >= (idx+1)*4) {
				mask = 0xFFFFFFFF;
			} else {
				mask = 0xFFFFFFFF >> (8*(4-(job->len%4)));
			}
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(10,i), mask);
		}
		
		
		err = start_job(sc);
		if (err)
			return err;
		delay(1);
		err = wait_job(sc, job->len);
		if (err)
			return err;

		/* final accum */
		for (i = 0 ; i < 4 ; i ++) {
			job->data[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(8,i));
		}

#if 0
		int reg;
		for (reg = 0 ; reg < 32 ; reg++) {
			uint32_t buf[8];
			for (i = 0 ; i < 8 ; i ++) {
				buf[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(reg,i));
			}
			device_printf(sc->sc_dev, "GCM_FINISH %d: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x \n", reg,
						  buf[0],  buf[1],  buf[2],  buf[3], buf[4],  buf[5],  buf[6],  buf[7]);
		}
#endif
	}
		break;
		
	case SBUSFPGA_EC25519_GETSESSION:{
		if (unit != 0)
			return ENOTTY;
		
		struct sbusfpga_curve25519engine_session* ses = (struct sbusfpga_curve25519engine_session*)data;
		int s = get_session(sc);
		if (s < 0)
			return EBUSY;
		ses->session = s;
		sc->sessions_cookies[s] = cprng_strong32();
		ses->cookie = sc->sessions_cookies[s];
	}
		break;
	case SBUSFPGA_EC25519_OPENSESSION:{
		if (unit == 0)
			return ENOTTY;
  
		struct sbusfpga_curve25519engine_session* ses = (struct sbusfpga_curve25519engine_session*)data;
		CHECKSESSION(ses);
		if ((sc->mapped_sessions & (1 << ses->session)) != 0)
			return EINVAL;
	}
		break;
	case SBUSFPGA_EC25519_CLOSESESSION:{
		if (unit == 0)
			return ENOTTY;
		
		struct sbusfpga_curve25519engine_session* ses = (struct sbusfpga_curve25519engine_session*)data;

		CHECKSESSION(ses);
		
		/* if ((sc->mapped_sessions & (1 << ses->session)) != 0) */
		/* 	return EBUSY; */
		sc->sessions_cookies[ses->session] = 0;
		sc->active_sessions &= ~(1 << ses->session);
		sc->mapped_sessions &= ~(1 << ses->session); // FIXME
	}
		break;
		
	default:
		err = EINVAL;
		break;
	}

	return(err);
}


static int power_on(struct sbusfpga_curve25519engine_softc *sc) {
	int err = 0;
	if ((curve25519engine_power_read(sc) & 1) == 0) {
		curve25519engine_power_write(sc, 1);
		delay(1);
	}
	return err;
}
static int power_off(struct sbusfpga_curve25519engine_softc *sc) {
	int err = 0;
	curve25519engine_power_write(sc, 0);
	return err;
}

static int init_programs(struct sbusfpga_curve25519engine_softc *sc) {
	/* the microcode is a the beginning */
	int err = 0;
	uint32_t i, j;
	uint32_t offset = 0;

	for (j = 0 ; programs[j] != NULL; j ++) {
		program_offset[j] = offset;
		for (i = 0 ; i < program_len[j] ; i++) {
			bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_microcode, ((offset+i)*4), programs[j][i]);
			if ((i%16)==15)
				delay(1);
		}
		offset += program_len[j];
	}

	curve25519engine_window_write(sc, 0); /* could use window_window to access fields, but it creates a RMW cycle for nothing */
	curve25519engine_mpstart_write(sc, 0); /* EC25519 */
	curve25519engine_mplen_write(sc, program_len[0]); /* EC25519 */

	aprint_normal_dev(sc->sc_dev, "INIT - Curve25519Engine status: 0x%08x\n", curve25519engine_status_read(sc));

#if 1
	/* double check */
	u_int32_t x;
	int count = 0;
	for (i = 0 ; i < program_len[0] && count < 10; i++) {
		x = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_microcode, (i*4));
		if (x != programs[0][i]) {
			aprint_error_dev(sc->sc_dev, "INIT - Curve25519Engine program failure: [%d] 0x%08x <> 0x%08x\n", i, x, programs[0][i]);
			err = 1;
			count ++;
		}
		if ((i%8)==7)
			delay(1);
	}
	if ((x = curve25519engine_window_read(sc)) != 0) {
			aprint_error_dev(sc->sc_dev, "INIT - Curve25519Engine register failure: window = 0x%08x\n", x);
			err = 1;
	}
	if ((x = curve25519engine_mpstart_read(sc)) != 0) {
			aprint_error_dev(sc->sc_dev, "INIT - Curve25519Engine register failure: mpstart = 0x%08x\n", x);
			err = 1;
	}
	if ((x = curve25519engine_mplen_read(sc)) != program_len[0]) {
			aprint_error_dev(sc->sc_dev, "INIT - Curve25519Engine register failure: mplen = 0x%08x\n", x);
			err = 1;
	}
	const int test_reg_num = 73;
	const uint32_t test_reg_value = 0x0C0FFEE0;
	bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile, 4*test_reg_num, test_reg_value);
	delay(1);
	if ((x = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile, 4*test_reg_num)) != test_reg_value) {
		aprint_error_dev(sc->sc_dev, "INIT - Curve25519Engine register file failure: 0x%08x != 0x%08x\n", x, test_reg_value);
		err = 1;
	}
#endif
	
	return err;
}

static int write_inputs(struct sbusfpga_curve25519engine_softc *sc, struct sbusfpga_curve25519engine_montgomeryjob *job, const int window) {
	const uint32_t base = window * 0x400;
	int i;
	uint32_t status = curve25519engine_status_read(sc);
	int err = 0;
	if (status & (1<<CSR_CURVE25519ENGINE_STATUS_RUNNING_OFFSET)) {
		aprint_error_dev(sc->sc_dev, "WRITE - Curve25519Engine status: 0x%08x, still running?\n", status);
		return ENXIO;
	}
	for (i = 0 ; i < 8 ; i ++) {
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(24,i), job->affine_u[i]);
		/* bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(25,i), job->x0_u[i]); */
		/* bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(26,i), job->x0_w[i]); */
		/* bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(27,i), job->x1_u[i]); */
		/* bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(28,i), job->x1_w[i]); */
		bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(31,i), job->scalar[i]);
		/* bus_space_write_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(19,i), ((i == 0) ? 254 : 0)); */
		/* delay(1); */
	}

#if 1
	for (i = 0 ; i < 8 && !err; i ++) {
		if (job->affine_u[i] != bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(24,i))) err = EIO;
		/* if (job->x0_u[i]     != bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(25,i))) err = EIO; */
		/* if (job->x0_w[i]     != bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(26,i))) err = EIO; */
		/* if (job->x1_u[i]     != bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(27,i))) err = EIO; */
		/* if (job->x1_w[i]     != bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(28,i))) err = EIO; */
		if (job->scalar[i]   != bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(31,i))) err = EIO;
		/* delay(1); */
	}
	if (err) aprint_error_dev(sc->sc_dev, "WRITE - data did not read-write properly\n");
#endif

	return err;
}

static int start_job(struct sbusfpga_curve25519engine_softc *sc) {
	uint32_t status = curve25519engine_status_read(sc);
	if (status & (1<<CSR_CURVE25519ENGINE_STATUS_RUNNING_OFFSET)) {
		aprint_error_dev(sc->sc_dev, "START - Curve25519Engine status: 0x%08x, still running?\n", status);
		return ENXIO;
	}
	curve25519engine_control_write(sc, 1);
	//aprint_normal_dev(sc->sc_dev, "START - Curve25519Engine status: 0x%08x\n", curve25519engine_status_read(sc));
	
	return 0;
}

static int wait_job(struct sbusfpga_curve25519engine_softc *sc, uint32_t param) {
	uint32_t status = curve25519engine_status_read(sc);
	int count = 0;
	int max_count = 250;
	int del = 1;
	const int max_del = 32;
	static int max_del_seen = 1;
	static int max_cnt_seen = 0;
	
	while ((status & (1<<CSR_CURVE25519ENGINE_STATUS_RUNNING_OFFSET)) && (count < max_count)) {
		//uint32_t ls_status = curve25519engine_ls_status_read(sc);
		//aprint_normal_dev(sc->sc_dev, "WAIT - ongoing, Curve25519Engine status: 0x%08x [%d] ls_status: 0x%08x\n", status, count, ls_status);
		count ++;
		delay(del);
		del = del < max_del ? 2*del : del;
		status = curve25519engine_status_read(sc);
	}
	if (del > max_del_seen) {
		max_del_seen = del;
		aprint_normal_dev(sc->sc_dev, "WAIT - new max delay %d after %d count (param was %u)\n", max_del_seen, count, param);
	}
	if (count > max_cnt_seen) {
		max_cnt_seen = count;
		aprint_normal_dev(sc->sc_dev, "WAIT - new max count %d with %d delay (param was %u)\n", max_cnt_seen, del, param);
		
	}
	
	//curve25519engine_control_write(sc, 0);
	if (status & (1<<CSR_CURVE25519ENGINE_STATUS_RUNNING_OFFSET)) {
		aprint_error_dev(sc->sc_dev, "WAIT - Curve25519Engine status: 0x%08x, did not finish in time? [inst: 0x%08x ls_status: 0x%08x]\n", status, curve25519engine_instruction_read(sc),  curve25519engine_ls_status_read(sc));
		return ENXIO;
	} else if (status & (1<<CSR_CURVE25519ENGINE_STATUS_SIGILL_OFFSET)) {
		aprint_error_dev(sc->sc_dev, "WAIT - Curve25519Engine status: 0x%08x, sigill [inst: 0x%08x ls_status: 0x%08x]\n", status, curve25519engine_instruction_read(sc),  curve25519engine_ls_status_read(sc));
		return ENXIO;
	} else if (status & (1<<CSR_CURVE25519ENGINE_STATUS_ABORT_OFFSET)) {
		aprint_error_dev(sc->sc_dev, "WAIT - Curve25519Engine status: 0x%08x, aborted [inst: 0x%08x ls_status: 0x%08x]\n", status, curve25519engine_instruction_read(sc),  curve25519engine_ls_status_read(sc));
		return ENXIO;
	} else {
		//aprint_normal_dev(sc->sc_dev, "WAIT - Curve25519Engine status: 0x%08x [%d] ls_status: 0x%08x\n", status, count, curve25519engine_ls_status_read(sc));
	}

	return 0;
}

static int read_outputs(struct sbusfpga_curve25519engine_softc *sc, struct sbusfpga_curve25519engine_montgomeryjob *job, const int window) {
	const uint32_t base = window * 0x400;
	int i;
	uint32_t status = curve25519engine_status_read(sc);
	if (status & (1<<CSR_CURVE25519ENGINE_STATUS_RUNNING_OFFSET)) {
		aprint_error_dev(sc->sc_dev, "READ - Curve25519Engine status: 0x%08x, still running?\n", status);
		return ENXIO;
	}
	
	for (i = 0 ; i < 8 ; i ++) {
		/* job->affine_u[i] = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(24,i)); */
		/* job->x0_u[i]     = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(25,i)); */
		/* job->x0_w[i]     = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(26,i)); */
		/* job->x1_u[i]     = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(27,i)); */
		/* job->x1_w[i]     = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(28,i)); */
		job->scalar[i]   = bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(0,i));
		/* delay(1); */
	}
	aprint_normal_dev(sc->sc_dev, "READ - Curve25519Engine 19 low 32 bits: 0x%08x\n", bus_space_read_4(sc->sc_bustag, sc->sc_bhregs_regfile,SUBREG_ADDR(19,0)));

	return 0;
}


static int
dma_init(struct sbusfpga_curve25519engine_softc *sc) {
	
	/* Allocate a dmamap */
	if (bus_dmamap_create(sc->sc_dmatag, SBUSFPGA_CURVE25519ENGINE_VAL_DMA_MAX_SZ, 1, SBUSFPGA_CURVE25519ENGINE_VAL_DMA_MAX_SZ, 0, BUS_DMA_NOWAIT | BUS_DMA_ALLOCNOW, &sc->sc_dmamap) != 0) {
		aprint_error_dev(sc->sc_dev, "DMA map create failed\n");
		return 0;
	} else {
		aprint_normal_dev(sc->sc_dev, "dmamap: %lu %lu %d (%p)\n", sc->sc_dmamap->dm_maxsegsz, sc->sc_dmamap->dm_mapsize, sc->sc_dmamap->dm_nsegs, sc->sc_dmatag->_dmamap_load);
	}

	if (bus_dmamem_alloc(sc->sc_dmatag, SBUSFPGA_CURVE25519ENGINE_VAL_DMA_MAX_SZ, 64, 64, &sc->sc_segs, 1, &sc->sc_rsegs, BUS_DMA_NOWAIT | BUS_DMA_STREAMING)) {
		aprint_error_dev(sc->sc_dev, "cannot allocate DVMA memory");
		bus_dmamap_destroy(sc->sc_dmatag, sc->sc_dmamap);
		return 0;
	}
  
	if (bus_dmamem_map(sc->sc_dmatag, &sc->sc_segs, 1, SBUSFPGA_CURVE25519ENGINE_VAL_DMA_MAX_SZ, &sc->sc_dma_kva, BUS_DMA_NOWAIT)) {
		aprint_error_dev(sc->sc_dev, "cannot allocate DVMA address");
		bus_dmamem_free(sc->sc_dmatag, &sc->sc_segs, 1);
		bus_dmamap_destroy(sc->sc_dmatag, sc->sc_dmamap);
		return 0;
	}
  
	if (bus_dmamap_load(sc->sc_dmatag, sc->sc_dmamap, sc->sc_dma_kva, SBUSFPGA_CURVE25519ENGINE_VAL_DMA_MAX_SZ, /* kernel space */ NULL,
						BUS_DMA_NOWAIT | BUS_DMA_STREAMING | BUS_DMA_WRITE)) {
		aprint_error_dev(sc->sc_dev, "cannot load dma map");
		bus_dmamem_unmap(sc->sc_dmatag, &sc->sc_dma_kva, SBUSFPGA_CURVE25519ENGINE_VAL_DMA_MAX_SZ);
		bus_dmamem_free(sc->sc_dmatag, &sc->sc_segs, 1);
		bus_dmamap_destroy(sc->sc_dmatag, sc->sc_dmamap);
		return 0;
	}
	
	aprint_normal_dev(sc->sc_dev, "DMA: SW -> kernel address is %p, dvma address is 0x%08llx, seg %llx / %ld\n", sc->sc_dma_kva, sc->sc_dmamap->dm_segs[0].ds_addr, sc->sc_segs.ds_addr, sc->sc_segs.ds_len);
	
	return 1;
}

paddr_t sbusfpga_curve25519engine_mmap(dev_t dev, off_t offset, int prot) {
	int unit = minor(dev) & (MAX_SESSION - 1);
	int driver = unit & ~(MAX_SESSION - 1);
	struct sbusfpga_curve25519engine_softc *sc = device_lookup_private(&sbusfpga_c29e_cd, driver);
	paddr_t addr = -1;

	device_printf(sc->sc_dev, "%s:%d: %lld %d for %d / %d\n", __PRETTY_FUNCTION__, __LINE__, offset, prot, driver, unit);
	
	if (offset != 0)
		return -1;
	if (prot & PROT_EXEC)
		return -1;
	/* if (sc->mapped_sessions & (1 << unit)) */
	/* 	return -1; */
	if ((sc->active_sessions & (1 << unit)) == 0)
		return -1;
	if (unit >= MAX_ACTIVE_SESSION)
		return -1;
	if (unit <= 0)
		return -1;
	
	//	addr = bus_dmamem_mmap(sc->sc_dmatag, sc->sc_dmamap->dm_segs, 1, (off_t)(4096*unit), prot, BUS_DMA_NOWAIT);
	if (pmap_extract(pmap_kernel(), ((vaddr_t)sc->sc_dma_kva) + (unit * 4096), &addr)) {
	
		device_printf(sc->sc_dev, "mapped page %d to 0x%08lx [0x%08lx], kernel is %p\n", unit, addr, atop(addr), (void*)(((vaddr_t)sc->sc_dma_kva) + (unit * 4096)));

		((uint32_t*)(((vaddr_t)sc->sc_dma_kva) + (unit * 4096)))[0] = 0xDEADBEEF;
		sc->mapped_sessions |= (1 << unit);
		
		return addr;
	}

	return -1;
}
