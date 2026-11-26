/*
 * DIAGNOSTIC BootShim for OPPO A3 (MT6771). DO NOT USE AS RELEASE.
 *
 * Same arm64 Image header as the release shim, but the code at 0x40 writes
 * the TopRGU watchdog reset values proven in
 * mainline-linux-mt6771/script/a3-prep-kernel.py:
 *
 *   WDT_MODE  @ 0x10007000 = 0x22000030
 *   WDT_SWRST @ 0x10007014 = 0x1209
 *
 * If the device reboots within a few seconds, LK DID jump to our payload.
 * If it still hangs at logo, LK DID NOT jump to our payload.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

_Head:
	b	_Start
	nop

	.quad	0x00080000
	.quad	0x0257f000
	.quad	0x0000000a
	.quad	0
	.quad	0
	.quad	0

	.ascii	"ARM\x64"
	.long	0

	.align	4
_Start:
	.inst	0xd28e0000
	.inst	0xf2a20000
	.inst	0x52800602
	.inst	0x72a44002
	.inst	0xb9000002
	.inst	0x52824121
	.inst	0xb9001401
	.inst	0x14000000

.text
.align	4
