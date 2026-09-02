#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# debug.sh — OpenOCD and GDB against the Debug Probe on J6.
#
#   ./debug.sh            build, flash over SWD, break at main, run
#   ./debug.sh openocd    server only, for attaching a GDB by hand
#   ./debug.sh gdb        attach to a server that is already up
#
# The probe is also the console: it enumerates a CDC port alongside
# CMSIS-DAP, and that port is what J1's UART should be wired to. See
# console.sh.
#
set -euo pipefail
cd "$(dirname "$0")"

ELF="${ELF:-build/frank-xt8086.elf}"
CFG="openocd.cfg"

gdb_attach() {
    [[ -f "${ELF}" ]] || { echo "ERROR: ${ELF} not found. Run ./build.sh." >&2; exit 1; }
    arm-none-eabi-gdb "${ELF}" \
        -ex "target extended-remote :3333" \
        -ex "monitor reset halt" \
        -ex "load" \
        -ex "break main" \
        -ex "continue"
}

case "${1:-all}" in
    openocd) exec openocd -f "${CFG}" ;;
    gdb)     gdb_attach ;;
    all)
        ./build.sh
        openocd -f "${CFG}" &
        OPENOCD_PID=$!
        trap 'kill ${OPENOCD_PID} 2>/dev/null || true' EXIT
        # The server needs to be listening before GDB connects; two
        # seconds is comfortably more than it takes to attach to the probe.
        sleep 2
        gdb_attach
        ;;
    *) echo "usage: $0 [all|openocd|gdb]" >&2; exit 2 ;;
esac
