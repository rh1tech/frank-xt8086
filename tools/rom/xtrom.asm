; frank-xt8086 - an RP2350B acting as the whole chipset for a real 8086
;
; Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
; https://github.com/rh1tech/frank-xt8086
; SPDX-License-Identifier: GPL-3.0-or-later
;
; XTROM - the firmware's option ROM, mapped at D000:0000.
;
; GLaBIOS scans C000-FDFF on 2K boundaries for the 55 AA signature and
; far-calls offset 3 of whatever it finds, before DOS loads. That is the
; only place code of ours can run inside the guest without anyone
; installing a file, which makes it the right home for the clock.
;
; GLaBIOS is an XT BIOS and an XT had no CMOS clock, so it does not read
; one: DOS starts at midnight on every boot no matter what the
; battery-backed DS3231 on the board says. This answers INT 1Ah from the
; MC146818 the firmware emulates at 70h/71h, and seeds the BIOS tick
; counter during POST so DOS is right from its first prompt rather than
; only after something asks.
;
; The whole of INT 1Ah is implemented rather than hooked-and-chained, and
; that is deliberate: this code runs from ROM, so there is nowhere
; writable to keep a displaced vector, and every workaround for that on an
; 8086 costs more than simply answering all six functions. An XT's INT 1Ah
; is AH=00 through AH=05 and nothing else.
;
; 8086 only. Built by tools/rom/build.py, which pads the image to a
; multiple of 512 and fixes the checksum byte the BIOS verifies.

    cpu 8086
    org 0

ROM_BLOCKS      equ 4                   ; 4 * 512 = 2048 bytes

CMOS_SEC        equ 000h
CMOS_MIN        equ 002h
CMOS_HOUR       equ 004h
CMOS_DAY        equ 007h
CMOS_MONTH      equ 008h
CMOS_YEAR       equ 009h
CMOS_STATUS_B   equ 00Bh
CMOS_STATUS_D   equ 00Dh
CMOS_CENTURY    equ 032h
CMOS_B_BINARY   equ 004h
CMOS_D_VRT      equ 080h

; --- the firmware's own I/O window -----------------------------------------
; Two ports nothing on an XT uses. The redirector stub marshals the 8086's
; registers through them rather than through a buffer in guest memory:
; the firmware can read all of RAM directly, so the only thing it cannot
; see is the register file, and eighteen bytes of OUT is a cheaper way to
; hand that over than finding somewhere resident to put it.
;
;   OUT 0E0h, AX   push one register into the request, index auto-advances
;   OUT 0E2h, AL   0 = start a request, 1 = execute it
;   IN  0E2h       0 = still working, 1 = done
;   IN  0E0h, AX   pop one result, same order as they went in
FW_DATA         equ 0E0h
FW_CTRL         equ 0E2h
FW_CMD_BEGIN    equ 0
FW_CMD_EXEC     equ 1

BDA_SEG         equ 0040h
BDA_TICKS       equ 006Ch               ; dword, ticks since midnight
BDA_ROLLOVER    equ 0070h               ; byte, set when the count wrapped

; ===========================================================================
header:
        db      55h, 0AAh
        db      ROM_BLOCKS
        jmp     short init              ; the BIOS far-calls offset 3
        nop

; ===========================================================================
; POST entry. Far call, so it ends in RETF.
; ===========================================================================
init:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es

        mov     si, msg_banner
        call    puts

        ; Status D bit 7 says whether the clock has been set since it last
        ; lost power. When it is clear the date is still used, and the
        ; reason is specific to this machine: the firmware does not pass
        ; the stopped chip's registers through, it free-runs its own clock
        ; from the time the firmware was built. So the value here is always
        ; plausible -- wrong by however long the board has been in
        ; service, never wrong by forty-six years. That beats the 1980 DOS
        ; would otherwise invent, and the warning says which one you have.
        mov     al, CMOS_STATUS_D
        call    cmos_read
        test    al, CMOS_D_VRT
        jnz     .rtc_ok
        mov     si, msg_novrt
        call    puts
.rtc_ok:
        call    read_time_bin           ; CH=hour CL=min DH=sec

        ; Seconds since midnight -> SI
        mov     al, ch
        xor     ah, ah
        mov     bx, 3600
        mul     bx
        mov     si, ax
        mov     al, cl
        xor     ah, ah
        mov     bx, 60
        mul     bx
        add     si, ax
        mov     al, dh
        xor     ah, ah
        add     si, ax

        ; ticks = sec * 18.2065, as sec*18 + sec/5. The error is under a
        ; tenth of a percent and the timer interrupt re-derives everything
        ; from here on, so it never accumulates.
        mov     ax, si
        mov     bx, 18
        mul     bx                      ; DX:AX = sec * 18
        mov     cx, dx
        mov     bx, ax
        mov     ax, si
        xor     dx, dx
        mov     di, 5
        div     di
        add     bx, ax
        adc     cx, 0                   ; CX:BX = ticks

        mov     ax, BDA_SEG
        mov     ds, ax
        mov     [BDA_TICKS], bx
        mov     [BDA_TICKS+2], cx
        mov     byte [BDA_ROLLOVER], 0

        mov     si, msg_set
        call    puts

.hook:
        xor     ax, ax
        mov     ds, ax
        cli
        mov     word [1Ah*4], int1a
        mov     [1Ah*4+2], cs

        ; INT 2Fh, hooked here at POST and deliberately not chained.
        ;
        ; DOS installs its own INT 2Fh handler later, saving whatever it
        ; finds and chaining down to it for anything it does not answer --
        ; so hooking first puts us at the bottom of the chain, which is
        ; exactly where a redirector belongs. DOS routes its 11xx calls
        ; down to us, and anything else that reaches us has already been
        ; declined by everyone above, so an IRET is the correct answer.
        ;
        ; This is what makes a ROM-resident redirector possible at all:
        ; there is nowhere writable in ROM to keep a displaced vector, and
        ; being last in the chain means we never need one.
        mov     word [2Fh*4], int2f
        mov     [2Fh*4+2], cs
        sti

        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

; ===========================================================================
; INT 1Ah - the whole of it.
;
; Returns with RETF 2 rather than IRET throughout: that discards the saved
; flags and leaves the caller looking at the ones we set, which is how the
; carry flag gets back to it.
; ===========================================================================
int1a:
        sti
        cmp     ah, 00h
        je      .get_ticks
        cmp     ah, 01h
        je      .set_ticks
        cmp     ah, 02h
        je      .get_time
        cmp     ah, 03h
        je      .set_time
        cmp     ah, 04h
        je      .get_date
        cmp     ah, 05h
        je      .set_date
        stc                             ; unknown function
        retf    2

; AH=00: CX:DX = tick count, AL = rollover flag (and clear it)
.get_ticks:
        push    ds
        push    bx
        mov     bx, BDA_SEG
        mov     ds, bx
        cli
        mov     dx, [BDA_TICKS]
        mov     cx, [BDA_TICKS+2]
        mov     al, [BDA_ROLLOVER]
        mov     byte [BDA_ROLLOVER], 0
        sti
        pop     bx
        pop     ds
        clc
        retf    2

; AH=01: set the tick count from CX:DX
.set_ticks:
        push    ds
        push    bx
        mov     bx, BDA_SEG
        mov     ds, bx
        cli
        mov     [BDA_TICKS], dx
        mov     [BDA_TICKS+2], cx
        mov     byte [BDA_ROLLOVER], 0
        sti
        pop     bx
        pop     ds
        clc
        retf    2

; AH=02: CH=hour CL=min DH=sec, BCD; DL=0 (no daylight saving)
.get_time:
        call    read_time_bcd
        xor     dl, dl
        clc
        retf    2

; AH=03: set the time from CH/CL/DH
.set_time:
        mov     al, CMOS_HOUR
        mov     ah, ch
        call    cmos_write_bcd
        mov     al, CMOS_MIN
        mov     ah, cl
        call    cmos_write_bcd
        mov     al, CMOS_SEC
        mov     ah, dh
        call    cmos_write_bcd
        clc
        retf    2

; AH=04: CH=century CL=year DH=month DL=day, BCD
.get_date:
        call    read_date_bcd
        clc
        retf    2

; AH=05: set the date from CH/CL/DH/DL
.set_date:
        mov     al, CMOS_CENTURY
        mov     ah, ch
        call    cmos_write_bcd
        mov     al, CMOS_YEAR
        mov     ah, cl
        call    cmos_write_bcd
        mov     al, CMOS_MONTH
        mov     ah, dh
        call    cmos_write_bcd
        mov     al, CMOS_DAY
        mov     ah, dl
        call    cmos_write_bcd
        clc
        retf    2

;============================================================================
; INT 2Fh - the DOS network redirector, for drive H:.
;
; MAPDRIVE.COM flags H: in DOS's current-directory structure as a network
; drive; from then on DOS routes every access to it here as AH=11h.
; Nothing was answering, which is why the drive existed and could not be
; opened.
;
; All the work happens in the firmware. This is the courier: it hands over
; the register file, waits, and brings the answers back.
;
; The registers go out through an I/O port rather than a buffer in guest
; memory, because the firmware can already read every byte of RAM -- the
; register file is the one thing it cannot see, and there is nowhere
; resident in ROM to build a buffer anyway.
;============================================================================
int2f:
        cmp     ah, 11h
        je      .redirect
        iret                            ; not ours, and nothing is below us

.redirect:
        ; Marshalling needs AX and DX for the OUT itself, so the values
        ; have to be somewhere else before it starts. The stack is that
        ; somewhere, and it doubles as the place the results are written
        ; back to, so the POPs at the end deliver them to the caller.
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
        ; [bp+00] AX   [bp+02] BX   [bp+04] CX   [bp+06] DX
        ; [bp+08] SI   [bp+10] DI   [bp+12] DS   [bp+14] ES
        ; [bp+16] BP   [bp+18] IP   [bp+20] CS   [bp+22] FLAGS

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

        ; SS:SP as the caller had them, so the firmware can walk the
        ; caller's stack if a function needs to.
        mov     ax, ss
        out     dx, ax
        mov     ax, bp
        add     ax, 18                  ; undo our nine pushes
        out     dx, ax

        mov     dx, FW_CTRL
        mov     al, FW_CMD_EXEC
        out     dx, al

        ; Spin until it is done. The firmware does the filesystem work on
        ; its other core, so this is a wait on real disk I/O -- the same
        ; wait a physical drive would have imposed, and the CPU has
        ; nothing else to do with it.
.wait:
        in      al, dx
        or      al, al
        jz      .wait

        ; Six words come back, over the saved copies, so the POPs below
        ; hand them to the caller.
        mov     dx, FW_DATA
        mov     cx, 6
        xor     si, si
.recv:
        in      ax, dx
        mov     [bp+si], ax
        add     si, 2
        loop    .recv

        ; The carry flag is the redirector's success/failure channel, and
        ; IRET restores flags from the stack -- so it is set there, in the
        ; caller's saved FLAGS, not in ours.
        in      ax, dx
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

;============================================================================
; CMOS. The NMI mask (bit 7 of the index port) is always left clear.
;============================================================================
cmos_read:
        and     al, 7Fh
        out     70h, al
        jmp     short $+2
        jmp     short $+2
        in      al, 71h
        ret

; AL = register, AH = value already in the chip's format
cmos_write_bcd:
        push    ax
        and     al, 7Fh
        out     70h, al
        jmp     short $+2
        jmp     short $+2
        mov     al, ah
        out     71h, al
        pop     ax
        ret

; AL = register in, AL = binary value out
cmos_bin:
        call    cmos_read
        push    bx
        push    cx
        mov     bl, al
        mov     al, CMOS_STATUS_B
        call    cmos_read
        test    al, CMOS_B_BINARY
        mov     al, bl
        jnz     .done                   ; the chip is already binary
        mov     ah, al
        and     al, 0Fh                 ; units
        mov     cl, 4
        shr     ah, cl                  ; tens
        mov     bl, al
        mov     al, ah
        mov     ah, 10
        mul     ah                      ; AX = tens * 10
        add     al, bl
.done:
        pop     cx
        pop     bx
        ret

read_time_bin:
        mov     al, CMOS_HOUR
        call    cmos_bin
        mov     ch, al
        mov     al, CMOS_MIN
        call    cmos_bin
        mov     cl, al
        mov     al, CMOS_SEC
        call    cmos_bin
        mov     dh, al
        ret

read_time_bcd:
        mov     al, CMOS_HOUR
        call    cmos_read
        mov     ch, al
        mov     al, CMOS_MIN
        call    cmos_read
        mov     cl, al
        mov     al, CMOS_SEC
        call    cmos_read
        mov     dh, al
        ret

read_date_bcd:
        mov     al, CMOS_CENTURY
        call    cmos_read
        mov     ch, al
        mov     al, CMOS_YEAR
        call    cmos_read
        mov     cl, al
        mov     al, CMOS_MONTH
        call    cmos_read
        mov     dh, al
        mov     al, CMOS_DAY
        call    cmos_read
        mov     dl, al
        ret

; ===========================================================================
; puts - SI = offset in CS of a NUL-terminated string.
;
; INT 10h teletype, not INT 21h: this runs during POST and DOS does not
; exist yet. That mistake costs a hang on a machine with no way to say so.
; ===========================================================================
puts:
        push    ax
        push    bx
        push    si
.loop:
        mov     al, [cs:si]
        inc     si
        or      al, al
        jz      .done
        mov     ah, 0Eh
        mov     bx, 0007h
        int     10h
        jmp     .loop
.done:
        pop     si
        pop     bx
        pop     ax
        ret

msg_banner  db 13, 10, 'FRANK XT8086 ROM', 13, 10, 0
msg_novrt   db '  RTC not set: date is approximate (DATE/TIME then SETRTC)', 13, 10, 0
msg_set     db '  clock set from RTC', 13, 10, 0
msg_dummy   db 0
