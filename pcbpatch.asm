; SPDX-FileCopyrightText: 2026 Joaquim Serra
; SPDX-License-Identifier: GPL-2.0-or-later
;
; Developed by Claude Opus 5.5 in high effort mode.
;
; PCBPATCH.COM: makes PCB386+ v2.22H (17-MAY-96) start 1-2 s (emulated) sooner.
;
; At start-up the Watcom C runtime in PCB386.EXE calibrates its delay(): it
; asks DOS for the time (int 21h AH=2Ch) until the second changes, then counts
; the calls it can make during one more second. That is 1-2 s of emulated time
; at every start, and in DOSBox-X each call is a Phar Lap switch to real mode
; and back: at cycles=fixed 1000000 the start takes about 8 s instead of 1 s.
; PCB386 uses delay() only for the length of its 30 ms beep. The patch replaces
; the start of the calibration routine (file offset 0xE9298) with
;     mov dword [0x150a14], 1000000   ; the calls-per-second count
;     ret
; so the beep keeps a length of the same order and nothing else changes.
;
; Usage: PCBPATCH [/U] [file]
;   file  PCB386.EXE to patch; default PCB386.EXE in the current directory,
;         else %ORCADEXE%PCB386.EXE.
;   /U    undo: put the original bytes back.
;
; Flow:
;  1. Read the switch and the file name from the command line.
;  2. Open the file; check its size and the 50 bytes at 0xE9298: original,
;     patched, or anything else (another version: nothing is changed).
;  3. Patch: copy the file to <name>.ORG once (kept if it exists), then write
;     the 11 new bytes. Undo: check that <name>.ORG holds the original bytes and
;     copy its 11 bytes back.
;
; The program holds no byte of PCB386.EXE: it recognises the original and the
; patched routine by their size and CRC-32, and takes the bytes for an undo
; from the user's own backup.
;  4. Print the result; exit code 0 on success or nothing to do, 1 on error.

        cpu     8086
        org     0x100

FILE_SIZE_LO    equ 0x8804              ; 1 411 076 bytes = 0x00158804
FILE_SIZE_HI    equ 0x0015
PATCH_OFF_LO    equ 0x9298              ; file offset 0x000E9298
PATCH_OFF_HI    equ 0x000E
CHECK_LEN       equ 50                  ; the calibration routine
PATCH_LEN       equ 11
CRC_ORIGINAL    equ 0x12abc5ae          ; CRC-32 of the 50 original bytes
CRC_TAIL        equ 0x1ca554ea          ; CRC-32 of their last 39 (kept by the patch)
COPY_BUF        equ 0x4000              ; 16 KB copy buffer (in this segment)

start:
        cld
        ; 1. Command line: PSP 0x80 = length, 0x81 = text.
        mov     si, 0x81
        mov     cl, [0x80]
        xor     ch, ch
        mov     di, name_buf
.scan:
        jcxz    .scanned
        lodsb
        dec     cx
        cmp     al, ' '
        jbe     .scan
        cmp     al, '/'
        jne     .name
        jcxz    .scanned
        lodsb
        dec     cx
        and     al, 0xdf                ; upper case
        cmp     al, 'U'
        jne     usage
        mov     byte [undo], 1
        jmp     .scan
.name:
        cmp     di, name_buf
        jne     usage                   ; a second file name
.copy_name:
        stosb
        jcxz    .name_end
        lodsb
        dec     cx
        cmp     al, ' '
        ja      .copy_name
.name_end:
        mov     byte [di], 0
        jmp     .scan
.scanned:
        cmp     di, name_buf
        jne     open_file
        ; No name: PCB386.EXE here, else in %ORCADEXE%.
        mov     si, default_name
        mov     di, name_buf
        call    strcpy
        mov     dx, name_buf
        mov     ax, 0x4300              ; get attributes: does it exist?
        int     0x21
        jnc     open_file
        call    orcadexe_path
        jc      not_found

open_file:
        ; 2. Open read/write and check the size.
        mov     dx, name_buf
        mov     ax, 0x3d02
        int     0x21
        jc      not_found
        mov     [handle], ax
        mov     bx, ax
        call    read_check
        jc      io_error
        jnz     other_version
        ; Original: the CRC of all 50 bytes. Patched: the new 11, then the
        ; original rest.
        mov     si, check_buf
        mov     cx, CHECK_LEN
        call    crc32
        cmp     ax, CRC_ORIGINAL & 0xffff
        jne     .not_original
        cmp     dx, CRC_ORIGINAL >> 16
        je      is_original
.not_original:
        mov     si, check_buf
        mov     di, patched
        mov     cx, PATCH_LEN
        call    compare
        jne     other_version
        mov     si, check_buf + PATCH_LEN
        mov     cx, CHECK_LEN - PATCH_LEN
        call    crc32
        cmp     ax, CRC_TAIL & 0xffff
        jne     other_version
        cmp     dx, CRC_TAIL >> 16
        jne     other_version
        ; Already patched.
        cmp     byte [undo], 0
        je      .already
        call    original_from_backup    ; 3. undo
        jc      done_error              ; message printed
        mov     si, check_buf
        call    write_patch
        jc      io_error
        mov     dx, msg_restored
        jmp     done_ok
.already:
        mov     dx, msg_already
        jmp     done_ok

is_original:
        cmp     byte [undo], 0
        je      .patch
        mov     dx, msg_not_patched
        jmp     done_ok
.patch:
        call    backup
        jc      done_error              ; backup prints its own message
        mov     si, patched
        call    write_patch
        jc      io_error
        mov     dx, msg_patched
        call    print
        mov     dx, msg_backup
        call    print
        mov     si, backup_name
        call    print_z
        mov     dx, msg_crlf
        jmp     done_ok

other_version:
        mov     dx, msg_other
        jmp     done_err_msg
not_found:
        mov     dx, msg_not_found
        call    print
        mov     si, name_buf
        call    print_z
        mov     dx, msg_crlf
        jmp     done_err_msg
io_error:
        mov     dx, msg_io
        jmp     done_err_msg
usage:
        mov     dx, msg_usage
done_err_msg:
        call    print
done_error:
        call    close
        mov     ax, 0x4c01
        int     0x21
done_ok:
        call    print
        call    close
        mov     ax, 0x4c00
        int     0x21

; Seek the open file to the patch offset. CF on error.
seek_patch:
        mov     bx, [handle]
        mov     ax, 0x4200
        mov     cx, PATCH_OFF_HI
        mov     dx, PATCH_OFF_LO
        int     0x21
        ret

; Write the PATCH_LEN bytes at SI to the patch offset. CF on error.
write_patch:
        call    seek_patch
        jc      .ret
        mov     ah, 0x40
        mov     cx, PATCH_LEN
        mov     dx, si
        int     0x21
        jc      .ret
        cmp     ax, PATCH_LEN
        je      .ret                    ; equal: CF clear
        stc
.ret:
        ret

; Copy the open file to <name>.ORG unless that file exists. CF on error
; (message printed).
backup:
        call    backup_name_make
        mov     dx, backup_name
        mov     ax, 0x4300
        int     0x21
        jnc     .ok                     ; a backup is already there: keep it
        mov     ah, 0x3c                ; create
        xor     cx, cx
        int     0x21
        jc      .fail
        mov     [backup_handle], ax
        mov     bx, [handle]            ; from the start of the original
        mov     ax, 0x4200
        xor     cx, cx
        xor     dx, dx
        int     0x21
        jc      .fail_close
.loop:
        mov     bx, [handle]
        mov     ah, 0x3f
        mov     cx, COPY_BUF
        mov     dx, copy_buf
        int     0x21
        jc      .fail_close
        test    ax, ax
        jz      .copied
        mov     cx, ax
        mov     bx, [backup_handle]
        mov     ah, 0x40
        mov     dx, copy_buf
        int     0x21
        jc      .fail_close
        cmp     ax, cx
        jne     .fail_close             ; disk full
        jmp     .loop
.copied:
        mov     bx, [backup_handle]
        mov     ah, 0x3e
        int     0x21
.ok:
        clc
        ret
.fail_close:
        mov     bx, [backup_handle]
        mov     ah, 0x3e
        int     0x21
        mov     dx, backup_name         ; no half-written backup
        mov     ah, 0x41
        int     0x21
.fail:
        mov     dx, msg_backup_fail
        call    print
        mov     si, backup_name
        call    print_z
        mov     dx, msg_crlf
        call    print
        stc
        ret

; backup_name = name_buf with the extension .ORG.
backup_name_make:
        mov     si, name_buf
        mov     di, backup_name
        call    strcpy                  ; DI -> the terminating 0
        mov     bx, di
.back:
        cmp     bx, backup_name         ; find the extension's dot
        je      .no_dot
        dec     bx
        mov     al, [bx]
        cmp     al, '.'
        je      .dot
        cmp     al, '\'
        je      .no_dot
        cmp     al, ':'
        je      .no_dot
        jmp     .back
.no_dot:
        mov     bx, di
.dot:
        mov     si, ext_org
        mov     di, bx
        jmp     strcpy

; Read the 50 bytes at the patch offset of the file BX into check_buf.
; CF on a read error; else ZF set when the file has the right size and the
; bytes were read.
read_check:
        mov     ax, 0x4202              ; seek to the end: DX:AX = size
        xor     cx, cx
        xor     dx, dx
        int     0x21
        jc      .ret
        cmp     ax, FILE_SIZE_LO
        jne     .differ
        cmp     dx, FILE_SIZE_HI
        jne     .differ
        mov     ax, 0x4200
        mov     cx, PATCH_OFF_HI
        mov     dx, PATCH_OFF_LO
        int     0x21
        jc      .ret
        mov     ah, 0x3f
        mov     cx, CHECK_LEN
        mov     dx, check_buf
        int     0x21
        jc      .ret
        cmp     ax, CHECK_LEN           ; ZF when all were read
.differ:
        clc                             ; keeps ZF
.ret:
        ret

; For an undo: read the original bytes from <name>.ORG into check_buf. CF on
; error (message printed).
original_from_backup:
        call    backup_name_make
        mov     dx, backup_name
        mov     ax, 0x3d00              ; open read-only
        int     0x21
        jc      .fail
        mov     bx, ax
        push    bx
        call    read_check
        pop     bx
        pushf
        mov     ah, 0x3e
        int     0x21
        popf
        jc      .fail
        jnz     .fail
        mov     si, check_buf
        mov     cx, CHECK_LEN
        call    crc32
        cmp     ax, CRC_ORIGINAL & 0xffff
        jne     .fail
        cmp     dx, CRC_ORIGINAL >> 16
        jne     .fail
        clc
        ret
.fail:
        mov     dx, msg_no_backup
        call    print
        mov     si, backup_name
        call    print_z
        mov     dx, msg_crlf
        call    print
        stc
        ret

; CRC-32 (IEEE, as zlib) of CX bytes at SI: DX:AX.
crc32:
        mov     ax, 0xffff
        mov     dx, ax
.byte:
        xor     al, [si]
        inc     si
        mov     bl, 8
.bit:
        shr     dx, 1
        rcr     ax, 1
        jnc     .next
        xor     dx, 0xedb8
        xor     ax, 0x8320
.next:
        dec     bl
        jnz     .bit
        loop    .byte
        not     ax
        not     dx
        ret

; name_buf = %ORCADEXE% + PCB386.EXE, if the variable exists and the file too.
; CF when not.
orcadexe_path:
        push    ds
        mov     es, [0x2c]              ; environment segment
        xor     di, di
.var:
        cmp     byte [es:di], 0
        je      .none
        mov     si, env_name
        mov     bx, di
.cmp:
        lodsb
        test    al, al
        jz      .found
        cmp     al, [es:bx]
        jne     .next
        inc     bx
        jmp     .cmp
.next:
        mov     al, 0                   ; skip to the next variable
        mov     cx, 0xffff
        repne   scasb
        jmp     .var
.found:
        mov     di, name_buf            ; copy the value
        mov     si, bx
        push    es
        pop     ds
        pop     es                      ; ES = our segment
        push    es
.val:
        lodsb
        stosb
        test    al, al
        jnz     .val
        pop     ds
        push    ds
        pop     es
        dec     di
        cmp     di, name_buf
        je      .add
        cmp     byte [di - 1], '\'
        je      .add
        mov     al, '\'
        stosb
.add:
        mov     si, default_name
        call    strcpy
        mov     dx, name_buf
        mov     ax, 0x4300
        int     0x21
        ret
.none:
        pop     ds
        push    ds
        pop     es
        stc
        ret

; Compare CX bytes at SI and DI (DS = ES): ZF set when equal.
compare:
        push    ds
        pop     es
        repe    cmpsb
        ret

; Copy the 0-terminated string at SI to DI; DI ends on the copied 0.
strcpy:
        push    ds
        pop     es
.c:
        lodsb
        stosb
        test    al, al
        jnz     .c
        dec     di
        ret

; Print the $-terminated string at DX.
print:
        mov     ah, 0x09
        int     0x21
        ret

; Print the 0-terminated string at SI.
print_z:
        lodsb
        test    al, al
        jz      .end
        mov     dl, al
        mov     ah, 0x02
        int     0x21
        jmp     print_z
.end:
        ret

; Close the file if it is open.
close:
        mov     bx, [handle]
        test    bx, bx
        jz      .end
        mov     ah, 0x3e
        int     0x21
        mov     word [handle], 0
.end:
        ret

; mov dword [0x150a14], 1000000 / ret
patched:
        db 0xc7, 0x05, 0x14, 0x0a, 0x15, 0x00
        dd 1000000
        db 0xc3

default_name    db 'PCB386.EXE', 0
env_name        db 'ORCADEXE=', 0
ext_org         db '.ORG', 0
undo            db 0
handle          dw 0
backup_handle   dw 0

msg_usage       db 'PCBPATCH [/U] [file]', 13, 10
                db '  Makes PCB386+ v2.22H start without its 1-2 s delay() calibration.', 13, 10
                db '  file: PCB386.EXE (default: current directory, then %ORCADEXE%)', 13, 10
                db '  /U    undo the patch', 13, 10, '$'
msg_patched     db 'PCB386.EXE patched: the delay() calibration is skipped.', 13, 10, '$'
msg_backup      db 'Original kept as $'
msg_already     db 'Already patched: nothing changed (PCBPATCH /U undoes it).', 13, 10, '$'
msg_restored    db 'Patch undone: the original bytes are back.', 13, 10, '$'
msg_not_patched db 'Not patched: nothing to undo.', 13, 10, '$'
msg_other       db 'Not PCB386.EXE v2.22H (17-MAY-96), or patched otherwise: nothing changed.', 13, 10, '$'
msg_not_found   db 'Cannot open $'
msg_io          db 'Read/write error: check that the file is not read-only.', 13, 10, '$'
msg_backup_fail db 'Cannot write the backup, nothing changed: $'
msg_no_backup   db 'Cannot undo: no original backup (PCB386.ORG made by PCBPATCH) at $'
msg_crlf        db 13, 10, '$'

section .bss
name_buf        resb 128
backup_name     resb 132
check_buf       resb CHECK_LEN
copy_buf        resb COPY_BUF
