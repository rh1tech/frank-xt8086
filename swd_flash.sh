#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# swd_flash.sh — flash over SWD with a Raspberry Pi Debug Probe.
#
# The preferred path once a probe is attached. SWD does not care what the
# target is doing, so a firmware that has faulted into lockup still takes
# a new image without anyone touching the board.
#
# Wiring: J6 on the board, pin 1 = SWDIO, pin 2 = GND, pin 3 = SWCLK.
#
# Usage: ./swd_flash.sh [image.elf] [--reset-only]
#
set -euo pipefail
cd "$(dirname "$0")"

ELF="${1:-app/build/frank-xt8086.elf}"

OPENOCD_ARGS=(-f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg)

if [[ "${1:-}" == "--reset-only" || "${2:-}" == "--reset-only" ]]; then
    exec openocd "${OPENOCD_ARGS[@]}" -c "init" -c "reset run" -c "exit"
fi

if [[ ! -f "${ELF}" ]]; then
    echo "ERROR: ${ELF} not found. Run ./build.sh first." >&2
    exit 1
fi

echo "Flashing over SWD: ${ELF}"
openocd "${OPENOCD_ARGS[@]}" -c "program ${ELF} verify reset exit"
