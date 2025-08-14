# WoodyBoy

A Game Boy emulator running on Besta RTOS, based on Peanut-GB.

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

To configure the emulator, create an ASCII-encoded, Windows line-ending INI file named `wb.ini` under `C:\APPS\woodyboy` (create one if it does not already exist).

Supported options are as follows:

```ini
[Config]
; Start the emulator with audio enabled by default.
EnableAudio = 1

; Enable interlaced rendering.
Interlace = 0

; Enable frame skip (half refresh rate) mode.
HalfRefresh = 0

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
; Whenever a button press is currently being tracked, the values set here will
; be used as a fraction to derive a compensation value, which is then
; subtracted from the delay factor before the thread goes to sleep for
; DelayFactor milliseconds (scheduler ticks). In other words, the actual delay
; factor used by OSSleep at the end of the emulator frame will be calculated by
; DelayFactor = DelayFactor - FrameTime * Num / Denom.
ButtonHoldCompensationNum = 1
ButtonHoldCompensationDenom = 1

; Defines the behavior of the input poller when multiple keys were held-down.
;
; Currently 2 modes are supported:
; - Mode 0: Direct Input Simulation (DIS). This is a mode that detects whether
;   a key was being held down through rapid key-down events that gets emitted
;   by the OS event producer when the user holds down a key. Maximum of 2 key
;   presses can be detected on boards that populate both key_code0 and
;   key_code1 correctly. This is the default behavior.
; - Mode 1: Use KEY_UP events. This treats a KEY event as a key-down and a
;   KEY_UP event as a key-up. Works on mainly the S3C-based boards that have
;   an external keyboard module (like laptop keyboards).
; - Mode 2: Reserved for HP Prime keypad protocol. Do not use.
MultiPressMode = 0

; Synchronize emulated RTC with the system RTC on emulator resume (i.e. waking
; up from deep sleep and selecting No on quit confirmation dialog). May cause
; issues with some games.
SyncRTCOnResume = 0

; Select 4-bit LCD type.
;
; There are several types of 4-bit LCD panels used by W55SA7101-based grayscale
; boards, each with protocol incompatible with each other. Use this option to
; select which LCD driver is ued by the emulator to draw the screen. When set
; to 0, a fallback line-by-line mode is used which is a bit faster than safe
; mode.
;
; Supported screen variants:
; - Type 1: The LCD used by BA101.
; - Type 2: The LCD used by CA106 (untested as my unit is bricked).
L4LCDType = 0

; Use boot ROM if the file is available under the config directory
; (dmg_boot.bin for DMG mode and cgb_boot.bin for CGB mode [wbc only])
UseBootROM = 1

[Debug]
; Show the average number of milliseconds spent on delaying the main loop after
; each frame. Updated every 32 frames.
ShowDelayFactor = 0

; Use the safe fallback framebuffer setup regardless of availability of faster
; alternatives (slow).
ForceSafeFramebuffer = 0

[KeyBinding]
; Key binding settings in the foramt of <gb-key> = <besta-key-code>. Uncomment
; to override the default bindings, and set to 0 to disable a key.
;A = 88  ; KEY_X
;B = 90  ; KEY_Z
;Select = 65  ; KEY_A
;Start = 83  ; KEY_S
;Right = 4  ; KEY_RIGHT
;Left = 2  ; KEY_LEFT
;Up = 3  ; KEY_UP
;Down = 5  ; KEY_DOWN
;ResetCombo = 82  ; KEY_R

;Quit = 1  ; KEY_ESC
;Mute = 77  ; KEY_M
;ResetHard = 72  ; KEY_H
;ScrollUp = 6  ; KEY_PGUP
;ScrollDown = 7  ; KEY_PGDN
;ScrollTop = 49  ; KEY_1
;ScrollCenter = 50  ; KEY_2
;ScrollBottom = 51  ; KEY_3
;SRAMCommit = 150  ; KEY_SAVE
```

## Known board-specific quirks

### Absence of millisecond-level RTC

Most boards that I came across seem to lack true millisecond-level RTC (the millis field either don't increment in milliseconds or is a constant 0). In this case, timing issue will be expected if not using the scheduler timer.

### Delays in scheduler timer when holding down a button

Some boards, in particular the S3C-based ones, have a high priority input tracking thread that block for a significant amount of time when some buttons were held down. This could cause the timer to lag when some buttons were held down. The `ButtonHoldCompensation` configuration option can be used to alleviate this problem.

### Writing to LCD framebuffer will not update the LCD

This is seen on W55SA7101-based boards. These boards don't have DMA-backed framebuffer. Calling `_BitBlt` manually on these boards is necessary to actually update the LCD.

### Board-specific multi-press behavior

There doesn't seem to be a standard way of handling the input events, specifically when it comes to handling multiple simultaneous key presses. i.MX233 and W55SA7101 boards use the `key_code0` and `key_code1` fields and therefore are limited to only 2 simultaneous key presses. There's also no release event so one needs to track the repeat press events to simulate the key-down and key-up event. S3C24xx-based boards (except HP Prime) adds release events, but does not populate `key_code1`, and HP Prime uses an extended event format that lays out up to 8 simultaneous presses.
