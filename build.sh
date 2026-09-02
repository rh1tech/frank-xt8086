#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build.sh — configure and build.
#
# Every knob is an environment variable with a default, so the common case
# is `./build.sh` and the uncommon case does not need a second script:
#
#   CPU_SPEED=252 ./build.sh          # the conservative clock
#   SERIAL_CONSOLE=ON ./build.sh      # screen mirror over UART0
#   CLEAN=1 ./build.sh                # from scratch
#
set -euo pipefail
cd "$(dirname "$0")"
source ./sdk_env.sh

# A one-line nudge, not a gate. The commit hooks only exist for a clone
# that has run `make hooks`, and the usual way to find that out is to have
# a push rejected later. This disappears as soon as they are installed.
if [ "$(git config core.hooksPath 2>/dev/null || true)" != ".githooks" ]; then
    echo "note: git hooks are not installed in this clone -- run 'make hooks'"
fi

BUILD_DIR="${BUILD_DIR:-app/build}"
BOARD="${BOARD:-frank_xt8086}"
CPU_SPEED="${CPU_SPEED:-504}"
PSRAM_SPEED="${PSRAM_SPEED:-166}"
SERIAL_CONSOLE="${SERIAL_CONSOLE:-OFF}"
BEEPER_SWEEP="${BEEPER_SWEEP:-OFF}"
CLEAN="${CLEAN:-0}"

[[ "${CLEAN}" == "1" ]] && rm -rf "${BUILD_DIR}"

echo "=== frank-xt8086 ==="
echo "  board ${BOARD}   CPU ${CPU_SPEED} MHz   PSRAM ${PSRAM_SPEED} MHz"
echo "  serial console ${SERIAL_CONSOLE}   beeper sweep ${BEEPER_SWEEP}"
echo

cmake -S app -B "${BUILD_DIR}" \
      -DPICO_BOARD="${BOARD}" \
      -DCPU_SPEED="${CPU_SPEED}" \
      -DPSRAM_SPEED="${PSRAM_SPEED}" \
      -DSERIAL_CONSOLE="${SERIAL_CONSOLE}" \
      -DBEEPER_SWEEP="${BEEPER_SWEEP}" \
      ${BUS_TRACE_PIN:+-DBUS_TRACE_PIN="${BUS_TRACE_PIN}"}

cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "Built: ${BUILD_DIR}/frank-xt8086.uf2"
