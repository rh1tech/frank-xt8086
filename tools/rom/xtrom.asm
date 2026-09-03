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

; 40:F0 is the inter-application communication area, sixteen bytes the BIOS
; reserves and nothing in a DOS machine uses. The displaced INT 10h vector
; goes there because this code runs from ROM and has nowhere of its own to
; put it.
BDA_OLD_INT10   equ 00F0h

; The firmware's video-mode port. Not a register any real machine has:
; setting a VGA mode means programming a sequencer, a graphics controller,
; an attribute controller and a CRTC, and there is neither a card here nor
; a BIOS that knows how to drive one. So the mode is named directly.
FW_VIDEO        equ 03DCh
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

        ; INT 10h, for one function only. See int10 for why.
        mov     ax, BDA_SEG
        mov     es, ax
        mov     ax, [10h*4]
        mov     [es:BDA_OLD_INT10], ax
        mov     ax, [10h*4+2]
        mov     [es:BDA_OLD_INT10+2], ax
        mov     word [10h*4], int10
        mov     [10h*4+2], cs

        ; INT 2Fh is deliberately NOT hooked here.
        ;
        ; It was, and it could never have worked: DOS installs its own
        ; INT 2Fh handler afterwards and does not pass AH=11h down the
        ; chain, because a redirector is expected to hook *after* DOS and
        ; sit above it. Reading the guest's vector table from the firmware
        ; confirmed it -- INT 2Fh pointed into the DOS kernel, not here.
        ; REDIR.COM does that job from AUTOEXEC.BAT instead.
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
; INT 10h - mode 7 becomes mode 3, and nothing else is touched.
;
; Mode 7 is MDA text. GLaBIOS is an XT BIOS configured here for a CGA, and
; asked for a mode that needs a monochrome card it does not believe is
; present it writes a mismatched pair: the graphics CRTC table together
; with 0x29, the MDA mode byte. The result is a display programmed for 40
; columns of two-scanline characters, cleared to attribute 0. Black on
; black, on a screen whose geometry is wrong as well.
;
; That is reached by ordinary software. Planet X3 asks for mode 7 on the
; way into Hercules graphics and again on the way out, so leaving the game
; returned DOS to an invisible prompt.
;
; This machine has one display. Presenting MDA text as CGA text is the
; honest reading of that: the BIOS then does its own correct mode 3 setup,
; the right CRTC table, attribute 7, and a screen that can be seen. The
; Hercules graphics modes are unaffected, because software drives those
; through the card's own registers and never through the BIOS.
;
; Everything else chains to the original handler untouched.
; ===========================================================================
int10:
        cmp     ah, 12h
        je      .altselect
        cmp     ah, 00h
        jne     .chain
        cmp     al, 13h
        je      .vgamode
        cmp     al, 0Dh
        je      .vgamode
        cmp     al, 0Eh
        je      .vgamode
        cmp     al, 10h
        je      .vgamode

        ; Any other mode set gives the 0xA0000 window back, so a program
        ; that leaves mode 13h stops being a VGA.
        push    ax
        push    dx
        mov     dx, FW_VIDEO
        xor     al, al
        out     dx, al
        pop     dx
        pop     ax

        cmp     al, 07h
        jne     .chain
        mov     al, 03h                 ; MDA text on a machine with one display
        jmp     short .chain

; ---------------------------------------------------------------------------
; INT 10h AH=12h BL=10h - "return EGA information".
;
; The one call every EGA-aware program makes to find out whether an EGA is
; there at all. It is answered here because GLaBIOS is an XT BIOS written
; for a CGA and has no function 12h: the call falls through its dispatch
; and returns with BL still 10h, which is precisely the "no EGA" answer.
;
; Dangerous Dave 2 asks this before it will use a single EGA mode. Told no,
; it sets mode 0Dh anyway, clears the screen and then waits -- a running
; CPU, a correct planar mode, and nothing ever drawn into it. The card is
; real and the modes work; only this answer was missing.
;
; The reply describes what this machine actually has: a colour display at
; 3Dx, the full 256K the card is given in chipset/vgacard.h, no feature
; connector, and the switch setting for an enhanced colour monitor.
;
; Any other subfunction of 12h chains, so the BIOS keeps whatever it does
; provide.
; ---------------------------------------------------------------------------
.altselect:
        cmp     bl, 10h
        jne     .chain
        mov     bh, 0                   ; colour, registers at 3Dx
        mov     bl, 3                   ; 256K of display memory
        mov     ch, 0                   ; feature connector: nothing wired
        mov     cl, 9                   ; switches: enhanced colour display
        iret

; ---------------------------------------------------------------------------
; The EGA and VGA graphics modes: 0Dh, 0Eh, 10h and 13h.
;
; Handled here rather than chained, because GLaBIOS is an XT BIOS and has
; never heard of any of them. The firmware is told which mode to provide, the BIOS
; data area is filled in so that anything asking what mode this is gets a
; straight answer, and the framebuffer is cleared the way a real mode set
; would leave it.
; ---------------------------------------------------------------------------
.vgamode:
        push    ax
        push    cx
        push    di
        push    es
        push    dx

        mov     dx, FW_VIDEO
        out     dx, al                  ; AL is still the mode number

        mov     ah, al                  ; keep it for the BIOS data area
        mov     ax, BDA_SEG
        mov     es, ax
        mov     [es:0049h], ah          ; current mode
        mov     word [es:004Ah], 40     ; text columns, such as they are
        mov     word [es:004Ch], 0      ; page size
        mov     word [es:004Eh], 0      ; page offset
        mov     byte [es:0062h], 0      ; active page
        mov     byte [es:0084h], 24     ; rows - 1

        mov     ax, 0A000h
        mov     es, ax
        xor     di, di
        xor     ax, ax
        mov     cx, 32000               ; 64,000 bytes, a word at a time
        cld
        rep     stosw

        pop     dx
        pop     es
        pop     di
        pop     cx
        pop     ax
        iret

.chain:
        ; Hand over to the original with every register as the caller left
        ; it, and with our INT frame still beneath: a far jump lets its
        ; IRET return straight to the caller.
        ;
        ; Room for the far pointer is made first, then filled in through
        ; BP once the registers used to fetch it have somewhere to be
        ; saved. RETF then jumps to it with nothing of ours left on the
        ; stack.
        sub     sp, 4
        push    bp
        mov     bp, sp                  ; [bp+2] offset, [bp+4] segment
        push    ds
        push    ax
        mov     ax, BDA_SEG
        mov     ds, ax
        mov     ax, [BDA_OLD_INT10]
        mov     [bp+2], ax
        mov     ax, [BDA_OLD_INT10+2]
        mov     [bp+4], ax
        pop     ax
        pop     ds
        pop     bp
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
