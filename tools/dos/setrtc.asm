; frank-xt8086 - an RP2350B acting as the whole chipset for a real 8086
;
; Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
; https://github.com/rh1tech/frank-xt8086
; SPDX-License-Identifier: GPL-3.0-or-later
;
; SETRTC.COM - store the DOS date and time into the CMOS RTC.
;
; The other half of SETCLOCK.COM. That one copies the RTC into DOS at
; boot; this one copies DOS into the RTC, which is how the battery-backed
; clock gets set in the first place.
;
; The usual sequence on a board whose coin cell has gone flat:
;
;   DATE          set today's date the DOS way
;   TIME          and the time
;   SETRTC        persist both into the DS3231
;
; After that the firmware trusts the chip -- writing the clock is what
; clears its oscillator-stopped flag -- and SETCLOCK restores it on every
; later boot.
;
; 8086 only. Assemble: nasm -f bin setrtc.asm -o setrtc.com

    org 100h
    cpu 8086

CMOS_SEC        equ 000h
CMOS_MIN        equ 002h
CMOS_HOUR       equ 004h
CMOS_DAY        equ 007h
CMOS_MONTH      equ 008h
CMOS_YEAR       equ 009h
CMOS_STATUS_B   equ 00Bh
CMOS_CENTURY    equ 032h

CMOS_B_BINARY   equ 004h
CMOS_B_SET      equ 080h        ; hold the clock while it is rewritten

start:
        mov     dx, msg_banner
        call    puts

; --- what does DOS think it is? -------------------------------------------
        mov     ah, 2Ah                 ; get date: CX=year DH=month DL=day
        int     21h
        mov     [d_year], cx
        mov     [d_mon], dh
        mov     [d_day], dl

        mov     ah, 2Ch                 ; get time: CH=hour CL=min DH=sec
        int     21h
        mov     [d_hour], ch
        mov     [d_min], cl
        mov     [d_sec], dh

; A year DOS has not been told is 1980, and writing that to a clock is
; worse than leaving it alone: the firmware would then trust it.
        cmp     word [d_year], 1981
        jae     .year_ok
        mov     dx, msg_unset
        call    puts
        mov     ax, 4C01h
        int     21h
.year_ok:

; --- BCD or binary? -------------------------------------------------------
        mov     al, CMOS_STATUS_B
        call    cmos_read
        mov     [stat_b], al
        and     al, CMOS_B_BINARY
        mov     [is_binary], al

; --- hold the clock while it is rewritten ---------------------------------
; Bit 7 of status B is SET: it tells the chip a multi-register update is in
; progress, so it does not advance the seconds between our first write and
; our last and leave the two halves describing different moments.
        mov     al, CMOS_STATUS_B
        mov     ah, [stat_b]
        or      ah, CMOS_B_SET
        call    cmos_write

; --- write it -------------------------------------------------------------
        mov     al, CMOS_SEC
        mov     ah, [d_sec]
        call    cmos_put
        mov     al, CMOS_MIN
        mov     ah, [d_min]
        call    cmos_put
        mov     al, CMOS_HOUR
        mov     ah, [d_hour]
        call    cmos_put
        mov     al, CMOS_DAY
        mov     ah, [d_day]
        call    cmos_put
        mov     al, CMOS_MONTH
        mov     ah, [d_mon]
        call    cmos_put

        ; year: the register holds two digits, the century its own
        mov     ax, [d_year]
        mov     bl, 100
        div     bl                      ; AL = century, AH = year in century
        mov     [tmp_cent], al
        mov     al, CMOS_YEAR
        ; AH already holds the remainder
        call    cmos_put
        mov     al, CMOS_CENTURY
        mov     ah, [tmp_cent]
        call    cmos_put

; --- release the hold -----------------------------------------------------
        mov     al, CMOS_STATUS_B
        mov     ah, [stat_b]
        and     ah, ~CMOS_B_SET & 0FFh
        call    cmos_write

        mov     dx, msg_done
        call    puts
        mov     ax, 4C00h
        int     21h

; ---------------------------------------------------------------------------
; cmos_put - AL = register, AH = binary value; converts to BCD if needed.
; ---------------------------------------------------------------------------
cmos_put:
        cmp     byte [is_binary], 0
        jne     cmos_write              ; chip wants binary; write as-is
        push    ax
        push    cx
        mov     al, ah
        xor     ah, ah
        mov     cl, 10
        div     cl                      ; AL = tens, AH = units
        mov     cl, 4
        shl     al, cl
        or      al, ah                  ; AL = packed BCD
        mov     ah, al
        pop     cx
        pop     bx                      ; recover the register number in BL
        mov     al, bl
        ; fall through

; cmos_write - AL = register, AH = raw value
cmos_write:
        push    ax
        and     al, 7Fh
        out     70h, al
        jmp     short $+2
        jmp     short $+2
        mov     al, ah
        out     71h, al
        pop     ax
        ret

; cmos_read - AL = register in, AL = raw value out
cmos_read:
        and     al, 7Fh
        out     70h, al
        jmp     short $+2
        jmp     short $+2
        in      al, 71h
        ret

puts:
        push    ax
        mov     ah, 09h
        int     21h
        pop     ax
        ret

; ---------------------------------------------------------------------------
is_binary   db 0
stat_b      db 0
tmp_cent    db 0
d_year      dw 0
d_mon       db 0
d_day       db 0
d_hour      db 0
d_min       db 0
d_sec       db 0

msg_banner  db 'SETRTC - DOS clock to CMOS RTC', 13, 10, '$'
msg_unset   db 'DOS date is still the 1980 default; set DATE and TIME first.', 13, 10, '$'
msg_done    db 'RTC set from the DOS clock.', 13, 10, '$'
