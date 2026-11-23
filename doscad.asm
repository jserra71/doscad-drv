; SPDX-FileCopyrightText: 2026 Joaquim Serra
; SPDX-License-Identifier: GPL-2.0-or-later
;
; Developed by Claude Opus 5.5 in high effort mode.
;
; DOSCAD.DRV real-mode stub: load routine, entry table, per-entry thunks
; that switch to the driver's own stack and call the C dispatcher, and the
; small helpers C cannot express (software interrupts).
;
; Flow:
;  1. PCB386 far-calls offset 0 (jmp to load_routine) with a far pointer to
;     its driver record; we fill the record (entry table offset) and return
;     the driver size in paragraphs, like VGA640.DRV.
;  2. PCB386 far-calls an entry through the table; the thunk saves every
;     register, copies the stack arguments to `args`, switches to our stack
;     and calls dispatch() in C.
;  3. dispatch() sets ret_ax/ret_dx; the thunk restores the caller's stack,
;     writes the return registers and does `retf <argument bytes>`.

        bits 16
        section .text

        extern dispatch
        extern image_end
        global int86
        global entry_table
        global args
        global ret_ax
        global ret_bx
        global ret_cx
        global ret_dx
        global g_entry

start:
        jmp     near load_routine
        db      0x3e, 0, 0, 0, 0, 0xb6, 0x06, 0xe8, 0x15, 0xe0, 0x04
banner:
        db      "DOSCAD display driver for PCB386", 13, 10, "$"

; Unused entries (52..56) point here, as in VGA640.DRV.
plain_retf:
        retf

; Entry table: 57 near offsets in this segment.
entry_table:
        dw      e0, e1, e2, e3, e4, e5, e6, e7, e8, e9
        dw      e10, e11, e12, e13, e14, e15, e16, e17, e18, e19
        dw      e20, e21, e22, e23, e24, e25, e26, e27, e28, e29
        dw      e30, e31, e32, e33, e34, e35, e36, e37, e38, e39
        dw      e40, e41, e42, e43, e44, e45, e46, e47, e48, e49
        dw      e50, e51
        dw      plain_retf, plain_retf, plain_retf, plain_retf, plain_retf

; Load routine (far, Pascal, one far pointer argument): mirrors VGA640.DRV.
load_routine:
        push    bp
        mov     bp, sp
        push    ds
        les     bx, [bp+6]
        cmp     word [es:bx+6], 0xa55a
        je      .marked
        cmp     word [es:bx+6], 0xa55b
        jne     .unmarked
.marked:
        mov     word [es:bx+8], 0x10
.unmarked:
        pop     ds
        ; size of the whole image (code, data and stack) in paragraphs
        mov     bx, image_end
        add     bx, 15
        shr     bx, 4
        add     bx, 2
        push    bx
        les     di, [bp+6]
        cmp     word [es:di+6], 0xa55a
        je      .no_resize
        push    cs
        pop     es
        mov     ah, 0x4a
        int     0x21
        push    ds
        push    cs
        pop     ds
        mov     dx, banner
        mov     ah, 9
        int     0x21
        pop     ds
.no_resize:
        les     bx, [bp+6]
        mov     word [es:bx], 0xd
        ; segment of the table: VGA640.DRV has a relocated immediate here
        mov     word [es:bx+4], cs
        mov     word [es:bx+2], entry_table
        pop     ax
        pop     bp
        retf    4

; One thunk per entry: N = entry number, B = argument bytes popped by retf.
%macro ENTRY 2
e%1:
        push    bp
        mov     bp, sp
        push    ds
        push    es
        pushad
        push    fs
        push    gs
        push    cs
        pop     ds
        cld
        mov     ax, %1
        mov     cx, %2
        call    entry_common
        pop     gs
        pop     fs
        popad
        pop     es
        pop     ds
        pop     bp
%if %2 == 0
        retf
%else
        retf    %2
%endif
%endmacro

        ENTRY 0, 0xa
        ENTRY 1, 0
        ENTRY 2, 0xa
        ENTRY 3, 0
        ENTRY 4, 2
        ENTRY 5, 0
        ENTRY 6, 8
        ENTRY 7, 0
        ENTRY 8, 2
        ENTRY 9, 0
        ENTRY 10, 2
        ENTRY 11, 8
        ENTRY 12, 4
        ENTRY 13, 6
        ENTRY 14, 2
        ENTRY 15, 0
        ENTRY 16, 0
        ENTRY 17, 0x10
        ENTRY 18, 0x22
        ENTRY 19, 8
        ENTRY 20, 4
        ENTRY 21, 0xa
        ENTRY 22, 0xa
        ENTRY 23, 8
        ENTRY 24, 4
        ENTRY 25, 2
        ENTRY 26, 6
        ENTRY 27, 0x14
        ENTRY 28, 8
        ENTRY 29, 4
        ENTRY 30, 4
        ENTRY 31, 4
        ENTRY 32, 0x12
        ENTRY 33, 0xa
        ENTRY 34, 0xa
        ENTRY 35, 0xc
        ENTRY 36, 0xc
        ENTRY 37, 8
        ENTRY 38, 8
        ENTRY 39, 0xa
        ENTRY 40, 0xa
        ENTRY 41, 0x16
        ENTRY 42, 0xe
        ENTRY 43, 8
        ENTRY 44, 8
        ENTRY 45, 8
        ENTRY 46, 0
        ENTRY 47, 0
        ENTRY 48, 0
        ENTRY 49, 8
        ENTRY 50, 0
        ENTRY 51, 8

; Shared body: AX = entry number, CX = argument bytes, BP = thunk frame.
; Return-register slots of the pushad frame: AX [bp-8], CX [bp-12],
; DX [bp-16], BX [bp-20].
entry_common:
        mov     [g_entry], ax
        ; copy the caller's stack arguments (last pushed first) into args
        push    ds
        pop     es
        mov     di, args
        lea     si, [bp+6]
        shr     cx, 1
        jcxz    .copied
.copy:
        mov     ax, [ss:si]
        stosw
        add     si, 2
        loop    .copy
.copied:
        mov     ax, [bp-8]
        mov     [ret_ax], ax
        mov     ax, [bp-12]
        mov     [ret_cx], ax
        mov     ax, [bp-16]
        mov     [ret_dx], ax
        mov     ax, [bp-20]
        mov     [ret_bx], ax
        ; switch to our own stack: the caller's may be small
        mov     [saved_ss], ss
        mov     [saved_sp], sp
        ; dispatch() returns with ebp = 0 (cleared below, callee-saved by
        ; gcc), so keep the frame pointer for the return-register writes
        mov     [saved_bp], bp
        mov     ax, cs
        cli
        mov     ss, ax
        mov     esp, stack_top
        ; Interrupts stay off while the C code runs: 16-bit interrupt
        ; handlers (mouse, timer, extender) may not keep the high halves of
        ; the 32-bit registers gcc -m16 code relies on.
        mov     ax, 0xa000
        mov     gs, ax
        xor     ebp, ebp
        ; 32-bit near call: gcc -m16 code returns with a 32-bit ret.
        ; (`o32 call` would get a 16-bit displacement from nasm.)
        call    dword dispatch
        mov     ss, [cs:saved_ss]
        mov     sp, [cs:saved_sp]
        mov     bp, [cs:saved_bp]
        sti
        push    cs
        pop     ds
        mov     ax, [ret_ax]
        mov     [bp-8], ax
        mov     ax, [ret_cx]
        mov     [bp-12], ax
        mov     ax, [ret_dx]
        mov     [bp-16], ax
        mov     ax, [ret_bx]
        mov     [bp-20], ax
        ret

; void int86(struct regs *r): run int 10h or int 21h (r->intno) with the
; registers of r and store the results back (C calling convention, -m16).
; Layout: eax 0, ebx 4, ecx 8, edx 12, esi 16, edi 20, es 24 (word),
; intno 26 (byte), flags 28 (word).
int86:
        push    ebp
        mov     ebp, esp
        pushad
        push    es
        mov     esi, [ebp+8]
        mov     es, [esi+24]
        mov     eax, [esi]
        mov     ebx, [esi+4]
        mov     ecx, [esi+8]
        mov     edx, [esi+12]
        mov     edi, [esi+20]
        cmp     byte [esi+26], 0x21
        mov     esi, [esi+16]
        je      .dos
        int     0x10
        jmp     .back
.dos:
        int     0x21
.back:
        push    esi
        pushf
        mov     esi, [ebp+8]
        pop     word [esi+28]
        pop     dword [esi+16]
        mov     [esi], eax
        mov     [esi+4], ebx
        mov     [esi+8], ecx
        mov     [esi+12], edx
        mov     [esi+20], edi
        mov     [esi+24], es
        pop     es
        popad
        pop     ebp
        db      0x66            ; 32-bit near ret (gcc -m16 calling convention)
        ret

        section .bss
        alignb  4
args:           resw 32
ret_ax:         resw 1
saved_bp:       resw 1
ret_bx:         resw 1
ret_cx:         resw 1
ret_dx:         resw 1
g_entry:        resw 1
saved_ss:       resw 1
saved_sp:       resw 1
        alignb  16
; 8 KB: the deepest chain (wide line, arc end, circle) needs over 1 KB, and an
; overflow overwrites saved_ss/saved_sp just below, so the return to PCB386 fails.
stack_bottom:   resb 8192
stack_top:
