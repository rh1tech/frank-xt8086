#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#

.DEFAULT_GOAL := help

.PHONY: help hooks build clean flash swd console shot view

help: ## Show this help
	@grep -hE '^[a-z_-]+:.*?## ' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-10s\033[0m %s\n", $$1, $$2}'

hooks: ## Point git at the repo's hooks (.githooks)
	@git config core.hooksPath .githooks
	@echo "core.hooksPath = .githooks"

build: ## Build the firmware
	@./build.sh

clean: ## Remove the build tree
	@rm -rf app/build app/build-*

flash: ## Flash over USB (needs BOOTSEL)
	@./flash.sh

swd: ## Flash over SWD with the Debug Probe on J6
	@./swd_flash.sh

console: ## Open the UART console
	@./console.sh

shot: ## Capture one frame of the board's video to out/screen.png
	@./capture.sh shot

view: ## Live preview of the board's video
	@./capture.sh view
