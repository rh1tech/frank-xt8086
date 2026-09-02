; frank-xt8086 - an RP2350B acting as the whole chipset for a real 8086
;
; Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
; https://github.com/rh1tech/frank-xt8086
; SPDX-License-Identifier: GPL-3.0-or-later
;
; REDIR.COM - map the microSD as drive H:.
;
; Replaces MAPDRIVE.COM, and does the half it was missing. MAPDRIVE only
; flags H: in DOS's current-directory structure as a network drive; it
; relies on the emulator underneath trapping INT 2Fh in its instruction
; decoder, which is what pico-286 does and what a real 8086 makes
; impossible. So this also stays resident and answers the calls.
;
; It must be a TSR, and that is not a preference. An option ROM hooking
; INT 2Fh during POST does not work: DOS installs its own handler on top
; afterwards and does not pass AH=11h down the chain, because a redirector
; is expected to hook *after* DOS and sit above it. Verified by reading
; the guest's vector table from the firmware -- INT 2Fh pointed into the
; DOS kernel, not at our ROM.
;
; The work happens in the firmware. This is the courier: it hands the
; register file over through an I/O port, waits, and brings the answers
; back. The firmware can already read every byte of RAM, so the register
; file is the only thing it cannot see for itself.
;
; 8086 only. Assemble: nasm -f bin redir.asm -o redir.com

    org 100h
    cpu 8086

DRIVE_LETTER    equ 'H'
DRIVE_NUMBER    equ 7                   ; H = 7
CDS_ENTRY_SIZE  equ 058h                ; DOS 4+ current-directory entry
CDS_OFF_FLAGS   equ 043h
CDSFLAG_PHY     equ 04000h
CDSFLAG_NET     equ 08000h
CDSFLAG_NET_PHY equ (CDSFLAG_PHY | CDSFLAG_NET)

FW_DATA         equ 0E0h
FW_CTRL         equ 0E2h
FW_CMD_BEGIN    equ 0
FW_CMD_EXEC     equ 1

; ===========================================================================
; The resident part comes first, so the transient installer below it can be
; discarded by keeping only the paragraphs up to `resident_end`.
; ===========================================================================
start:
        jmp     install

old_2f  dd      0                       ; the handler we displaced

; ---------------------------------------------------------------------------
int2f:
        cmp     ah, 11h
        jne     .chain

        ; Function 11h with no subfunction we recognise still belongs to
        ; whoever is below us; only the calls for our drive are ours. The
        ; firmware decides that, so everything AH=11h goes across.
        push    bp
        push    es
        push    ds
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        mov     bp, sp
        ; [bp+00] AX  [bp+02] BX  [bp+04] CX  [bp+06] DX
        ; [bp+08] SI  [bp+10] DI  [bp+12] DS  [bp+14] ES
        ; [bp+16] BP  [bp+18] IP  [bp+20] CS  [bp+22] FLAGS

        mov     dx, FW_CTRL
        mov     al, FW_CMD_BEGIN
        out     dx, al

        mov     dx, FW_DATA
        mov     cx, 8
        xor     si, si
.send:
        mov     ax, [bp+si]
        out     dx, ax
        add     si, 2
        loop    .send

        mov     ax, ss
        out     dx, ax
        mov     ax, bp
        add     ax, 18                  ; the caller's SP, before our pushes
        out     dx, ax

        mov     dx, FW_CTRL
        mov     al, FW_CMD_EXEC
        out     dx, al
.wait:
        in      al, dx
        or      al, al
        jz      .wait

        ; AL from the firmware says whether it handled the call at all.
        ; Declining has to mean passing it down the chain, not failing it:
        ; another redirector may be below us, and DOS itself relies on the
        ; chain ending somewhere sensible.
        mov     dx, FW_DATA
        mov     cx, 6
        xor     si, si
.recv:
        in      ax, dx
        mov     [bp+si], ax
        add     si, 2
        loop    .recv

        in      ax, dx                  ; 0 = clear carry, 1 = set, 2 = decline
        cmp     ax, 2
        je      .decline

        or      ax, ax
        jz      .clear_cf
        or      word [bp+22], 0001h
        jmp     short .restore
.clear_cf:
        and     word [bp+22], 0FFFEh
.restore:
        pop     ax
        pop     bx
        pop     cx
        pop     dx
        pop     si
        pop     di
        pop     ds
        pop     es
        pop     bp
        iret

.decline:
        pop     ax
        pop     bx
        pop     cx
        pop     dx
        pop     si
        pop     di
        pop     ds
        pop     es
        pop     bp
.chain:
        jmp     far [cs:old_2f]

resident_end:

; ===========================================================================
; Installer. Everything below here is discarded.
; ===========================================================================
install:
        mov     dx, msg_banner
        mov     ah, 09h
        int     21h

        ; --- List of Lists: INT 21h AH=52h -> ES:BX ---
        mov     ah, 52h
        int     21h

        mov     si, 021h
        mov     dl, [es:bx+si]          ; LASTDRIVE

        mov     si, 016h
        les     bx, [es:bx+si]          ; ES:BX = CDS base

        cmp     bx, 0FFFFh
        jne     .cds_ok
        mov     ax, es
        cmp     ax, 0FFFFh
        je      .err_cds
.cds_ok:
        mov     al, DRIVE_NUMBER
        cmp     al, dl
        jg      .err_lastdrive

        ; DI = CDS base + drive * 58h
        mov     di, bx
        xor     ax, ax
        mov     al, DRIVE_NUMBER
        mov     bl, CDS_ENTRY_SIZE
        mul     bl
        add     di, ax

        mov     word [es:di+CDS_OFF_FLAGS], CDSFLAG_NET_PHY
        mov     byte [es:di+0], DRIVE_LETTER
        mov     byte [es:di+1], ':'
        mov     byte [es:di+2], '\'
        mov     byte [es:di+3], 0

        ; --- hook INT 2Fh, above DOS ---
        mov     ax, 352Fh
        int     21h                     ; ES:BX = current handler
        mov     [old_2f], bx
        mov     [old_2f+2], es

        push    ds
        mov     ax, cs
        mov     ds, ax
        mov     dx, int2f
        mov     ax, 252Fh
        int     21h                     ; DS:DX = ours
        pop     ds

        mov     dx, msg_ok
        mov     ah, 09h
        int     21h

        ; --- stay resident ---
        ; Keep from the PSP through resident_end, rounded up to whole
        ; paragraphs. The installer above this point is given back.
        mov     dx, (resident_end - start + 100h + 15) / 16
        mov     ax, 3100h
        int     21h

.err_cds:
        mov     dx, err_cds
        mov     ah, 09h
        int     21h
        jmp     .quit
.err_lastdrive:
        mov     dx, err_last
        mov     ah, 09h
        int     21h
.quit:
        mov     ax, 4C01h
        int     21h

msg_banner db 'REDIR - microSD as drive H:', 13, 10, '$'
msg_ok     db '  resident; H: is the card', 13, 10, '$'
err_cds    db '  cannot reach the CDS for H:', 13, 10, '$'
err_last   db '  LASTDRIVE in CONFIG.SYS must be H or later', 13, 10, '$'
