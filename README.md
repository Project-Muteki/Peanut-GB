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
; Show the average number of milliseconds spent on delaying the main loop after
; each frame. Updated every 32 frames.
ShowDelayFactor = 0
```
