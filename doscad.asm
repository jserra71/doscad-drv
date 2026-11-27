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
;  2. PCB386 far-calls an entry through the table; the entry's stub jumps to
;     entry_common, which switches to our stack first, saves every register
;     there, copies the stack arguments to `args` and calls dispatch() in C.
;  3. dispatch() sets ret_ax/ret_dx (the saved registers themselves); the
;     registers come back, and the stub restores the caller's stack and does
;     `retf <argument bytes>`. Nothing is written on the caller's stack.
;  4. While the driver is open, int16_hook sits on INT 16h (installed and
;     removed by doscad.c, entries 0 and 1) and halts the CPU in a program's
;     key-poll loop, so the emulator idles while the user does nothing.

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

; One stub per entry: N = entry number, B = argument bytes popped by retf, the same
; as the original drivers' retf of that entry (all 52 checked against VGA640.DRV;
; entry 4 takes none: up to 1.7.1 it popped 2, which unbalanced the caller's stack).
; The stubs are all STUB_SIZE bytes, in entry order, so entry_common finds the
; return half of stub N at e0 + N * STUB_SIZE + STUB_RETURN. `strict` keeps the
; jump at 3 bytes: nasm would shorten it for the stubs near entry_common.
%macro ENTRY 2
e%1:
        mov     [cs:saved_ax], ax
        mov     ax, %1 | (%2 << 8)
        jmp     strict near entry_common
        ; entry_common returns here on our stack, registers and flags restored
        mov     ss, [cs:saved_ss]
        mov     sp, [cs:saved_sp]       ; no interrupt between the two (mov ss)
        db      0xca                    ; retf B (also for B = 0: fixed size)
        dw      %2
%endmacro
STUB_SIZE       equ 23
STUB_RETURN     equ 10

        ENTRY 0, 0xa
        ENTRY 1, 0
        ENTRY 2, 0xa
        ENTRY 3, 0
        ENTRY 4, 0
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

; Shared body: AL = entry number, AH = argument bytes, the caller's AX in
; saved_ax, SS:SP = the caller's stack (return address, then the arguments).
;
; Nothing is written on the caller's stack: PCB_RM's stack shares a 4 KB page
; with its code, and DOSBox-X's dynamic core takes every write into a page that
; holds translated code through a slow handler (the old thunk made ~21 such
; writes per call, ~350 000 per zoom-in redraw of PR41).
;
; Our stack, from stack_top down: the stub's return offset, the flags (with the
; caller's IF), the pushad frame (its AX, CX, DX and BX words are ret_ax,
; ret_cx, ret_dx and ret_bx: dispatch() writes the return registers in place),
; DS, ES, FS, GS.
entry_common:
        mov     [cs:saved_sp], sp
        mov     [cs:saved_ss], ss
        mov     [cs:saved_entry], ax
        mov     ax, cs
        mov     ss, ax
        mov     esp, stack_top          ; no interrupt between the two (mov ss)
        movzx   ax, byte [cs:saved_entry]
        imul    ax, ax, STUB_SIZE
        add     ax, e0 + STUB_RETURN
        push    ax
        ; Interrupts stay off while the C code runs: 16-bit interrupt handlers
        ; (mouse, timer, extender) may not keep the high halves of the 32-bit
        ; registers gcc -m16 code relies on. PCB386 calls with them on; popf
        ; gives the caller's IF back at the end (sti would cost more: the
        ; dynamic core hands it to the interpreter).
        pushf
        cli
        mov     ax, [cs:saved_ax]
        pushad
        push    ds
        push    es
        push    fs
        push    gs
        cld
        ; copy the caller's stack arguments (last pushed first) into args
        push    cs
        pop     es
        lds     si, [cs:saved_sp]
        add     si, 4
        mov     di, args
        movzx   cx, byte [cs:saved_entry + 1]
        shr     cx, 1
        rep movsw
        push    cs
        pop     ds
        movzx   ax, byte [saved_entry]
        mov     [g_entry], ax
        mov     ax, 0xa000
        mov     gs, ax
        xor     ebp, ebp
        ; 32-bit near call: gcc -m16 code returns with a 32-bit ret.
        ; (`o32 call` would get a 16-bit displacement from nasm.)
        call    dword dispatch
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popad
        popf
        ret                             ; to the return half of the entry's stub

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

; ---- Span kernel ----------------------------------------------------------
; Called from C (doscad.c, span_fill and pixel_put) by 32-bit near calls with
; regparm(3) arguments in eax, edx, ecx; ebx, esi, edi and ebp are kept.
; Every row of the picture goes through span_fill, so it is written for
; DOSBox-X's dynamic core, where each instruction costs the same: few
; instructions per row, the write mode tested once, dwords at any alignment
; (VRAM is host memory there), rep stos for long replace runs.
        extern pitch
        extern current_bank
        extern granularity_kb
        extern span_colour
        extern span_mode
        global span_fill
        global pixel_put

; Moves the VESA window to 64 KB bank EAX and records it in current_bank
; (16-bit near call, every register kept).
bank_switch:
        mov     [current_bank], eax
        pushad
        shl     eax, 6                  ; KB
        xor     edx, edx
        div     dword [granularity_kb]
        mov     edx, eax
        mov     ax, 0x4f05
        xor     bx, bx
        int     0x10
        popad
        ret

; void span_fill(i32 y, i32 left, i32 right_exclusive): pixels left..right-1
; of row y in span_colour / span_mode (0 replace, 1 or, 2 xor); GS = A000.
; Nothing is written when left >= right_exclusive.
; The window offset is addressed as [gs:eax] (32-bit addressing, always below
; 0x10000), so no register needs saving. OR and replace may write a byte twice
; (the last dword overlaps the one before): the result is the same. XOR may not.
span_fill:
        sub     ecx, edx                ; ecx = pixels
        jle     .ret
        imul    eax, [pitch]
        add     eax, edx                ; eax = VRAM offset of the first pixel
.piece:
        mov     edx, eax
        shr     edx, 16
        cmp     edx, [current_bank]
        jne     .switch
.banked:
        movzx   edx, ax
        add     edx, ecx
        cmp     edx, 0x10000
        ja      .split                  ; runs into the next bank
        movzx   eax, ax                 ; eax = offset in the window
        mov     edx, [span_colour]
        cmp     byte [span_mode], 1
        jne     .not_or
        cmp     ecx, 4
        jb      .or_bytes
        lea     ecx, [eax+ecx-4]        ; ecx = offset of the last dword
.or_dwords:
        or      [gs:eax], edx
        add     eax, 4
        cmp     eax, ecx
        jb      .or_dwords
        or      [gs:ecx], edx
.ret:
        db      0x66                    ; 32-bit near ret (gcc -m16 calling convention)
        ret
.or_bytes:
        or      [gs:eax], dl
        inc     eax
        dec     ecx
        jnz     .or_bytes
        db      0x66
        ret
.not_or:
        ja      .xor
        cmp     ecx, 4
        jb      .mov_bytes
        cmp     ecx, 16
        jae     .mov_string
        lea     ecx, [eax+ecx-4]
.mov_dwords:
        mov     [gs:eax], edx
        add     eax, 4
        cmp     eax, ecx
        jb      .mov_dwords
        mov     [gs:ecx], edx
        db      0x66
        ret
.mov_bytes:
        mov     [gs:eax], dl
        inc     eax
        dec     ecx
        jnz     .mov_bytes
        db      0x66
        ret
.mov_string:                            ; 16 bytes or more: rep stos, ES = GS
        push    edi
        push    es
        push    gs
        pop     es
        mov     edi, eax
        mov     eax, edx
        mov     edx, ecx
        shr     ecx, 2
        rep stosd
        mov     ecx, edx
        and     ecx, 3
        rep stosb
        pop     es
        pop     edi
        db      0x66
        ret
.xor:                                   ; no overlap: dwords, then bytes
        push    ecx
        shr     ecx, 2
        jz      .xor_tail
.xor_dwords:
        xor     [gs:eax], edx
        add     eax, 4
        dec     ecx
        jnz     .xor_dwords
.xor_tail:
        pop     ecx
        and     ecx, 3
        jz      .ret
.xor_bytes:
        xor     [gs:eax], dl
        inc     eax
        dec     ecx
        jnz     .xor_bytes
        db      0x66
        ret
.switch:
        xchg    eax, edx
        call    bank_switch
        xchg    eax, edx
        jmp     .banked
.split:                                 ; a row crosses at most one bank end
        sub     edx, 0x10000            ; edx = pixels in the next bank
        sub     ecx, edx                ; ecx = pixels in this one
        push    eax
        push    ecx
        push    edx
        call    dword .banked           ; this bank's part, back by the 32-bit ret
        pop     ecx
        pop     edx
        pop     eax
        add     eax, edx                ; the next bank's part
        jmp     .piece

; void pixel_put(i32 x, i32 y): one pixel in span_colour / span_mode.
pixel_put:
        imul    edx, [pitch]
        add     eax, edx
        mov     edx, eax
        shr     edx, 16
        cmp     edx, [current_bank]
        jne     .switch
.banked:
        movzx   eax, ax
        mov     dl, [span_colour]
        cmp     byte [span_mode], 1
        je      .or
        ja      .xor
        mov     [gs:eax], dl
        db      0x66
        ret
.or:
        or      [gs:eax], dl
        db      0x66
        ret
.xor:
        xor     [gs:eax], dl
        db      0x66
        ret
.switch:
        xchg    eax, edx
        call    bank_switch
        xchg    eax, edx
        jmp     .banked

; INT 16h hook. PCB_RM, DRAFT and ESP wait for the user by asking "key
; waiting?" (AH=01h/11h) hundreds of thousands of times a second; in DOSBox-X
; each question is a costly callback, so the emulator runs one host core flat
; out and, with fixed cycles, falls behind real time (the mouse lags).
; More than IDLE_POLLS empty polls within one BIOS tick (55 ms) means the
; program is only waiting: each further empty poll of that tick halts the CPU
; until the next interrupt (timer, keyboard, mouse). A program that polls
; while it computes (PCB386's autorouter) stays far below the limit. No halt
; when the mouse moved or a button changed since the last poll (INT 33h
; function 3 leaves the motion counters to the program), so the arrow
; follows the mouse at the full rate.
IDLE_POLLS equ 500
        global int16_hook
        global old16
int16_hook:
        cmp     ah, 0x01
        je      .poll
        cmp     ah, 0x11
        je      .poll
        jmp     far [cs:old16]
.poll:
        pushf
        call    far [cs:old16]
        jnz     .key
        push    ax
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, [0x46c]     ; BIOS tick count, low word
        cmp     ax, [cs:poll_tick]
        je      .same_tick
        mov     [cs:poll_tick], ax
        mov     word [cs:poll_count], 0
.same_tick:
        cmp     word [cs:poll_count], IDLE_POLLS
        jae     .idle
        inc     word [cs:poll_count]
        jmp     .empty
.idle:
        push    bx
        push    cx
        push    dx
        mov     ax, 3
        int     0x33
        cmp     cx, [cs:mouse_x]
        jne     .moved
        cmp     dx, [cs:mouse_y]
        jne     .moved
        cmp     bx, [cs:mouse_b]
        jne     .moved
        pop     dx
        pop     cx
        pop     bx
        sti
        hlt
        jmp     .empty
.moved:
        mov     [cs:mouse_x], cx
        mov     [cs:mouse_y], dx
        mov     [cs:mouse_b], bx
        pop     dx
        pop     cx
        pop     bx
.empty:
        pop     ds
        pop     ax
        sti
        cmp     ax, ax          ; ZF = 1: no key
        retf    2
.key:
        mov     word [cs:poll_count], 0
        sti
        retf    2               ; ZF = 0 from the BIOS

        alignb  2
old16:          dw 0, 0         ; the previous INT 16h vector (offset, segment)
poll_tick:      dw 0
poll_count:     dw 0
mouse_x:        dw 0
mouse_y:        dw 0
mouse_b:        dw 0

        section .bss
        alignb  16
; The stack comes first, right after the code and constants (link.ld puts this
; .bss before the C data): its unused bottom keeps the data written on every
; call out of the 4 KB pages that hold code, which DOSBox-X's dynamic core
; writes through a slow handler. So keep at least 4 KB of it unused.
; 6 KB: the deepest chain (wide line, arc end, circle) used 1404 bytes at most
; (measured 2026-11-24 in PCB386 and DRAFT with zooms).
stack_bottom:   resb 6144
stack_top:
        alignb  4
args:           resw 32
g_entry:        resw 1
saved_sp:       resw 1      ; saved_sp, saved_ss: one far pointer (lds)
saved_ss:       resw 1
saved_ax:       resw 1
saved_entry:    resw 1      ; entry number, then argument bytes
; The return registers: the caller's AX, CX, DX and BX as pushad stored them
; on our stack, under the stub's return offset and the flags (entry_common).
ret_ax          equ stack_top - 8
ret_cx          equ stack_top - 12
ret_dx          equ stack_top - 16
ret_bx          equ stack_top - 20
