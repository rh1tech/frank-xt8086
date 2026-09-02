#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#

.DEFAULT_GOAL := help

.PHONY: help hooks check-attribution build clean flash swd console shot view

help: ## Show this help
	@grep -hE '^[a-z_-]+:.*?## ' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-10s\033[0m %s\n", $$1, $$2}'

hooks: ## Install the repo's git hooks (do this once per clone)
	@git config core.hooksPath .githooks
	@chmod +x .githooks/* tools/check-attribution.sh
	@echo "core.hooksPath = .githooks"
	@./tools/check-attribution.sh --self-test

check-attribution: ## Scan this branch's commits for AI attribution
	@./tools/check-attribution.sh --self-test
	@./tools/check-attribution.sh --range HEAD --not --remotes

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
