; frank-xt8086 - an RP2350B acting as the whole chipset for a real 8086
;
; Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
; https://github.com/rh1tech/frank-xt8086
; SPDX-License-Identifier: GPL-3.0-or-later
;
; SETCLOCK.COM - set the DOS clock from the CMOS RTC.
;
; GLaBIOS is an XT BIOS and an XT had no CMOS clock, so it does not read
; one: DOS takes its date and time from the BIOS tick counter, which
; starts at midnight on every boot. The firmware does present a real
; MC146818 at 70h/71h, backed by the battery-backed DS3231 on the board,
; and this reads it and hands the answer to DOS.
;
; No TSR and nothing resident: it sets the clock once and exits. Run it
; from AUTOEXEC.BAT.
;
; 8086 only - no 80186 instructions, since the CPU in the socket is the
; real thing.
;
; Assemble:  nasm -f bin setclock.asm -o setclock.com

    org 100h
    cpu 8086

; --- CMOS registers --------------------------------------------------------
CMOS_SEC        equ 000h
CMOS_MIN        equ 002h
CMOS_HOUR       equ 004h
CMOS_DAY        equ 007h
CMOS_MONTH      equ 008h
CMOS_YEAR       equ 009h
CMOS_STATUS_A   equ 00Ah
CMOS_STATUS_B   equ 00Bh
CMOS_STATUS_D   equ 00Dh
CMOS_CENTURY    equ 032h

CMOS_A_UIP      equ 080h        ; update in progress
CMOS_B_BINARY   equ 004h        ; set = binary, clear = BCD
CMOS_D_VRT      equ 080h        ; set = the clock has been set since power loss

start:
        mov     dx, msg_banner
        call    puts

; --- is the clock worth believing? ----------------------------------------
; Status D bit 7 is the RTC's own answer to "have I lost power since
; anyone last set me". Refusing to act on a clock that says no is the
; whole point of the bit; setting DOS to a meaningless date is worse than
; leaving it at the boot default, because at least the default is
; obviously wrong.
        mov     al, CMOS_STATUS_D
        call    cmos_read
        test    al, CMOS_D_VRT
        jnz     .clock_ok
        mov     dx, msg_invalid
        call    puts
        mov     ax, 4C01h
        int     21h

.clock_ok:
; --- wait out any update in progress --------------------------------------
; The time registers must not be read while the chip is copying them. This
; firmware updates them atomically so UIP is always clear, but a real
; MC146818 would not be, and the loop costs nothing when it never spins.
        mov     cx, 0
.wait_uip:
        mov     al, CMOS_STATUS_A
        call    cmos_read
        test    al, CMOS_A_UIP
        jz      .no_update
        loop    .wait_uip
.no_update:

; --- binary or BCD? -------------------------------------------------------
        mov     al, CMOS_STATUS_B
        call    cmos_read
        and     al, CMOS_B_BINARY
        mov     [is_binary], al

; --- read it --------------------------------------------------------------
        mov     al, CMOS_SEC
        call    cmos_value
        mov     [t_sec], al
        mov     al, CMOS_MIN
        call    cmos_value
        mov     [t_min], al
        mov     al, CMOS_HOUR
        call    cmos_value
        mov     [t_hour], al
        mov     al, CMOS_DAY
        call    cmos_value
        mov     [t_day], al
        mov     al, CMOS_MONTH
        call    cmos_value
        mov     [t_mon], al
        mov     al, CMOS_YEAR
        call    cmos_value
        mov     [t_year], al
        mov     al, CMOS_CENTURY
        call    cmos_value
        mov     [t_cent], al

; A century register is a convention rather than a standard, so a value
; that is not a plausible century means the field was never written.
; Twenty is the only century this hardware can be running in.
        mov     al, [t_cent]
        cmp     al, 19
        jb      .fix_century
        cmp     al, 21
        jbe     .century_ok
.fix_century:
        mov     byte [t_cent], 20
.century_ok:

; --- set the DOS date: INT 21h AH=2Bh, CX=year DH=month DL=day ------------
        mov     al, [t_cent]
        mov     bl, 100
        mul     bl                      ; AX = century * 100
        mov     bl, [t_year]
        xor     bh, bh
        add     ax, bx
        mov     cx, ax
        mov     dh, [t_mon]
        mov     dl, [t_day]
        mov     ah, 2Bh
        int     21h
        or      al, al
        jz      .date_ok
        mov     dx, msg_baddate
        call    puts
        jmp     .finish
.date_ok:

; --- set the DOS time: INT 21h AH=2Dh, CH=hour CL=min DH=sec DL=1/100 -----
        mov     ch, [t_hour]
        mov     cl, [t_min]
        mov     dh, [t_sec]
        xor     dl, dl
        mov     ah, 2Dh
        int     21h

.finish:
        mov     dx, msg_done
        call    puts
        mov     ax, 4C00h
        int     21h

; ---------------------------------------------------------------------------
; cmos_read - AL = register number in, AL = raw value out.
;
; Bit 7 of the index port is the NMI mask. It is left clear, which enables
; NMI - the firmware ignores the bit entirely, but a stray 1 here would
; leave real hardware with NMI disabled after this program exits.
; ---------------------------------------------------------------------------
cmos_read:
        push    cx
        and     al, 7Fh
        out     70h, al
        ; A short settle between selecting a register and reading it. Real
        ; parts want a few bus cycles; jmp $+2 is the traditional way to
        ; spend one without a timer.
        jmp     short $+2
        jmp     short $+2
        in      al, 71h
        pop     cx
        ret

; cmos_value - as cmos_read, but converted from BCD unless the chip is in
; binary mode.
cmos_value:
        call    cmos_read
        cmp     byte [is_binary], 0
        jne     .done                   ; already binary
        push    cx
        mov     ah, al
        and     al, 0Fh                 ; low digit
        mov     cl, 4
        shr     ah, cl
        push    ax
        mov     al, ah
        mov     ah, 10
        mul     ah                      ; AX = high digit * 10
        mov     cx, ax
        pop     ax
        and     ax, 000Fh
        add     ax, cx
        pop     cx
.done:
        ret

; puts - DX = offset of a '$'-terminated string
puts:
        push    ax
        mov     ah, 09h
        int     21h
        pop     ax
        ret

; ---------------------------------------------------------------------------
is_binary   db 0
t_sec       db 0
t_min       db 0
t_hour      db 0
t_day       db 0
t_mon       db 0
t_year      db 0
t_cent      db 0

msg_banner  db 'SETCLOCK - CMOS RTC to DOS clock', 13, 10, '$'
msg_invalid db 'RTC reports its time is not valid (battery?); clock unchanged.', 13, 10, '$'
msg_baddate db 'DOS rejected the date from the RTC; time not set.', 13, 10, '$'
msg_done    db 'Clock set from the RTC.', 13, 10, '$'
