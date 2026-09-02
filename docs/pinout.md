# xt8086 beta — RP2350B pin map

Read off `xt8086_beta.kicad_pcb` (revision 1.01), not inherited from the
prototype. `src/core/xt8086.h` is the machine-readable copy of this table and
`boards/frank_xt8086.h` is the SDK-facing one; if the three ever disagree,
the PCB wins.

## The 8086's bus — GP0-GP29

Fixed by the CPU package (U5, 8086 in minimum mode). Nothing here is a
choice, and nothing else may use these pins.

| GPIO    | Net           | 8086 pin | Direction | Notes |
|---------|---------------|----------|-----------|-------|
| 0-15    | AD0-AD15      | 16-9, 39, 2-8 | bidir | Multiplexed address/data |
| 16-19   | A16-A19       | 38-35    | in        | Top address nibble |
| 20      | ALE           | 25       | in        | Address latch enable |
| 21      | ~RD           | 32       | in        | Read strobe |
| 22      | ~WR           | 29       | in        | Write strobe |
| 23      | ~INTA         | 24       | in        | Interrupt acknowledge |
| 24      | M/~IO         | 28       | in        | 1 = memory, 0 = I/O |
| 25      | ~BHE          | 34       | in        | Bus high enable; also lights LD1 |
| 26      | INTR          | 18       | out       | Interrupt request, active high |
| 27      | READY (D)     | —        | out       | Into U2 pin 2, see below |
| 28      | RESET         | 21       | out       | Active high |
| 29      | CLK           | 19       | out       | PWM, 33 % duty |

READY does not reach the CPU directly. GP27 drives the D input of U2, half
an SN74LS74, clocked from the CLK line inverted through U1 (SN74LS04); the
flip-flop's Q is the 8086's READY on pin 22. That is the synchroniser the
8086 datasheet asks for, and it is why the PIO can drive READY with a
side-set on any cycle boundary without violating setup time.

`src/core/i8086_bus.pio` repeats GP20-GP27 as `.define` constants, because a
PIO program cannot include a C header. The two lists have to agree.

## Everything above GP29

These are the board's own choices, and they are where this board differs
from the WeAct RP2350B module the prototype ran on.

| GPIO  | Function | Part |
|-------|----------|------|
| 30    | Blue, LSB (800R) | R27 → J11.3 via U16 |
| 31    | Blue, MSB (400R) | R28 |
| 32    | Green, LSB (800R) | R29 → J11.2 |
| 33    | Green, MSB (400R) | R30 |
| 34    | Red, LSB (800R) | R31 → J11.1 |
| 35    | Red, MSB (400R) | R32 |
| 36    | HSYNC | J11.13 and U16 HSIN, through R38 |
| 37    | VSYNC | J11.14 and U16 VSIN, through R39 |
| 38    | I2C SDA | U15 DS3231MZ pin 7, 4.7k pull-up |
| 39    | I2C SCL | U15 DS3231MZ pin 8, 4.7k pull-up |
| 40    | SD DAT0 / MISO | J4 microSD, SPI1 |
| 41    | SD CD/DAT3 / ~CS | J4 |
| 42    | SD CLK / SCK | J4 |
| 43    | SD CMD / MOSI | J4 |
| 44    | UART0 TX | J1 pin 1 |
| 45    | UART0 RX | J1 pin 3 |
| 46    | Speaker | JP1 HDMI audio, JP2 jack, JP3 buzzer |
| 47    | PSRAM ~CS | U8 ESP-PSRAM64H, XIP CS1, 10k pull-up |

Note the two traps the prototype's pin map would have walked into here:

- **GP45 was the prototype's "ISA" marker pin**, driven high on every bus
  cycle and low on an unmapped access. On this board it is the console's
  RX line. The marker is now `bus_trace()` in `src/core/xt8086.h`, compiled
  out unless `BUS_TRACE_PIN` names a pin.
- **The video byte is VHRRGGBB, not VHBBGGRR.** Blue is the low pair.
  The driver already packed it that way — `graphics_set_palette()` builds
  `(r << 4) | (g << 2) | b` — so the code is right and only the comment
  above it was wrong. Worth knowing before anyone "fixes" it.

## Colour depth

Two bits per channel, from a two-resistor ladder per channel into 75R.
Sixty-four colours. The CGA palette in `src/core/state.h` is 24-bit and is
reduced by taking the top two bits of each component, which is why bright
white and light grey are distinguishable but the six-bit result is not a
faithful CGA reproduction.

## Video path

The ladder drives J11 (VGA) directly *and* U16, an MS9288A VGA-to-HDMI
converter, which drives J12. Both outputs are live at once; the HDMI one
is what a USB capture stick can see, and `capture.sh` uses that.

Separate H and V sync, so `VGA_CSYNC` is 0. The prototype defaulted it to
1 because its display expected composite sync on green.

## QSPI

Flash (U7, W25Q128JVPIQ, 16 MB) on CS0, PSRAM (U8, ESP-PSRAM64H, 8 MB) on
CS1 selected by GP47. `RAM[]` and `UMB[]` live in the PSRAM through the
`.psram` section in `src/app/memmap.ld`; `VIDEORAM[]` stays in on-chip SRAM
because the scanline DMA reads it every line.

## Debug

J6: pin 1 SWDIO, pin 2 GND, pin 3 SWCLK. A Raspberry Pi Debug Probe on
this header is the intended flashing and debugging path — see the README.
