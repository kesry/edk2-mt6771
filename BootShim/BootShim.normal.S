/*
 * OPPO A3 (MT6771) BootShim - fake arm64 Linux Image header + EDK2 loader.
 *
 * The original LK on this board validates the arm64 Image header before it
 * jumps. Facts from mainline-linux-mt6771/script/a3-prep-kernel.py:
 *
 *   - code0 must be a branch instruction (0x14xxxxxx); a plain adr/nop makes
 *     LK hang at the logo until the watchdog resets the board.
 *   - text_offset must be 0x80000 (stock value).
 *   - image_size must be 0x257f000 (stock value); declaring the honest size
 *     hangs LK.
 *   - magic "ARM\x64" must be at offset 0x38.
 *
 * Stock header values were extracted from the original Android_boot.img:
 *   code0=0x14574000, text_offset=0x80000, image_size=0x257f000, flags=0xa.
 *
 * Layout:
 *   0x00  b _Start           (code0 - LK requires a branch)
 *   0x04  nop                (code1)
 *   0x08  text_offset        = 0x80000
 *   0x10  image_size         = 0x257f000
 *   0x18  flags              = 0xa
 *   0x38  "ARM\x64"
 *   0x40  _Start: real shim code (copy payload to UEFI_BASE and jump)
 *
 * This file is assembled into BootShim.bin and prepended to A3_UEFI.fd.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

_Head:
	/* code0: branch into the real shim (LK requires a branch here) */
	b	_Start
	/* code1: reserved */
	nop

	/* ---- fake Linux kernel header (LK reads these) ---- */
	.quad	0x00080000		/* text_offset */
	.quad	0x0257f000		/* image_size (stock value required by LK) */
	.quad	0x0000000a		/* flags (stock value) */
	.quad	0			/* reserved */
	.quad	0			/* reserved */
	.quad	0			/* reserved */

	/* ARM64 magic at 0x38 */
	.ascii	"ARM\x64"
	.long	0

	/* ---- real shim code starts at 0x40 ---- */
	.align	4
_Start:
	adr	x1, _Payload		/* runtime address of the EDK2 payload */
	adr	x4, _BootBaseValue
	ldr	x5, [x4]		/* x5 = UEFI_BASE */
	cmp	x1, x5
	beq	_Entry

	adr	x4, _BootSizeValue
	ldr	x6, [x4]		/* x6 = UEFI_SIZE */

_CopyLoop:
	ldp	x2, x3, [x1], #16
	stp	x2, x3, [x5], #16
	subs	x6, x6, #16
	b.ne	_CopyLoop

	adr	x4, _BootBaseValue
	ldr	x5, [x4]

_Entry:
	br	x5

_Dead:
	b	_Dead

	.align	3
_BootBaseValue:
	.quad	UEFI_BASE
_BootSizeValue:
	.quad	UEFI_SIZE

.text
.align	4
_Payload:
	/* A3_UEFI.fd is appended immediately after this binary. */
