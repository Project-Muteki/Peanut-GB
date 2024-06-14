# Peanut-GB

A port of Peanut-GB to Besta RTOS.

## Features

- ROM file picker
- Audio via `minigb_apu`
- Optional interlaced and half-rate rendering
- 2 frame timing methods (RTC- and scheduler-based)

## Key binding

| Key | Joypad |
| --- | ------ |
| Up | Up |
| Left | Left |
| Down | Down |
| Right | Right |
| Z | B |
| X | A |
| A | Select |
| S | Start |
| R | A+B+Select+Start (soft reset combo key) |
| M | Toggle sound emulation |
| ESC | Quit emulator |

## Configuration

To configure the emulator, one can create an ASCII-encoded, Windows line ending INI file named `pgbcfg.ini` under the work directory of the emulator.

Supported options are as follows:

```ini
[Config]
; Start the emulator with audio enabled by default.
EnableAudio = 1

; Enable interlaced rendering.
Interlace = 0

; Enable frame skip (half refresh rate) mode.
HalfRefresh = 0

; Use the OS scheduler instead of RTC to time the frames.
;
; This is achieved by spawning a high-priority thread that would increment a
; counter on every scheduler tick. Useful on boards without a millisecond-level
; accurate RTC (e.g. some S3C2416-based boards).
UseSchedulerTimer = 0

[Debug]
; For boards based on Nuvoton W55SA7101, copy the pixels to the main
; framebuffer and write to the LCD afterwards, instead of falling back to safe
; mode. Currently this is specific to a certain version of the CA106 board,
; therefore it's for internal testing only and should not be enabled. In the
; future we may add a more generic version for other boards based on Nuvoton
; W55SA7101 and move this into [Config].
P4SingleCopyBlit = 0

; Show the average number of milliseconds spent on delaying the main loop after
; each frame. Updated every 32 frames.
ShowDelayFactor = 0
```
