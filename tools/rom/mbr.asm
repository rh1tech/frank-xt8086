; frank-xt8086 - an RP2350B acting as the whole chipset for a real 8086
;
; Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
; https://github.com/rh1tech/frank-xt8086
; SPDX-License-Identifier: GPL-3.0-or-later
;
; Master boot record for hdd0.img.
;
; A partition table alone is not enough. XT-IDE tries the hard disk before
; the floppy, and it only moves on if the sector fails its 55 AA check --
; once it loads and jumps, control never returns. A disk carrying a valid
; signature and 446 bytes of zeros is therefore not "not bootable", it is
; a hang, and that is exactly what it did.
;
; DOS needs the signature to see the partition at all, so removing it is
; not an option either. This is the way out: boot the active partition if
; there is one, and otherwise hand off to the floppy, which is the
; behaviour the machine had when the disk was empty.
;
; Once the volume has DOS on it -- SYS C: from A: -- mark the partition
; active with FDISK and this boots it instead, with no change here.
;
; 8086 only. Assembled by tools/rom/build_mbr.py, which splices the result
; into sector 0 without disturbing the partition table.

    cpu 8086
    org 0600h                   ; where we relocate ourselves to

PART_TABLE  equ 07BEh           ; partition table, after relocation
BOOT_ADDR   equ 07C00h

start:
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, BOOT_ADDR   ; stack just below where we load things
        mov     ds, ax
        mov     es, ax
        sti

        ; Move out of 0000:7C00 so a boot sector can be loaded there.
        mov     si, BOOT_ADDR
        mov     di, 0600h
        mov     cx, 256
        cld
        rep     movsw
        jmp     0:relocated

relocated:
        ; Look for an active partition.
        mov     si, PART_TABLE
        mov     cx, 4
.scan:
        cmp     byte [si], 80h
        je      .found
        add     si, 16
        loop    .scan
        jmp     boot_floppy     ; none marked active

.found:
        ; Load its first sector to 0000:7C00. CHS comes straight from the
        ; table entry, which is why those fields have to be right.
        mov     dx, [si]        ; DL = 80h (drive), DH = head
        mov     cx, [si+2]      ; CH/CL = cylinder and sector
        mov     bx, BOOT_ADDR
        mov     ax, 0201h
        int     13h
        jc      boot_floppy
        cmp     word [BOOT_ADDR+510], 0AA55h
        jne     boot_floppy
        mov     si, PART_TABLE  ; convention: DS:SI -> the booted entry
        jmp     0:BOOT_ADDR

; ---------------------------------------------------------------------------
; Hand off to the floppy: load A:'s boot sector and run it.
;
; Three tries with a reset between, because a floppy controller that has
; just been touched by the BIOS often fails the first read, and giving up
; on one error would strand the machine on a disk that is perfectly good.
; ---------------------------------------------------------------------------
boot_floppy:
        mov     cx, 3
.retry:
        push    cx
        xor     ax, ax
        xor     dl, dl
        int     13h             ; reset drive 0
        mov     bx, BOOT_ADDR
        mov     ax, 0201h       ; read 1 sector
        mov     cx, 0001h       ; cylinder 0, sector 1
        xor     dx, dx          ; head 0, drive 0
        int     13h
        pop     cx
        jnc     .loaded
        loop    .retry
        jmp     fail

.loaded:
        cmp     word [BOOT_ADDR+510], 0AA55h
        jne     fail
        xor     dx, dx          ; DL = 0, booting from A:
        jmp     0:BOOT_ADDR

fail:
        mov     si, msg
.putc:
        lodsb
        or      al, al
        jz      .halt
        mov     ah, 0Eh
        mov     bx, 7
        int     10h
        jmp     .putc
.halt:
        hlt
        jmp     .halt

msg     db 13, 10, 'No system on C: and no floppy in A:', 13, 10, 0

    times 446 - ($ - $$) db 0   ; partition table starts at 446
