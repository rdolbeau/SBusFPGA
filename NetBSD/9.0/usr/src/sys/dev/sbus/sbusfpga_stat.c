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

#include <sys/rndsource.h>

#include <dev/sbus/sbusvar.h>

#include <dev/sbus/sbusfpga_stat.h>

#include <machine/param.h>

int	sbusfpga_stat_print(void *, const char *);
int	sbusfpga_stat_match(device_t, cfdata_t, void *);
void	sbusfpga_stat_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(sbusfpga_stat, sizeof(struct sbusfpga_sbus_bus_stat_softc),
    sbusfpga_stat_match, sbusfpga_stat_attach, NULL, NULL);

dev_type_open(sbusfpga_stat_open);
dev_type_close(sbusfpga_stat_close);
dev_type_ioctl(sbusfpga_stat_ioctl);


const struct cdevsw sbusfpga_stat_cdevsw = {
	.d_open = sbusfpga_stat_open,
	.d_close = sbusfpga_stat_close,
	.d_read = noread,
	.d_write = nowrite,
	.d_ioctl = sbusfpga_stat_ioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = 0
};

extern struct cfdriver sbusfpga_stat_cd;
int
sbusfpga_stat_open(dev_t dev, int flags, int mode, struct lwp *l)
{
	return (0);
}

int
sbusfpga_stat_close(dev_t dev, int flags, int mode, struct lwp *l)
{
	return (0);
}

int
sbusfpga_stat_print(void *aux, const char *busname)
{

	sbus_print(aux, busname);
	return (UNCONF);
}

int
sbusfpga_stat_match(device_t parent, cfdata_t cf, void *aux)
{
	struct sbus_attach_args *sa = (struct sbus_attach_args *)aux;

	return (strcmp("RDOL,sbusstat", sa->sa_name) == 0);
}

#define CONFIG_CSR_DATA_WIDTH 32
#include "dev/sbus/sbusfpga_csr_sbus_bus_stat.h"

static void sbusfpga_stat_display(void *);

/*
 * Attach all the sub-devices we can find
 */
void
sbusfpga_stat_attach(device_t parent, device_t self, void *aux)
{
	struct sbus_attach_args *sa = aux;
	struct sbusfpga_sbus_bus_stat_softc *sc = device_private(self);
	struct sbus_softc *sbsc = device_private(parent);
	int node;
	int sbusburst;
		
	sc->sc_bustag = sa->sa_bustag;
	sc->sc_dev = self;

	if (sbus_bus_map(sc->sc_bustag, sa->sa_slot, sa->sa_offset, sa->sa_size,
			 BUS_SPACE_MAP_LINEAR, &sc->sc_bhregs_sbus_bus_stat) != 0) {
		aprint_error(": cannot map registers\n");
		return;
	}

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

	sc->sc_delay = 5 * hz; // five seconds

	callout_init(&sc->sc_display, CALLOUT_MPSAFE);
	callout_setfunc(&sc->sc_display, sbusfpga_stat_display, sc);
	/* disable by default */
	sc->sc_enable = 0;
	/* do it once during boot*/
	callout_schedule(&sc->sc_display, sc->sc_delay);
}

#define SBUSFPGA_STAT_ON    _IO(0, 1)
#define SBUSFPGA_STAT_OFF   _IO(0, 0)

int
sbusfpga_stat_ioctl (dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct sbusfpga_sbus_bus_stat_softc *sc = device_lookup_private(&sbusfpga_stat_cd, minor(dev));
	int err = 0;

	if (sc == NULL) {
		err = EINVAL;
		goto done;
	}

	switch (cmd) {
	case SBUSFPGA_STAT_ON:
		if (!sc->sc_enable) {
			sc->sc_enable = 1;
			callout_schedule(&sc->sc_display, sc->sc_delay);
		}
		break;
	case SBUSFPGA_STAT_OFF:
		if (sc->sc_enable) {
			callout_stop(&sc->sc_display);
			sc->sc_enable = 0;
		}
		break;
	default:
		err = ENOTTY;
		break;
	}

 done:
	return err;
}

static void sbusfpga_stat_display(void *args) {
	struct sbusfpga_sbus_bus_stat_softc *sc = args;
	unsigned int c = sbus_bus_stat_stat_cycle_counter_read(sc), c2;
	int count;
	sbus_bus_stat_stat_ctrl_write(sc, 1);
	delay(1);
	count = 0;
	while (count < 10 && ((c2 = sbus_bus_stat_stat_cycle_counter_read(sc)) == c)) {
		count ++;
		delay(1);
	}
	if ((c2 == c) || (c2 == 0)){
		device_printf(sc->sc_dev, "Statistics didn't update\n");
	} else {
		device_printf(sc->sc_dev, "%u: slave %u %u %u %u\n", // [0x%08x @ 0x%08x] 
					  c2,
					  sbus_bus_stat_stat_slave_start_counter_read(sc),
					  sbus_bus_stat_stat_slave_done_counter_read(sc),
					  sbus_bus_stat_stat_slave_rerun_counter_read(sc),
					  //sbus_bus_stat_stat_slave_rerun_last_pa_read(sc),
					  //sbus_bus_stat_stat_slave_rerun_last_state_read(sc),
					  sbus_bus_stat_stat_slave_early_error_counter_read(sc));
		device_printf(sc->sc_dev, "%u: master %u %u %u %u (0x%08x)\n",
					  c2,
					  sbus_bus_stat_stat_master_start_counter_read(sc),
					  sbus_bus_stat_stat_master_done_counter_read(sc),
					  sbus_bus_stat_stat_master_error_counter_read(sc),
					  sbus_bus_stat_stat_master_rerun_counter_read(sc),
					  sbus_bus_stat_sbus_master_error_virtual_read(sc));
	}
	sbus_bus_stat_stat_ctrl_write(sc, 0);
	if (sc->sc_enable)
		callout_schedule(&sc->sc_display, sc->sc_delay);
}
