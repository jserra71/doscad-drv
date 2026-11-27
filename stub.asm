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
;  2. Find the resident image of this build (below), or allocate a DOS block
;     for it, in upper memory when DOS has it, else at the top of conventional
;     memory; decompress the image there.
;  3. Shrink our own block to the stub (unless the record says 0xa55a).
;  4. Without a VESA BIOS, print what to set in dosbox.conf and wait for a key.
;  5. Fill the record with the entry table of the decompressed image.
;  6. Return the stub size in paragraphs.
;
; The image stays resident for the whole DOS session, shared by every load of
; the same build. Under ESP each program loads its driver into ESP's EMS frame
; and ESP keeps a pointer to the last loaded driver; when PCB386 or DRAFT load
; a driver of another file name, ESP calls that driver once more after the
; program has ended (VGA640.DRV still sits in the EMS frame then). An image in
; a block of the program was freed by then, and in conventional memory ESP's
; own code is back over it: ESP jumped into garbage and DOSBox-X hung. So the
; block belongs to DOS (MCB owner 8, name DOSCAD) and its last paragraph holds
; the build's identity (IMAGE_ID, 12 bytes) and, at offset 12, the segment of
; the stroke font block (doscad.c). A load that finds that block unpacks the
; image again over it (each load starts from a fresh image, as before) but
; keeps old16 and keyboard_hooked: the INT 16h hook may still be chained.
;
; Built by build.sh with -DFULL_PARAS=<image paragraphs>, -DTABLE=<entry
; table offset>, -DOLD16=<old16 offset>, -DKBHOOKED=<keyboard_hooked offset>,
; -DIMAGE_ID=<12 bytes> and -DPAYLOAD="<compressed image>".

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
        ; 2. the resident image of this build, or a new block for it. Link
        ; UMBs for the search and the allocation (saved and restored).
        mov     ax, 0x5800
        int     0x21
        mov     [cs:old_strategy], ax
        mov     ax, 0x5802
        int     0x21
        mov     [cs:old_link], al
        mov     ax, 0x5803
        mov     bx, 1
        int     0x21
        call    find_image
        jnc     .reuse
        ; upper memory first, else the last fit in conventional memory: at
        ; its top the block leaves the free memory below in one piece
        mov     ax, 0x5801
        mov     bx, 0x82
        int     0x21
        mov     bx, FULL_PARAS
        mov     ah, 0x48
        int     0x21
        pushf
        push    ax
        call    restore_strategy
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
        call    mark_block
        mov     es, ax
        push    cs
        pop     ds
        mov     si, payload
        mov     bx, payload_end
        xor     di, di
        cld
        call    decompress
        jmp     .unpacked
.reuse:
        push    ax
        call    restore_strategy
        pop     ax
        ; keep the INT 16h hook's state across the new copy
        mov     es, ax
        mov     ebx, [es:OLD16]
        mov     [cs:saved_old16], ebx
        mov     ebx, [es:KBHOOKED]
        mov     [cs:saved_hooked], ebx
        push    cs
        pop     ds
        mov     si, payload
        mov     bx, payload_end
        xor     di, di
        cld
        call    decompress
        mov     es, [cs:image_segment]
        mov     ebx, [cs:saved_old16]
        mov     [es:OLD16], ebx
        mov     ebx, [cs:saved_hooked]
        mov     [es:KBHOOKED], ebx
.unpacked:
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

; Restore the allocation strategy and the UMB link saved on entry.
restore_strategy:
        mov     ax, 0x5801
        mov     bx, [cs:old_strategy]
        int     0x21
        mov     ax, 0x5803
        xor     bh, bh
        mov     bl, [cs:old_link]
        int     0x21
        ret

; Walk the MCB chain (UMBs linked) for a block owned by DOS, of FULL_PARAS
; paragraphs, named DOSCAD, whose last paragraph starts with IMAGE_ID.
; Returns CF clear and AX = its segment, or CF set.
find_image:
        push    ds
        push    cs
        pop     ds
        mov     ah, 0x52
        int     0x21
        mov     ax, [es:bx-2]
.walk:
        mov     es, ax
        mov     dl, [es:0]
        cmp     dl, 'M'
        je      .check
        cmp     dl, 'Z'
        jne     .none
.check:
        cmp     word [es:1], 8
        jne     .next
        cmp     word [es:3], FULL_PARAS
        jne     .next
        mov     si, mcb_name
        mov     di, 8
        mov     cx, 8
        cld
        repe    cmpsb
        jne     .next
        push    es
        mov     bx, es
        add     bx, FULL_PARAS
        mov     es, bx
        mov     si, image_id
        xor     di, di
        mov     cx, 12
        repe    cmpsb
        pop     es
        jne     .next
        mov     ax, es
        inc     ax
        pop     ds
        clc
        ret
.next:
        cmp     dl, 'Z'
        je      .none
        mov     ax, es
        add     ax, [es:3]
        inc     ax
        jmp     .walk
.none:
        pop     ds
        stc
        ret

; Give the block at AX to DOS (owner 8, name DOSCAD) so that it outlives the
; program that loaded it, and write the build's identity in its last
; paragraph (the stroke font segment after it: none yet).
mark_block:
        push    ds
        push    cs
        pop     ds
        mov     bx, ax
        dec     bx
        mov     es, bx
        mov     word [es:1], 8
        mov     si, mcb_name
        mov     di, 8
        mov     cx, 8
        cld
        rep     movsb
        add     bx, FULL_PARAS
        mov     es, bx
        mov     si, image_id
        xor     di, di
        mov     cx, 12
        rep     movsb
        xor     eax, eax
        stosd
        mov     ax, es
        sub     ax, FULL_PARAS - 1
        pop     ds
        ret

; Decompress DS:SI..DS:BX into ES:DI (lzpack.py). Token byte t:
;  t < 0x40: copy t+1 literal bytes;
;  0x40 <= t < 0x80: copy ((t >> 3) & 7) + 2 bytes from ((t & 7) * 256 + the
;    next byte + 1) back in the output;
;  t >= 0x80: copy (t & 0x7f) + 3 bytes from a word distance back.
; A copy may overlap itself.
decompress:
        mov     [cs:image_segment], es
.next:
        cmp     si, bx
        jae     .done
        xor     cx, cx
        lodsb
        mov     cl, al
        cmp     al, 0x40
        jb      .literals
        test    al, 0x80
        jnz     .far
        and     al, 7
        mov     ah, al
        lodsb
        inc     ax
        shr     cl, 3
        and     cl, 7
        add     cl, 2
        jmp     .copy
.far:
        and     cl, 0x7f
        add     cx, 3
        lodsw
.copy:
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
.literals:
        inc     cx
        rep     movsb
        jmp     .next
.done:
        ret

image_segment:
        dw      0
old_strategy:
        dw      0
old_link:
        db      0
saved_old16:
        dd      0
saved_hooked:
        dd      0
mcb_name:
        db      "DOSCAD", 0, 0
image_id:
        db      IMAGE_ID

payload:
        incbin  PAYLOAD
payload_end:
        align   16
stub_end:

STUB_PARAS equ (stub_end - start) / 16 + 2
