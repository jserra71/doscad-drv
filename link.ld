/* SPDX-FileCopyrightText: 2026 Joaquim Serra
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Developed by Claude Opus 5.5 in high effort mode.
 *
 * Flat 16-bit image for DOSCAD.DRV: the nasm stub's code at offset 0, then
 * the C code and data; uninitialised data is kept in the file (zeros) so the
 * whole image is loaded and image_end gives its size.
 */
OUTPUT_ARCH(i386)
SECTIONS
{
    . = 0;
    .image : {
        doscad-asm.o(.text)
        *(.text*)
        *(.rodata*)
        /* doscad.asm's .bss starts with the stack: see the note there */
        . = ALIGN(16);
        doscad-asm.o(.bss)
        *(.data*)
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        image_end = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
