#!/usr/bin/env python3
"""
Rewrite every conditional branch as an inverted short branch over a near
jump, so as86 never needs the 80386 long conditional form (0F 8x).

bcc and as86 have no 8086-strict mode: as86's -0 selects 16-bit output, not
an 8086 instruction set, so any branch that lands outside +/-127 bytes is
emitted as 0F 8x -- a 386 instruction. On an 8086 or a V20, 0F is POP CS,
which pops a word into CS and takes the machine somewhere else entirely.

    beq  target        ->     jne  .q<n>
                              jmp  target
                              .q<n>:

The inverted branch now spans only the three-byte jmp, so it always fits
the short form, and the unconditional jmp is 8086 code at any distance.
"""
import re, sys

INVERSE = {
    'beq': 'jne', 'bne': 'je',
    'blo': 'jae', 'bhis': 'jb',
    'blos': 'ja', 'bhi': 'jbe',
    'blt': 'jge', 'bge': 'jl', 'bgt': 'jle', 'ble': 'jg',
    'bltu': 'jae', 'bgeu': 'jb', 'bgtu': 'jbe', 'bleu': 'ja',
    'je': 'jne', 'jne': 'je', 'jz': 'jnz', 'jnz': 'jz',
    'jb': 'jae', 'jae': 'jb', 'jbe': 'ja', 'ja': 'jbe',
    'jl': 'jge', 'jge': 'jl', 'jle': 'jg', 'jg': 'jle',
    'jc': 'jnc', 'jnc': 'jc', 'js': 'jns', 'jns': 'js',
    'jo': 'jno', 'jno': 'jo',
}

BRANCH = re.compile(r'^(' + '|'.join(sorted(INVERSE, key=len, reverse=True)) +
                    r')[ \t]+([^ \t;!]+)[ \t]*(.*)$')

out, n = [], 0
for line in sys.stdin:
    m = BRANCH.match(line.rstrip('\n'))
    if not m:
        out.append(line)
        continue
    mnem, target, rest = m.groups()
    # A numeric displacement is already relative and safe to leave alone.
    if target.lstrip('-+').isdigit():
        out.append(line)
        continue
    lbl = f'.q{n}'
    n += 1
    out.append(f'{INVERSE[mnem]}\t{lbl}\n')
    out.append(f'jmp\t{target}\n')
    out.append(f'{lbl}:\n')
sys.stdout.write(''.join(out))
sys.stderr.write(f'shortbranch: rewrote {n} conditional branches\n')
