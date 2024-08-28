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
| H | Hard reset (via `gb_reset()`) |
| Page Up | Scroll screen up (boards with 240x96 4 bit screen only) |
| Page Down | Scroll screen down (boards with 240x96 4 bit screen only) |
| 1 | Scroll screen to the top (boards with 240x96 4 bit screen only) |
| 2 | Scroll screen to the center (boards with 240x96 4 bit screen only) |
| 3 | Scroll screen to the bottom (boards with 240x96 4 bit screen only) |
| ESC | Quit emulator |

## Configuration

To configure the emulator, create an ASCII-encoded, Windows line ending INI file named `pgbcfg.ini` under the work directory of the emulator.

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

; Enable perioical SRAM auto-commit (recommended)
;
; Some boards have very slow I/O and auto-commit creates noticeable lag spikes
; on these boards. One can set this to 0 to disable auto-commit. Beware that
; if auto commit is off, the only time that SRAM will be saved to disk is when
; the emulator exits, therefore a forced power off, for example, will cause
; data loss.
SRAMAutoCommit = 1

; Compensate scheduler timer lags caused by holding down a button
;
; Some boards, like the S3C-based ones, produce a somewhat deterministic lag
; on the scheduler timer when some buttons were being held down.
; The value set here will be added to the current frame time (i.e. subtracted
; from the calculated delay time) whenever a button press is currently being
; tracked.
ButtonHoldCompensation = 0

[Debug]
; Show the average number of milliseconds spent on delaying the main loop after
; each frame. Updated every 32 frames.
ShowDelayFactor = 0
```

## Known board-specific quirks

### Absence of millisecond-level RTC

Most boards that I came across seem to lack true millisecond-level RTC (the millis field either don't increment in milliseconds or is a constant 0). In this case, timing issue will be expected if not using the scheduler timer.

### Delays in scheduler timer when holding down a button

Some boards, in particular the S3C-based ones, have a high priority input tracking thread that block for a significant amount of time when some buttons were held down. This could cause the timer to lag when some buttons were held down. The `ButtonHoldCompensation` configuration option can be used to alleviate this problem.

### Writing to LCD framebuffer will not update the LCD

This is seen on W55SA7101-based boards. These boards don't have DMA-backed framebuffer. Calling `_BitBlt` manually on these boards is necessary to actually update the LCD.
