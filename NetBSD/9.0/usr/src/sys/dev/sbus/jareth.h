/*	$NetBSD$ */

/*-
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

#ifndef _JARETH_H_
#define _JARETH_H_

#define MAX_SESSION 32 // HW limit
#define MAX_ACTIVE_SESSION 8 // SW-imposed limit
// Single 4KiB pages per session
#define JARETH_VAL_DMA_MAX_SZ (MAX_ACTIVE_SESSION*4*1024)

struct jareth_softc {
	device_t sc_dev;		/* us as a device */
	u_int	sc_rev;			/* revision */
	int	sc_node;		/* PROM node ID */
	int	sc_burst;		/* DVMA burst size in effect */
	bus_space_tag_t	sc_bustag;	/* bus tag */
	bus_space_handle_t sc_bhregs_jareth;	/* bus handle */
	bus_space_handle_t sc_bhregs_microcode;	/* bus handle */
	bus_space_handle_t sc_bhregs_regfile;	/* bus handle */
	//void *	sc_buffer;		/* VA of the registers */
	int	sc_bufsiz_jareth;		/* Size of buffer */
	int	sc_bufsiz_microcode;		/* Size of buffer */
	int	sc_bufsiz_regfile;		/* Size of buffer */
	int initialized;
	uint32_t active_sessions;
	uint32_t mapped_sessions;
	uint32_t sessions_cookies[MAX_ACTIVE_SESSION];
	/* DMA kernel structures */
	bus_dma_tag_t		sc_dmatag;
	bus_dmamap_t		sc_dmamap;
	bus_dma_segment_t       sc_segs;
	int                     sc_rsegs;
	void *              sc_dma_kva;
};

#endif /* _JARETH_H_ */
