/*
 * Stub for murm386's hardware-VGA header.
 *
 * vga.c includes it on the RP2350 path, which is also the path that
 * brings in pico/stdlib.h for __not_in_flash_func. It needs nothing from
 * it here: this machine drives its own scanline generator and reads the
 * card through the accessors in vga.h.
 */
#pragma once
