/*
 * Stub for murm386's profiling header.
 *
 * vga.c is taken from murm386 unchanged so that fixes there can be
 * brought over by copying the file again. It includes this; nothing here
 * needs the profiler.
 */
#pragma once

// murm386 counts VGA memory accesses here. Nothing does with them in this
// machine, so they cost nothing.
static inline void cp_vga_write(void) {}
static inline void cp_vga_read(void)  {}
