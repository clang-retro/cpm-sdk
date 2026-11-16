/*
 * A hard disk boot sector that boots the floppy in drive A: without
 * checking it for the 55AAh signature, which SeaBIOS (QEMU's BIOS) wants
 * and CP/M-86 boot sectors don't have. It moves itself to 0000:0600h,
 * reads the floppy's first sector to 0000:7C00h and jumps there.
 *
 *   clang --target=i386-unknown-elf -m16 -c chainboot.S
 *   ld.lld -Ttext=0x600 --oformat=binary chainboot.o -o chainboot.bin
 *
 * SPDX-License-Identifier: CC0-1.0
 */
	.code16
	.text
	.globl	_start
_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$0x7c00, %sp
	sti
	cld
	movw	$0x7c00, %si
	movw	$0x600, %di
	movw	$256, %cx
	rep movsw
	ljmp	$0, $moved
moved:
	movw	$4, %bp			/* tries */
1:	xorw	%ax, %ax		/* reset the floppy controller */
	xorw	%dx, %dx
	int	$0x13
	movw	$0x0201, %ax		/* read 1 sector */
	movw	$0x0001, %cx		/* track 0, sector 1 */
	xorw	%dx, %dx		/* head 0, drive A: */
	movw	$0x7c00, %bx
	int	$0x13
	jnc	2f
	decw	%bp
	jnz	1b
	hlt
2:	xorw	%dx, %dx
	ljmp	$0, $0x7c00

	.org	510
	.byte	0x55, 0xaa
