/*-
 * Copyright (c) 2012-2016 Solarflare Communications Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 *
 * $FreeBSD: head/sys/dev/sfxge/common/hunt_impl.h 300607 2016-05-24 12:16:57Z arybchik $
 */

#ifndef _SYS_HUNT_IMPL_H
#define	_SYS_HUNT_IMPL_H

#include "efx.h"
#include "efx_regs.h"
#include "efx_regs_ef10.h"
#include "efx_mcdi.h"

#ifdef	__cplusplus
extern "C" {
#endif

/* Missing register definitions */
#ifndef	ER_DZ_TX_PIOBUF_OFST
#define	ER_DZ_TX_PIOBUF_OFST 0x00001000
#endif
#ifndef	ER_DZ_TX_PIOBUF_STEP
#define	ER_DZ_TX_PIOBUF_STEP 8192
#endif
#ifndef	ER_DZ_TX_PIOBUF_ROWS
#define	ER_DZ_TX_PIOBUF_ROWS 2048
#endif

#ifndef	ER_DZ_TX_PIOBUF_SIZE
#define	ER_DZ_TX_PIOBUF_SIZE 2048
#endif

#define	HUNT_PIOBUF_NBUFS	(16)
#define	HUNT_PIOBUF_SIZE	(ER_DZ_TX_PIOBUF_SIZE)

#define	HUNT_MIN_PIO_ALLOC_SIZE	(HUNT_PIOBUF_SIZE / 32)


/* NIC */

extern	__checkReturn	efx_rc_t
hunt_board_cfg(
	__in		efx_nic_t *enp);


/* PHY */

#if EFSYS_OPT_BIST

extern	__checkReturn		efx_rc_t
hunt_bist_enable_offline(
	__in			efx_nic_t *enp);

extern	__checkReturn		efx_rc_t
hunt_bist_start(
	__in			efx_nic_t *enp,
	__in			efx_bist_type_t type);

extern	__checkReturn		efx_rc_t
hunt_bist_poll(
	__in			efx_nic_t *enp,
	__in			efx_bist_type_t type,
	__out			efx_bist_result_t *resultp,
	__out_opt __drv_when(count > 0, __notnull)
	uint32_t 	*value_maskp,
	__out_ecount_opt(count)	__drv_when(count > 0, __notnull)
	unsigned long	*valuesp,
	__in			size_t count);

extern				void
hunt_bist_stop(
	__in			efx_nic_t *enp,
	__in			efx_bist_type_t type);

#endif	/* EFSYS_OPT_BIST */


#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_HUNT_IMPL_H */
