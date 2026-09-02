/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "redirector.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "fslock.h"
#include "state.h"

// ---------------------------------------------------------------------------
// The wire
// ---------------------------------------------------------------------------

#define FW_CMD_BEGIN 0
#define FW_CMD_EXEC  1

static guest_regs_t regs;
static uint8_t      slot;        // which word of the register file is next
static volatile bool pending;    // a request is waiting for core 0
static volatile bool done;       // ...and core 0 has finished it

/*
 * Three answers, not two.
 *
 * Carry clear is success and carry set is a DOS error, but there is a
 * third case: a call we do not implement. That must be *declined* so the
 * TSR passes it down the chain rather than failing it -- another
 * redirector may be below us, and a call we wrongly claim and botch
 * corrupts data where a declined one merely moves on.
 */
#define ANSWER_OK      0
#define ANSWER_ERROR   1
#define ANSWER_DECLINE 2

static uint8_t answer = ANSWER_DECLINE;

// The register file as an array, so the port handler can index it rather
// than switch on ten cases. The order is the stub's order and the two
// must not drift.
static uint16_t *reg_slot(const uint8_t n) {
    switch (n) {
        case 0: return &regs.ax; case 1: return &regs.bx;
        case 2: return &regs.cx; case 3: return &regs.dx;
        case 4: return &regs.si; case 5: return &regs.di;
        case 6: return &regs.ds; case 7: return &regs.es;
        case 8: return &regs.ss; case 9: return &regs.sp;
        default: return NULL;
    }
}

/*
 * Word I/O arrives here as two byte accesses.
 *
 * port_write() splits a 16-bit OUT into port and port+1, which is what
 * the 8086 bus actually does -- so the stub's `out dx, ax` shows up as a
 * write to 0E0h and one to 0E1h. The low half is staged and the high half
 * commits the pair, which also means a stray byte-sized access cannot
 * advance the register index on its own.
 */
static uint16_t staging;

void redirector_write(const uint16_t port, const uint16_t value, const bool word) {
    (void)word;
    const uint8_t v = (uint8_t)value;

    switch (port) {
        case FW_PORT_CTRL:
            if (v == FW_CMD_BEGIN) {
                slot = 0;
                done = false;
            } else if (v == FW_CMD_EXEC) {
                slot = 0;          // rewind, ready to hand results back
                pending = true;    // core 0 takes it from here
            }
            return;

        case FW_PORT_DATA:              // low half
            staging = (staging & 0xFF00u) | v;
            return;

        case FW_PORT_DATA + 1: {        // high half: commit
            staging = (uint16_t)((staging & 0x00FFu) | ((uint16_t)v << 8));
            uint16_t *r = reg_slot(slot);
            if (r) { *r = staging; slot++; }
            return;
        }
        default: return;
    }
}

uint16_t redirector_read(const uint16_t port, const bool word) {
    (void)word;

    if (port == FW_PORT_CTRL) return done ? 1u : 0u;

    // Six registers come back, then the carry flag as a seventh word.
    if (port == FW_PORT_DATA) {
        if (slot < 6) {
            const uint16_t *r = reg_slot(slot);
            staging = r ? *r : 0;
        } else if (slot == 6) {
            staging = answer;
        } else {
            staging = 0xFFFFu;
        }
        return staging & 0xFFu;
    }

    if (port == FW_PORT_DATA + 1) {
        const uint16_t hi = (staging >> 8) & 0xFFu;
        slot++;                          // the pair is complete
        return hi;
    }
    return 0xFFu;
}

// ---------------------------------------------------------------------------
// DOS
// ---------------------------------------------------------------------------

// The error codes DOS expects back in AX when carry is set.
#define DOS_OK              0x00
#define DOS_FILE_NOT_FOUND  0x02
#define DOS_PATH_NOT_FOUND  0x03
#define DOS_ACCESS_DENIED   0x05
#define DOS_INVALID_DRIVE   0x0F
#define DOS_NO_MORE_FILES   0x12

static void fail(const uint16_t code) { regs.ax = code; answer = ANSWER_ERROR; }
static void ok(void)                  { answer = ANSWER_OK; }
static void decline(void)             { answer = ANSWER_DECLINE; }

/*
 * Read a byte out of the guest's memory.
 *
 * The whole reason this design works: RAM[] *is* the 8086's memory, so a
 * far pointer out of its registers is an array index here. No copying, no
 * window, no marshalling of buffers -- only the register file had to come
 * across the wire.
 */
static uint8_t guest_peek(const uint16_t seg, const uint16_t off) {
    const uint32_t pa = ((uint32_t)seg << 4) + off;
    return pa < RAM_SIZE ? RAM[pa] : 0xFF;
}

static uint8_t guest_peek_lin(const uint32_t pa) {
    return pa < RAM_SIZE ? RAM[pa] : 0xFF;
}

static void guest_poke(const uint16_t seg, const uint16_t off, const uint8_t v) {
    const uint32_t pa = ((uint32_t)seg << 4) + off;
    if (pa < RAM_SIZE) RAM[pa] = v;
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/*
 * AX=110Ch — get disk information, which is what DOS asks before it will
 * believe the drive exists.
 *
 * The numbers are a polite fiction: a card of any size is reported as a
 * 32 MB volume with plenty free, because DOS's fields are 16-bit and the
 * honest answer does not fit. Every emulator does this, and DIR only ever
 * prints the total.
 */
static void fn_disk_info(void) {
    regs.ax = 0x0200;   // sectors per cluster
    regs.bx = 0x0400;   // total clusters
    regs.cx = 0x0200;   // bytes per sector
    regs.dx = 0x0300;   // available clusters
    ok();
}

/*
 * Where the Swappable Data Area lives in guest memory.
 *
 * DOS hands it over once, in BX:DX on the installation check, and every
 * other function needs it: the filename being operated on, the caller's
 * DTA and the search block all live inside it. Without it none of the
 * file operations can be implemented at all.
 */
static uint32_t sda_addr;

static void fn_install_check(void) {
    if (regs.bx || regs.dx) {
        sda_addr = ((uint32_t)regs.bx << 4) + regs.dx;
        printf("[redir] SDA at %04X:%04X (linear %05lX)\n",
               regs.bx, regs.dx, (unsigned long)sda_addr);
    }
    // AL non-zero means "a redirector is here". 0xFF is the conventional
    // answer; DOS only tests for non-zero.
    regs.ax = (regs.ax & 0xFF00u) | 0xFFu;
    ok();
}


// ---------------------------------------------------------------------------
// The Swappable Data Area
// ---------------------------------------------------------------------------
//
// Offsets found by dumping the SDA on this machine rather than assumed:
// the canonicalised search path turned up at +0x9E, which is the DOS 4+
// layout. DR-DOS 8.1 follows it. DOS 3 put the same field at +0x92, so a
// guess either way would have been wrong half the time.
#define SDA_FN1     0x09Eu   // canonicalised name, ASCIIZ, e.g. "H:\\SUB\\*.*"
#define SDA_DTA_OFF 0x00Cu   // the caller's DTA: offset here, segment at +2

/*
 * The search block lives in the caller's DTA, not in the SDA.
 *
 * No SDA layout says so, and entries written into the SDA at every offset
 * a layout suggested were simply never read: DIR kept showing one blank
 * line. DOS hands the redirector the DTA and expects the whole 53-byte
 * block there, its own search state first and the found file after.
 * pico-286's network redirector does exactly this, which is what settled
 * it -- reading a working implementation rather than deducing one.
 *
 *   +0      drive letter, bit 7 set to mark it a redirector's
 *   +1..11  search template
 *   +12     search attributes
 *   +13     entry index, which is ours to use as the position
 *   +15     parent cluster
 *   +17..20 reserved
 *   +21     found file: name[11], attribute, 10 reserved, time, date,
 *           start cluster, then a 32-bit size
 */
#define SDB_DRIVE     0
#define SDB_TEMPLATE  1
#define SDB_ATTR      12
#define SDB_INDEX     13
// Swept over SWD to find where DR-DOS actually reads it.
uint8_t sdb_found_offset = 21;
#define SDB_FOUND     sdb_found_offset
#define FOUND_ATTR    11
#define FOUND_TIME    22
#define FOUND_DATE    24
#define FOUND_CLUSTER 26
#define FOUND_SIZE    28

static void guest_poke_lin(const uint32_t pa, const uint8_t v) {
    if (pa < RAM_SIZE) RAM[pa] = v;
}

static void guest_poke_lin16(const uint32_t pa, const uint16_t v) {
    guest_poke_lin(pa, (uint8_t)v);
    guest_poke_lin(pa + 1, (uint8_t)(v >> 8));
}

static uint16_t guest_peek_lin16(const uint32_t pa) {
    return (uint16_t)guest_peek_lin(pa) | (uint16_t)guest_peek_lin(pa + 1) << 8;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

/*
 * Turn a name into the eleven-byte form a directory entry uses: eight of
 * name and three of extension, space padded, no dot.
 */
static void to_fcb(const char *name, char out[11]) {
    memset(out, ' ', 11);
    int i = 0;
    while (*name && *name != '.' && i < 8) out[i++] = (char)toupper((unsigned char)*name++);
    while (*name && *name != '.') name++;
    if (*name == '.') {
        name++;
        for (i = 0; *name && i < 3; i++) out[8 + i] = (char)toupper((unsigned char)*name++);
    }
}

/*
 * Does an eleven-byte name match an eleven-byte pattern?
 *
 * DOS has already expanded '*' into runs of '?' by the time a redirector
 * sees the template, so '?' is the only wildcard left to honour.
 */
static bool fcb_match(const char pat[11], const char name[11]) {
    for (int i = 0; i < 11; i++)
        if (pat[i] != '?' && pat[i] != name[i]) return false;
    return true;
}

/*
 * Split the canonicalised path into the directory to open and the
 * eleven-byte pattern to match inside it.
 *
 * "H:\SUB\FILE????.???" becomes "/XT/SUB" and "FILE????   ".
 */
static void split_template(const uint32_t sda, char *dir, const size_t dirsz,
                           char pattern[11]) {
    char path[128];
    size_t n = 0;
    for (; n < sizeof path - 1; n++) {
        const uint8_t c = guest_peek_lin(sda + SDA_FN1 + n);
        if (!c) break;
        path[n] = (char)c;
    }
    path[n] = 0;

    // Past "H:", and treat both separators the same.
    const char *p = path;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;

    const char *last = p;
    for (const char *q = p; *q; q++)
        if (*q == '\\' || *q == '/') last = q + 1;

    snprintf(dir, dirsz, "%s", REDIR_ROOT);
    if (last > p) {
        const size_t sub = (size_t)(last - p) - 1;   // without the separator
        const size_t at  = strlen(dir);
        if (at + 1 + sub < dirsz) {
            dir[at] = '/';
            for (size_t i = 0; i < sub; i++)
                dir[at + 1 + i] = (p[i] == '\\') ? '/' : p[i];
            dir[at + 1 + sub] = 0;
        }
    }
    to_fcb(last, pattern);
}

/*
 * Fill in the caller's DTA: our position, and the file we found.
 */
static uint32_t dta_addr(void) {
    return ((uint32_t)guest_peek_lin16(sda_addr + SDA_DTA_OFF + 2) << 4) +
           guest_peek_lin16(sda_addr + SDA_DTA_OFF);
}

static void publish_entry(const uint32_t sda, const FILINFO *fno,
                          const uint16_t next_index) {
    (void)sda;
    const uint32_t dta = dta_addr();
    char fcb[11];
    to_fcb(fno->altname[0] ? fno->altname : fno->fname, fcb);

    // Everything DOS put at the front is left alone except the drive
    // byte, whose top bit marks the block as a redirector's.
    // The letter itself with bit 7, not the drive number: that is what
    // pico-286 writes and what DOS checks for.
    guest_poke_lin(dta + SDB_DRIVE, 0x80u | (uint8_t)REDIR_DRIVE);
    guest_poke_lin16(dta + SDB_INDEX, next_index);

    const uint32_t f = dta + SDB_FOUND;
    for (int i = 0; i < 11; i++) guest_poke_lin(f + i, (uint8_t)fcb[i]);
    guest_poke_lin(f + FOUND_ATTR, (uint8_t)fno->fattrib);
    for (int i = 12; i < 22; i++) guest_poke_lin(f + i, 0);
    guest_poke_lin16(f + FOUND_TIME, fno->ftime);
    guest_poke_lin16(f + FOUND_DATE, fno->fdate);
    guest_poke_lin16(f + FOUND_CLUSTER, 0);
    guest_poke_lin16(f + FOUND_SIZE, (uint16_t)(fno->fsize & 0xFFFFu));
    guest_poke_lin16(f + FOUND_SIZE + 2, (uint16_t)(fno->fsize >> 16));

    printf("[redir] found '%.11s' attr %02X size %lu -> DTA %05lX\n",
           fcb, fno->fattrib, (unsigned long)fno->fsize, (unsigned long)dta);
    (void)dta;
    (void)next_index;
}

/*
 * One step of a directory search, from `index` onwards.
 *
 * Reopening the directory and skipping to the index each time, rather
 * than holding a DIR open between calls: DOS may run any number of
 * searches at once and abandon most of them without telling us, so
 * keeping state here would mean leaking it. Directories are small and the
 * card is fast.
 */
/*
 * Where the current search has got to.
 *
 * DR-DOS does not keep its search data block where the MS-DOS layout says
 * -- SDA+0x19E holds its own pointers, and a find-next reading an index
 * from there gets nonsense and stops after one entry. The template is at
 * SDA+0x9E in both, though, and DOS hands the same one back on every
 * find-next, so the position can be kept here and matched against it.
 *
 * One search at a time. DIR does exactly one, and a second starting
 * simply resets the first, which is better than reading a number out of a
 * structure whose shape is not known.
 */
static char     search_dir[160];
static char     search_pattern[11];
static uint8_t  search_attr;

/*
 * Does this entry belong in a search for these attributes?
 *
 * CX carries the mask, and DIR uses it three times over: once for the
 * volume label alone, then for directories, then for ordinary files.
 * Ignoring it meant the first file on the card was handed back as the
 * volume label, which is what put a line of nonsense at the top of the
 * listing.
 *
 * Read-only and archive never restrict a search; the other three bits
 * each have to be asked for.
 */
static bool attr_wanted(const uint8_t fattrib, const uint8_t search) {
    // No volume label on this drive: it is a directory on a card, not a
    // volume of its own.
    if (search & 0x08u) return false;   // 0x08 = volume label

    if ((fattrib & AM_DIR) && !(search & AM_DIR)) return false;
    if ((fattrib & AM_HID) && !(search & AM_HID)) return false;
    if ((fattrib & AM_SYS) && !(search & AM_SYS)) return false;
    return true;
}

static bool search_step(const uint32_t sda, uint16_t index) {
    char dir[160], pattern[11];
    split_template(sda, dir, sizeof dir, pattern);

    DIR d;
    FS_LOCK();
    FRESULT rc = f_opendir(&d, dir);
    FS_UNLOCK();
    if (rc != FR_OK) return false;

    bool found = false;
    uint16_t seen = 0;
    for (;;) {
        FILINFO fno;
        FS_LOCK();
        rc = f_readdir(&d, &fno);
        FS_UNLOCK();
        if (rc != FR_OK || !fno.fname[0]) break;

        if (!attr_wanted(fno.fattrib, search_attr)) continue;

        char fcb[11];
        to_fcb(fno.altname[0] ? fno.altname : fno.fname, fcb);
        if (!fcb_match(pattern, fcb)) continue;

        if (seen++ < index) continue;

        publish_entry(sda, &fno, index + 1);
        found = true;
        break;
    }

    FS_LOCK();
    f_closedir(&d);
    FS_UNLOCK();
    return found;
}

#define DOS_NO_MORE_FILES 0x12u

static void fn_find_first(void) {
    if (!sda_addr) { decline(); return; }

    split_template(sda_addr, search_dir, sizeof search_dir, search_pattern);
    search_attr = (uint8_t)regs.cx;

    const uint32_t dta = dta_addr();
    for (int i = 0; i < 11; i++)
        guest_poke_lin(dta + SDB_TEMPLATE + i, (uint8_t)search_pattern[i]);
    guest_poke_lin(dta + SDB_ATTR, search_attr);

    if (search_step(sda_addr, 0)) ok();
    else fail(DOS_NO_MORE_FILES);
}

static void fn_find_next(void) {
    if (!sda_addr) { decline(); return; }

    /*
     * Where to carry on, and what to look for, both come back in the
     * block DOS kept for us. Nothing here has to remember which search is
     * which, so any number can be interleaved.
     */
    const uint32_t dta = dta_addr();
    const uint16_t index = guest_peek_lin16(dta + SDB_INDEX);
    search_attr = guest_peek_lin(dta + SDB_ATTR);
    split_template(sda_addr, search_dir, sizeof search_dir, search_pattern);

    if (search_step(sda_addr, index)) ok();
    else fail(DOS_NO_MORE_FILES);
}

void redirector_task(void) {
    if (!pending) return;
    pending = false;

    const uint8_t fn = regs.ax & 0xFFu;
    answer = ANSWER_DECLINE;

    printf("[redir] AX=%04X BX=%04X CX=%04X DX=%04X\n",
           regs.ax, regs.bx, regs.cx, regs.dx);

    switch (fn) {
        case 0x00: fn_install_check(); break;
        case 0x0C: fn_disk_info();     break;
        case 0x1B: fn_find_first();    break;
        case 0x1C: fn_find_next();     break;

        default:
            /*
             * Everything else is declined, deliberately and visibly.
             *
             * A redirector that answers some calls and silently mishandles
             * others corrupts data; one that says "not supported" makes
             * DOS report an error the operator can see. Until each
             * function is written and tested, that is the honest answer.
             */
            // Declined, and said out loud once per function so the gap is
            // visible rather than inferred from DOS's behaviour.
            printf("[redir] AX=%04X declined (not implemented)\n", regs.ax);
            decline();
            break;
    }

    done = true;
}
