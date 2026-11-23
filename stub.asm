; SPDX-FileCopyrightText: 2026 Joaquim Serra
; SPDX-License-Identifier: GPL-2.0-or-later
;
; Developed by Claude Opus 5.5 in high effort mode.
;
; DOSCAD.DRV loader stub: the part of the file DOS loads.
;
; PCB_RM.EXE allocates a fixed 32 KB block (int 21h 48h, BX = 0x800) and loads
; the driver into it with int 21h 4B03h, which does not check the block size:
; a bigger load module overwrites the next memory control block and DOS hangs.
; So the file holds this small stub plus the whole driver image compressed.
;
; Flow (far Pascal call, one far pointer to the driver record, like VGA640.DRV):
;  1. Mark the record as VGA640.DRV does (0xa55a / 0xa55b markers).
;  2. Allocate a DOS block for the full image, in upper memory when DOS has
;     it, and decompress it there.
;  3. Shrink our own block to the stub (unless the record says 0xa55a).
;  4. Without a VESA BIOS, print what to set in dosbox.conf and wait for a key.
;  5. Fill the record with the entry table of the decompressed image.
;  6. Return the stub size in paragraphs.
;
; Built by build.sh with -DFULL_PARAS=<image paragraphs>, -DTABLE=<entry
; table offset> and -DPAYLOAD="<compressed image>".

        bits 16
        org 0

start:
        jmp     near load_routine
        db      0x3e, 0, 0, 0, 0, 0xb6, 0x06, 0xe8, 0x15, 0xe0, 0x04
banner:
        db      "DOSCAD display driver for OrCAD 386+", 13, 10, "$"
no_memory:
        db      "DOSCAD.DRV: not enough memory", 13, 10, "$"
no_vesa:
        db      "DOSCAD.DRV: no VESA BIOS. In dosbox.conf set machine=svga_dosbox", 13, 10
        db      "(see README.md of the driver). Press a key.", 13, 10, "$"

load_routine:
        push    bp
        mov     bp, sp
        push    ds
        push    si
        push    di
        ; 1. the record markers, as in VGA640.DRV
        les     bx, [bp+6]
        cmp     word [es:bx+6], 0xa55a
        je      .marked
        cmp     word [es:bx+6], 0xa55b
        jne     .unmarked
.marked:
        mov     word [es:bx+8], 0x10
.unmarked:
        ; 2. a block for the full image (code, data, stack)
        ; in upper memory when DOS has it (tested: in conventional memory the
        ; board picture comes out wrong; the cause is not known yet). Save the
        ; allocation strategy and UMB link, link UMBs, ask high-only (0x80).
        mov     ax, 0x5800
        int     0x21
        mov     [cs:old_strategy], ax
        mov     ax, 0x5802
        int     0x21
        mov     [cs:old_link], al
        mov     ax, 0x5803
        mov     bx, 1
        int     0x21
        mov     ax, 0x5801
        mov     bx, 0x80
        int     0x21
        mov     bx, FULL_PARAS
        mov     ah, 0x48
        int     0x21
        pushf
        push    ax
        mov     ax, 0x5801
        mov     bx, [cs:old_strategy]
        int     0x21
        mov     ax, 0x5803
        xor     bh, bh
        mov     bl, [cs:old_link]
        int     0x21
        pop     ax
        popf
        jnc     .allocated
        push    cs
        pop     ds
        mov     dx, no_memory
        mov     ah, 9
        int     0x21
        xor     ax, ax
        jmp     .fill
.allocated:
        mov     es, ax
        push    cs
        pop     ds
        mov     si, payload
        mov     bx, payload_end
        xor     di, di
        cld
        call    decompress
        ; 3. shrink our own block to the stub
        les     di, [bp+6]
        cmp     word [es:di+6], 0xa55a
        je      .no_resize
        push    cs
        pop     es
        mov     bx, STUB_PARAS
        mov     ah, 0x4a
        int     0x21
        mov     dx, banner
        mov     ah, 9
        int     0x21
.no_resize:
        ; 4a. no VESA BIOS (DOSBox-X machine=svga_et4000 has none): the open
        ; would fail and PCB386 stops with a page fault, so say why first and
        ; wait for a key. The 4F00 reply goes over the payload, already unpacked.
        push    cs
        pop     es
        mov     di, payload
        mov     ax, 0x4f00
        int     0x10
        cmp     ax, 0x004f
        je      .vesa_found
        mov     dx, no_vesa
        mov     ah, 9
        int     0x21
        xor     ah, ah
        int     0x16
.vesa_found:
        ; the segment decompress() saved
        mov     ax, [cs:image_segment]
.fill:
        ; 5. entry table of the decompressed image
        les     bx, [bp+6]
        mov     word [es:bx], 0xd
        mov     word [es:bx+4], ax
        mov     word [es:bx+2], TABLE
        ; 6. our size in paragraphs, as VGA640.DRV returns its own
        mov     ax, STUB_PARAS
        pop     di
        pop     si
        pop     ds
        pop     bp
        retf    4

; Decompress DS:SI..DS:BX into ES:DI. Token byte t: t < 0x80 copies t+1
; literal bytes; t >= 0x80 copies (t & 0x7f) + 3 bytes from a word distance
; back in the output (the copy may overlap itself).
decompress:
        mov     [cs:image_segment], es
.next:
        cmp     si, bx
        jae     .done
        xor     cx, cx
        lodsb
        mov     cl, al
        test    cl, 0x80
        jnz     .match
        inc     cx
        rep     movsb
        jmp     .next
.match:
        and     cl, 0x7f
        add     cx, 3
        lodsw
        push    si
        push    ds
        mov     si, di
        sub     si, ax
        push    es
        pop     ds
        rep     movsb
        pop     ds
        pop     si
        jmp     .next
.done:
        ret

image_segment:
        dw      0
old_strategy:
        dw      0
old_link:
        db      0

payload:
        incbin  PAYLOAD
payload_end:
        align   16
stub_end:

STUB_PARAS equ (stub_end - start) / 16 + 2
