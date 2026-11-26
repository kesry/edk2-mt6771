/*
 * DIAGNOSTIC STAGE 2 BootShim for OPPO A3 (MT6771). DO NOT USE AS RELEASE.
 *
 * Purpose: verify that the normal BootShim relocation path works.
 *
 * The release shim copies A3_UEFI.fd to UEFI_BASE (0x50000000) and jumps
 * there. This diagnostic performs the SAME copy, then immediately triggers
 * TopRGU reset instead of jumping into EDK2:
 *
 *   WDT_MODE  @ 0x10007000 = 0x22000030
 *   WDT_SWRST @ 0x10007014 = 0x1209
 *
 * Interpretation:
 *   - device reboots within a few seconds:
 *       BootShim relocation/copy loop works. Failure is inside EDK2.
 *   - device hangs at logo:
 *       BootShim relocation path or payload location/size is broken.
 *
 * Header still satisfies OPPO A3 LK.
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
	adr	x1, _Payload
	adr	x4, _BootBaseValue
	ldr	x5, [x4]
	adr	x4, _BootSizeValue
	ldr	x6, [x4]

_CopyLoop:
	ldp	x2, x3, [x1], #16
	stp	x2, x3, [x5], #16
	subs	x6, x6, #16
	b.ne	_CopyLoop

	/* Copy completed: trigger WDT reset. */
	.inst	0xd28e0000
	.inst	0xf2a20000
	.inst	0x52800602
	.inst	0x72a44002
	.inst	0xb9000002
	.inst	0x52824121
	.inst	0xb9001401
	.inst	0x14000000

	.align	3
_BootBaseValue:
	.quad	UEFI_BASE
_BootSizeValue:
	.quad	UEFI_SIZE

.text
.align	4
_Payload:
