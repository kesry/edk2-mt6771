/*
 * DIAGNOSTIC STAGE 3 BootShim for OPPO A3 (MT6771). DO NOT USE AS RELEASE.
 *
 * Stage 1 proved: LK jumps to our payload.
 * Stage 2 proved: BootShim can copy A3_UEFI.fd to UEFI_BASE (0x50000000).
 *
 * Stage 3 verifies the final `br UEFI_BASE` itself:
 *   1. copy payload to UEFI_BASE
 *   2. overwrite the first 8 instructions at UEFI_BASE with a WDT reset stub
 *   3. branch to UEFI_BASE
 *
 * Interpretation:
 *   - device reboots: br UEFI_BASE works, entry execution works. Failure is
 *     deeper in EDK2 after the reset vector.
 *   - device hangs: br UEFI_BASE or entry execution is broken.
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

	/* Reload UEFI_BASE into x5. */
	adr	x4, _BootBaseValue
	ldr	x5, [x4]

	/* Patch first 8 words at 0x50000000 with WDT reset stub. */
	adr	x7, _WdtStub
	ldr	x8, _WdtStub
	/* x7 = address of stub data; copy 8 words to x5 */
	ldp	x2, x3, [x7], #16
	stp	x2, x3, [x5], #16
	ldp	x2, x3, [x7], #16
	stp	x2, x3, [x5], #16
	ldp	x2, x3, [x7], #16
	stp	x2, x3, [x5], #16
	ldp	x2, x3, [x7], #16
	stp	x2, x3, [x5], #16

	/* x5 advanced to 0x50000020; reload base and jump. */
	adr	x4, _BootBaseValue
	ldr	x5, [x4]
	br	x5

_Dead:
	b	_Dead

	.align	3
_BootBaseValue:
	.quad	UEFI_BASE
_BootSizeValue:
	.quad	UEFI_SIZE

	.align	3
_WdtStub:
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
_Payload:
