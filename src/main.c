#include <inttypes.h>
#include <muteki/ui/common.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include <muteki/audio.h>
#include <muteki/datetime.h>
#include <muteki/devio.h>
#include <muteki/ini.h>
#include <muteki/threading.h>
#include <muteki/utf16.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/font.h>
#include <muteki/ui/event.h>
#include <muteki/ui/surface.h>
#include <muteki/ui/views/filepicker.h>
#include <muteki/ui/views/messagebox.h>

#include <mutekix/threading.h>
#include <mutekix/time.h>

#define ENABLE_SOUND 1

/* Compat with old Peanut-GB. */
#ifndef JOYPAD_A
#define JOYPAD_A            0x01
#define JOYPAD_B            0x02
#define JOYPAD_SELECT       0x04
#define JOYPAD_START        0x08
#define JOYPAD_RIGHT        0x10
#define JOYPAD_LEFT         0x20
#define JOYPAD_UP           0x40
#define JOYPAD_DOWN         0x80
#endif

#include "minigb_apu.h"

#ifndef LEGACY_APU
struct minigb_apu_ctx g_apu_ctx;

static void audio_init(void) {
  minigb_apu_audio_init(&g_apu_ctx);
}

static void audio_callback_wrapper(audio_sample_t *samples) {
  minigb_apu_audio_callback(&g_apu_ctx, samples);
}

static uint8_t audio_read(const uint16_t addr) {
  return minigb_apu_audio_read(&g_apu_ctx, addr);
}

static void audio_write(const uint16_t addr, const uint8_t val) {
  minigb_apu_audio_write(&g_apu_ctx, addr, val);
}
#else
#ifndef AUDIO_SAMPLES_TOTAL
#define AUDIO_SAMPLES_TOTAL AUDIO_SAMPLES * 2
#endif
typedef int16_t audio_sample_t;
static void audio_callback_wrapper(audio_sample_t *samples) {
  audio_callback(NULL, (uint8_t *) samples, AUDIO_SAMPLES_TOTAL * 2);
}
#endif  // LEGACY_APU

#include "peanut_gb.h"

#ifdef LEGACY_DETECT_SAVE
// Compatibility function with older Peanut-GB that rejects obviously bad returns.
static int gb_get_save_size_s(struct gb_s *gb, size_t *ram_size) {
  uint_fast32_t ram_size_unsigned = gb_get_save_size(gb);
  if (ram_size_unsigned > 0x20000u || ram_size_unsigned % 512 != 0) {
    return -1;
  }
  *ram_size = ram_size_unsigned;
  return 0;
}
#endif

typedef enum {
  MULTI_PRESS_MODE_DIS = 0,
  MULTI_PRESS_MODE_NATIVE_S3C,
} multi_press_mode_t;

enum emu_key_e {
  EMU_KEY_QUIT = 1,
  EMU_KEY_MUTE = 1 << 1,
  EMU_KEY_RESET = 1 << 2,
  EMU_KEY_SCROLL_UP = 1 << 3,
  EMU_KEY_SCROLL_DOWN = 1 << 4,
  EMU_KEY_SCROLL_TOP = 1 << 5,
  EMU_KEY_SCROLL_CENTER = 1 << 6,
  EMU_KEY_SCROLL_BOTTOM = 1 << 7,
  EMU_KEY_SRAM_COMMIT = 1 << 8,
};

struct key_binding_s {
  unsigned short a;
  unsigned short b;
  unsigned short select;
  unsigned short start;
  unsigned short right;
  unsigned short left;
  unsigned short up;
  unsigned short down;
  unsigned short reset_combo;

  unsigned short quit;
  unsigned short mute;
  unsigned short reset_hard;
  unsigned short scroll_up;
  unsigned short scroll_down;
  unsigned short scroll_top;
  unsigned short scroll_center;
  unsigned short scroll_bottom;
  unsigned short sram_commit;
};

static int _audio_worker(void *user_data);
static int _input_dis_worker(void *user_data);
static int _input_s3c_worker(void *user_data);
static unsigned int _map_emu_key_state(unsigned short key);
static uint8_t _map_pad_state(unsigned short key);
static void exit_cleanup(const struct gb_s * const gb);
static int messagebox_format(unsigned short type, const char *fmt, ...);

static mutekix_thread_arg_t audio_worker_arg = {
  .func = &_audio_worker,
  .user_data = NULL,
};

static mutekix_thread_arg_t input_dis_worker_arg = {
  .func = &_input_dis_worker,
  .user_data = NULL,
};

static mutekix_thread_arg_t input_s3c_worker_arg = {
  .func = &_input_s3c_worker,
  .user_data = NULL,
};

const int PALETTE_P4[16] = {
  0x000000, 0x111111, 0x222222, 0x333333,
  0x444444, 0x555555, 0x666666, 0x777777,
  0x888888, 0x999999, 0xaaaaaa, 0xbbbbbb,
  0xcccccc, 0xdddddd, 0xeeeeee, 0xffffff,
};

const uint8_t COLOR_MAP[4] = {
  0xff, 0xaa, 0x55, 0x00
};

const uint32_t COLOR_MAP_32[4] = {
  0xffffffff, 0xffaaaaaa, 0xff555555, 0xff000000
};

#if PEANUT_FULL_GBC_SUPPORT
const uint8_t COLOR_MAP_CGB[32] = {
  0x00, 0x08, 0x10, 0x19, 0x21, 0x29, 0x31, 0x3a,
  0x42, 0x4a, 0x52, 0x5a, 0x63, 0x6b, 0x73, 0x7b,
  0x84, 0x8c, 0x94, 0x9c, 0xa5, 0xad, 0xb5, 0xbd,
  0xc5, 0xce, 0xd6, 0xde, 0xe6, 0xef, 0xf7, 0xff
};
#endif

const char SAVE_FILE_SUFFIX[] = ".sav";

const char CONFIG_PATH[] = "C:\\SYSTEM\\muteki\\pgbcfg.ini";

const key_press_event_config_t KEY_EVENT_CONFIG_DRAIN = {65535, 65535, 1};
const key_press_event_config_t KEY_EVENT_CONFIG_SUPPRESS = {65535, 65535, 0};
const key_press_event_config_t KEY_EVENT_CONFIG_TURBO = {0, 0, 0};

volatile unsigned int emu_key_state = 0, pad_key_state = 0;
volatile bool holding_any_key = false;
volatile bool power_event = false;
volatile uint8_t audio_buffer_consumer_offset;
volatile uint8_t audio_buffer_producer_offset;
volatile bool audio_running = false;
volatile bool tim1_emulator_running = false;
volatile unsigned short sched_timer_ticks = 0;
audio_sample_t *audio_buffer = NULL;
thread_t *audio_worker_inst = NULL;
thread_t *input_worker_inst = NULL;
thread_t *sched_timer_worker_inst = NULL;
event_t *audio_shutdown_ack = NULL;
event_t *input_poller_shutdown_ack = NULL;

static struct key_binding_s g_key_binding = {0};

struct priv_config_s {
  short button_hold_compensation_num;
  short button_hold_compensation_denom;
  multi_press_mode_t multi_press_mode;
  bool enable_audio;
  bool interlace;
  bool half_refresh;
  bool sram_auto_commit;
  bool sync_rtc_on_resume;
  bool debug_show_delay_factor;
  bool debug_force_safe_framebuffer;
};

struct priv_s {
  /* Pointer to allocated memory holding GB file. */
  uint8_t *rom;
  /* Pointer to allocated memory holding save file. */
  uint8_t *cart_ram;
  size_t cart_ram_size;

  /* Framebuffer objects. */
  lcd_surface_t *fb;
  lcd_surface_t *real_fb;

  /* Blit offset and limit. In fallback blit mode, these are passed to _BitBlt. In fast mode, these are used
     directly by the fast blit routine. */
  int rotation;
  unsigned short canvas_x;
  unsigned short canvas_y;
  unsigned short yskip;
  unsigned short width;
  unsigned short height;
  /* This needs to be the longer side of the width and height for it to support rotation. */
  size_t surface_yoff[LCD_WIDTH];

  /* Backup of key press event parameters. */
  key_press_event_config_t old_hold_cfg;

  /* Direct Input Simulation (DIS) and sound emulation state. */
  bool dis_active;
  bool sound_on;

  /* Use fallback blit algorithm. */
  bool fallback_blit;
  bool p4_1line_buffer;

  /* Filenames for future reference. */
  char save_file_name[FILEPICKER_CONTEXT_OUTPUT_MAX_LFN * 3 + sizeof(SAVE_FILE_SUFFIX)];
  char rom_file_name[FILEPICKER_CONTEXT_OUTPUT_MAX_LFN * 3];

  struct priv_config_s config;

};

static inline bool _test_events_no_shift(ui_event_t *uievent) {
    // Deactivate shift key because it may cause the keycode to change.
    // This means we need to handle shift behavior ourselves (if we really need it) but that's a fair tradeoff.
    SetShiftState(TOGGLE_KEY_INACTIVE);
    return TestPendEvent(uievent) || TestKeyEvent(uievent);
}

static inline void _ext_ticker_s3c(void) {
  static ui_event_t uievent = {0};
  unsigned int emu_key_state_local = emu_key_state, pad_key_state_local = pad_key_state;

  /* TODO: what about single-shot events? Do we still need to request (lower rate) event
     spamming so we know whether a single-shot was held-down? */
  while ((TestPendEvent(&uievent) || TestKeyEvent(&uievent)) && GetEvent(&uievent)) {
    if (uievent.event_type == UI_EVENT_TYPE_KEY) {
      if (uievent.key_code0 == KEY_POWER) {
        power_event = true;
      } else {
        pad_key_state_local |= _map_pad_state(uievent.key_code0);
        emu_key_state_local |= _map_emu_key_state(uievent.key_code0);
      }
    } else if (uievent.event_type == UI_EVENT_TYPE_KEY_UP) {
      pad_key_state_local &= ~_map_pad_state(uievent.key_code0);
      emu_key_state_local &= ~_map_emu_key_state(uievent.key_code0);
    }
  }

  /* BUG: This will cause the scheduler timer compensation to be skipped when an unsupported key was held-down.
     There's nothing we can do about this at the moment since we can't tell apart which events were
     single-shot or not. */
  holding_any_key = pad_key_state_local || emu_key_state_local;
  pad_key_state = pad_key_state_local;
  emu_key_state = emu_key_state_local;
}

static inline void _ext_ticker_dis(void) {
  static ui_event_t uievent = {0};
  static uint_fast16_t down_counter = 0;
  static short pressing0 = 0, pressing1 = 0;
  bool hit = false;

  /* TODO this still seem to lose track presses on BA110. Find out why. */
  if (down_counter <= 3) {
    while (_test_events_no_shift(&uievent)) {
      hit = true;
      if (GetEvent(&uievent) && uievent.event_type == UI_EVENT_TYPE_KEY) {
        if (uievent.key_code0 == KEY_POWER || uievent.key_code1 == KEY_POWER) {
          hit = false;
          down_counter = 1;
          power_event = true;
        } else {
          pressing0 = uievent.key_code0;
          pressing1 = uievent.key_code1;
          down_counter = 7;
        }
      } else {
        ClearEvent(&uievent);
      }
    }
  }

  if (!hit) {
    if (down_counter == 1) {
      pressing0 = 0;
      pressing1 = 0;
      down_counter = 0;
    }
    if (down_counter != 0) {
      down_counter--;
    }
  }

  holding_any_key = pressing0 || pressing1;
  pad_key_state = _map_pad_state(pressing0) | _map_pad_state(pressing1);
  emu_key_state = _map_emu_key_state(pressing0) | _map_emu_key_state(pressing1);
}

static int _audio_worker(void *user_data) {
  (void) user_data;

  OSResetEvent(audio_shutdown_ack);

  audio_running = true;

  size_t actual_size;
  pcm_codec_context_t *pcmdesc = NULL;
  devio_descriptor_t *pcmdev = DEVIO_DESC_INVALID;

  pcmdesc = OpenPCMCodec(DIRECTION_OUT, AUDIO_SAMPLE_RATE, FORMAT_PCM_STEREO);
  if (pcmdesc == NULL) {
    audio_running = false;
    return 0;
  }

  pcmdev = CreateFile("\\\\?\\PCM", 0, 0, NULL, 3, 0, NULL);
  if (pcmdev == NULL || pcmdev == DEVIO_DESC_INVALID) {
    ClosePCMCodec(pcmdesc);
    audio_running = false;
    return 0;
  }

  while (audio_running) {
    uint8_t cbuf = audio_buffer_consumer_offset;
    if (cbuf != audio_buffer_producer_offset) {
      WriteFile(pcmdev, &audio_buffer[cbuf * AUDIO_SAMPLES_TOTAL], AUDIO_SAMPLES_TOTAL * 2, &actual_size, NULL);
      cbuf++;
      cbuf &= 3;
      audio_buffer_consumer_offset = cbuf;
    } else {
      /* Yield from thread for more audio data. */
      OSSleep(1);
    }
  }

  if (pcmdev != NULL && pcmdev != DEVIO_DESC_INVALID) {
    CloseHandle(pcmdev);
    pcmdev = DEVIO_DESC_INVALID;
  }
  if (pcmdesc != NULL) {
    ClosePCMCodec(pcmdesc);
    pcmdesc = NULL;
  }

  OSSetEvent(audio_shutdown_ack);

  return 0;
}

static int _input_dis_worker(void *user_data) {
  (void) user_data;

  OSResetEvent(input_poller_shutdown_ack);

  tim1_emulator_running = true;

  while (tim1_emulator_running) {
    _ext_ticker_dis();
    OSSleep(5);
  }
  OSSetEvent(input_poller_shutdown_ack);

  return 0;
}

static int _input_s3c_worker(void *user_data) {
  (void) user_data;

  OSResetEvent(input_poller_shutdown_ack);

  tim1_emulator_running = true;

  while (tim1_emulator_running) {
    _ext_ticker_s3c();
    OSSleep(15);
  }
  OSSetEvent(input_poller_shutdown_ack);

  return 0;
}

static inline void _drain_all_events(void) {
  ui_event_t uievent = {0};
  size_t silence_count = 0;
  while (silence_count < 60) {
    bool test = (TestPendEvent(&uievent) || TestKeyEvent(&uievent));
    if (test) {
      ClearAllEvents();
      silence_count = 0;
    }
    OSSleep(1);
    silence_count++;
  }
}

static void _input_poller_begin(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  if (!priv->dis_active) {
    emu_key_state = 0;
    pad_key_state = 0;

    if (priv->config.multi_press_mode == MULTI_PRESS_MODE_DIS) {
      GetSysKeyState(&priv->old_hold_cfg);

      input_poller_shutdown_ack = OSCreateEvent(true, 1);
      input_worker_inst = OSCreateThread(&mutekix_thread_wrapper, &input_dis_worker_arg, 16384, false);

      SetSysKeyState(&KEY_EVENT_CONFIG_TURBO);
      OSSleep(1);
      priv->dis_active = true;
    } else if (priv->config.multi_press_mode == MULTI_PRESS_MODE_NATIVE_S3C) {
      GetSysKeyState(&priv->old_hold_cfg);

      input_poller_shutdown_ack = OSCreateEvent(true, 1);
      input_worker_inst = OSCreateThread(&mutekix_thread_wrapper, &input_s3c_worker_arg, 16384, false);

      SetSysKeyState(&KEY_EVENT_CONFIG_SUPPRESS);
      OSSleep(1);
      priv->dis_active = true;
    }
  }
}

static void _input_poller_end(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  if (priv->dis_active) {
    /* TODO do we need to drain the input in S3C mode? */
    SetSysKeyState(&KEY_EVENT_CONFIG_DRAIN);

    tim1_emulator_running = false;
    while (OSWaitForEvent(input_poller_shutdown_ack, 1000) != WAIT_RESULT_RESOLVED) {};
    OSCloseEvent(input_poller_shutdown_ack);
    OSSleep(1);
    if (input_worker_inst != NULL) {
      OSTerminateThread(input_worker_inst, 0);
      input_worker_inst = NULL;
    }

    _drain_all_events();

    SetSysKeyState(&priv->old_hold_cfg);
    emu_key_state = 0;
    pad_key_state = 0;
    priv->dis_active = false;
  }
}

static void _sound_on(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;

  if (!priv->sound_on) {
    audio_buffer_consumer_offset = 0;
    audio_buffer_producer_offset = 0;
    if (audio_buffer != NULL) {
      free(audio_buffer);
      audio_buffer = NULL;
    }
    audio_buffer = calloc(sizeof(*audio_buffer) * 4, AUDIO_SAMPLES_TOTAL);
    if (audio_buffer == NULL) {
      return;
    }
    audio_shutdown_ack = OSCreateEvent(true, 1);
    audio_worker_inst = OSCreateThread(&mutekix_thread_wrapper, &audio_worker_arg, 16384, false);
    OSSleep(1);
    priv->sound_on = true;
  }
}

static void _sound_off(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;

  if (priv->sound_on) {
    audio_running = false;
    while (OSWaitForEvent(audio_shutdown_ack, 1000) != WAIT_RESULT_RESOLVED) {};
    OSCloseEvent(audio_shutdown_ack);
    OSSleep(1);
    if (audio_worker_inst != NULL) {
      OSTerminateThread(audio_worker_inst, 0);
      audio_worker_inst = NULL;
    }
    if (audio_buffer != NULL) {
      free(audio_buffer);
      audio_buffer = NULL;
    }
    priv->sound_on = false;
  }
}

static void _set_blit_parameter(struct gb_s *gb, const lcd_surface_t * const surface) {
  struct priv_s *priv = gb->direct.priv;

  const bool rotate = (!priv->fallback_blit) && (
    priv->rotation == ROTATION_TOP_SIDE_FACING_LEFT ||
    priv->rotation == ROTATION_TOP_SIDE_FACING_RIGHT
  );

  if (surface == NULL) {
    priv->canvas_x = 0;
    priv->canvas_y = 0;
    priv->width = LCD_WIDTH;
    priv->height = LCD_HEIGHT;
    return;
  }

  short ww, hh;
  if (!rotate) {
    ww = surface->width;
    hh = surface->height;
  } else {
    ww = surface->height;
    hh = surface->width;
  }

  int xoff = (ww - LCD_WIDTH) / 2, yoff = (hh - LCD_HEIGHT) / 2;
  if (xoff <= 0) {
    priv->canvas_x = 0;
    priv->width = ww;
  } else {
    priv->canvas_x = xoff;
    priv->width = LCD_WIDTH;
  }
  if (yoff <= 0) {
    priv->canvas_y = 0;
    priv->height = hh;
  } else {
    priv->canvas_y = yoff;
    priv->height = LCD_HEIGHT;
  }
}

static void _precompute_yoff(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  bool use_intermediate_fb = priv->fallback_blit;

  unsigned short x = use_intermediate_fb ? 0 : priv->canvas_x;
  unsigned short y = use_intermediate_fb ? 0 : priv->canvas_y;

  if (
      (!use_intermediate_fb) &&
      (priv->rotation == ROTATION_TOP_SIDE_FACING_LEFT || priv->rotation == ROTATION_TOP_SIDE_FACING_RIGHT)
  ) {
    size_t surface_xoff = y;

    switch (priv->fb->depth) {
    case LCD_SURFACE_PIXFMT_L4:
      surface_xoff = y / 2;
      break;
    case LCD_SURFACE_PIXFMT_XRGB:
      surface_xoff = y * 4;
      break;
    }
    for (size_t i = 0; i < LCD_WIDTH; i++) {
      priv->surface_yoff[i] = (x + i) * priv->fb->xsize + surface_xoff;
      if (priv->fb->depth == LCD_SURFACE_PIXFMT_XRGB) {
        priv->surface_yoff[i] /= 4;
      }
    }
  } else {
    size_t surface_xoff = x;

    switch (priv->fb->depth) {
    case LCD_SURFACE_PIXFMT_L4:
      surface_xoff = x / 2;
      break;
    case LCD_SURFACE_PIXFMT_XRGB:
      surface_xoff = x * 4;
      break;
    }

    for (size_t i = 0; i < LCD_HEIGHT; i++) {
      priv->surface_yoff[i] = (y + i) * priv->fb->xsize + surface_xoff;
      if (priv->fb->depth == LCD_SURFACE_PIXFMT_XRGB) {
        priv->surface_yoff[i] /= 4;
      }
    }
  }
}

static uint8_t *_read_file(const char *path, size_t size, bool allocate_anyway) {
  struct stat st = {0};

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    if (allocate_anyway && size > 0) {
      return (uint8_t *) calloc(size, sizeof(uint8_t));
    }
    return NULL;
  }

  if (size == 0) {
    int fstat_ret = fstat(fileno(f), &st);
    if (fstat_ret < 0) {
      fclose(f);
      return NULL;
    }
    size = (size_t) st.st_size;
  }

  uint8_t *content = (uint8_t *) malloc(size);
  if (content == NULL) {
    fclose(f);
    return NULL;
  }
  fread(content, 1, size, f);
  fclose(f);

  return content;
}

static void _write_save(struct gb_s *gb, const char *path) {
  struct priv_s *priv = gb->direct.priv;
  size_t save_size = priv->cart_ram_size;
  if (priv->cart_ram == NULL || save_size == 0) {
    return;
  }
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    return;
  }
  fwrite(priv->cart_ram, 1, save_size, f);
  fclose(f);
}

#define _TEST_KEY(map, state, as) \
  if (map != 0 && key == map) { \
    state |= as; \
  }

static unsigned int _map_emu_key_state(unsigned short key) {
  unsigned int state = 0;

  _TEST_KEY(g_key_binding.quit, state, EMU_KEY_QUIT)
  _TEST_KEY(g_key_binding.mute, state, EMU_KEY_MUTE)
  _TEST_KEY(g_key_binding.reset_hard, state, EMU_KEY_RESET)
  _TEST_KEY(g_key_binding.scroll_up, state, EMU_KEY_SCROLL_UP)
  _TEST_KEY(g_key_binding.scroll_down, state, EMU_KEY_SCROLL_DOWN)
  _TEST_KEY(g_key_binding.scroll_top, state, EMU_KEY_SCROLL_TOP)
  _TEST_KEY(g_key_binding.scroll_center, state, EMU_KEY_SCROLL_CENTER)
  _TEST_KEY(g_key_binding.scroll_bottom, state, EMU_KEY_SCROLL_BOTTOM)
  _TEST_KEY(g_key_binding.sram_commit, state, EMU_KEY_SRAM_COMMIT)

  return state;
}

static uint8_t _map_pad_state(unsigned short key) {
  uint8_t state = 0;

  /* If reset button is pressed, press down A B Select Start and short circuit. */
  if (g_key_binding.reset_combo != 0 && key == g_key_binding.reset_combo) {
    state = JOYPAD_A | JOYPAD_B | JOYPAD_SELECT | JOYPAD_START;
    return state;
  }

  _TEST_KEY(g_key_binding.a, state, JOYPAD_A)
  _TEST_KEY(g_key_binding.b, state, JOYPAD_B)
  _TEST_KEY(g_key_binding.select, state, JOYPAD_SELECT)
  _TEST_KEY(g_key_binding.start, state, JOYPAD_START)
  _TEST_KEY(g_key_binding.up, state, JOYPAD_UP)
  _TEST_KEY(g_key_binding.down, state, JOYPAD_DOWN)
  _TEST_KEY(g_key_binding.left, state, JOYPAD_LEFT)
  _TEST_KEY(g_key_binding.right, state, JOYPAD_RIGHT)

  return state;
}

void lcd_draw_line_safe(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
  const struct priv_s * const priv = gb->direct.priv;
  lcd_surface_t *fb = priv->fb;

  for (size_t x = 0; x < LCD_WIDTH; x += 2) {
    ((uint8_t *) fb->buffer)[priv->surface_yoff[line] + x / 2] = (
      ((COLOR_MAP[pixels[x] & 3] & 0xf) << 4) |
      (COLOR_MAP[pixels[x + 1] & 3] & 0xf)
    );
  }
}

void lcd_draw_line_fast_p4(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
  const struct priv_s * const priv = gb->direct.priv;
  lcd_surface_t *fb = priv->fb;

  if (line >= priv->height + priv->yskip || line < priv->yskip) {
    return;
  }

  ((uint8_t *) fb->buffer)[0] = 0xff;

  for (size_t x = 0; x < LCD_WIDTH; x += 2) {
    if (x >= priv->width) {
      break;
    }
    /* TODO: handle misaligned pixels (i.e. when priv->x is odd). */
#if PEANUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
      uint16_t pixel = gb->cgb.fixPalette[pixels[x]];
      uint16_t pixel2 = gb->cgb.fixPalette[pixels[x + 1]];
      uint8_t y = (COLOR_MAP_CGB[pixel & 0x001f] * 18 + COLOR_MAP_CGB[(pixel & 0x03e0) >> 5] * 183 + COLOR_MAP_CGB[(pixel & 0x7c00) >> 10] * 54) >> 12;
      uint8_t y2 = (COLOR_MAP_CGB[pixel2 & 0x001f] * 18 + COLOR_MAP_CGB[(pixel2 & 0x03e0) >> 5] * 183 + COLOR_MAP_CGB[(pixel2 & 0x7c00) >> 10] * 54) >> 12;
      ((uint8_t *) fb->buffer)[x / 2] = (y << 4) | (y2 & 0xf);
    } else {
#endif
      ((uint8_t *) fb->buffer)[x / 2] = (
        ((COLOR_MAP[pixels[x] & 3] & 0xf) << 4) |
        (COLOR_MAP[pixels[x + 1] & 3] & 0xf)
      );
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif
  }

  _BitBlt(priv->real_fb, priv->canvas_x & 0xfffe, priv->canvas_y + line - priv->yskip, LCD_WIDTH, 1, priv->fb, 0, 0, BLIT_NONE);
}

void lcd_draw_line_fast_xrgb(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
  const struct priv_s * const priv = gb->direct.priv;
  lcd_surface_t *fb = priv->fb;

  if (line >= priv->height) {
    return;
  }

  for (size_t x = 0; x < LCD_WIDTH; x++) {
    if (x >= priv->width) {
      break;
    }

    size_t pixel_offset = priv->surface_yoff[line] + x;

#if PEANUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
      pixel_offset *= 4;
      uint16_t pixel = gb->cgb.fixPalette[pixels[x]];
      ((uint8_t *) fb->buffer)[pixel_offset + 0] = COLOR_MAP_CGB[pixel & 0x001f];
      ((uint8_t *) fb->buffer)[pixel_offset + 1] = COLOR_MAP_CGB[(pixel & 0x03e0) >> 5];
      ((uint8_t *) fb->buffer)[pixel_offset + 2] = COLOR_MAP_CGB[(pixel & 0x7c00) >> 10];
      ((uint8_t *) fb->buffer)[pixel_offset + 3] = 0xff;
    } else {
#endif
      /* TODO palette */
      ((uint32_t *) fb->buffer)[pixel_offset] = COLOR_MAP_32[pixels[x] & 3];
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif
  }
}

void lcd_draw_line_fast_xrgb_rot(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
  const struct priv_s * const priv = gb->direct.priv;
  lcd_surface_t *fb = priv->fb;

  if (line >= priv->height) {
    return;
  }

  for (size_t x = 0; x < LCD_WIDTH; x++) {
    if (x >= priv->width) {
      break;
    }

    size_t pixel_offset;
    switch (priv->rotation) {
      case ROTATION_TOP_SIDE_FACING_UP:
      default:
        pixel_offset = priv->surface_yoff[line] + x;
        break;
      case ROTATION_TOP_SIDE_FACING_LEFT:
        pixel_offset = priv->surface_yoff[LCD_WIDTH - 1 - x] + line;
        break;
      case ROTATION_TOP_SIDE_FACING_DOWN:
        pixel_offset = priv->surface_yoff[LCD_HEIGHT - 1 - line] + (LCD_WIDTH - 1 - x);
        break;
      case ROTATION_TOP_SIDE_FACING_RIGHT:
        pixel_offset = priv->surface_yoff[x] + (LCD_HEIGHT - 1 - line);
        break;
    }
    

#if PEANUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
      pixel_offset *= 4;
      uint16_t pixel = gb->cgb.fixPalette[pixels[x]];
      ((uint8_t *) fb->buffer)[pixel_offset + 0] = COLOR_MAP_CGB[pixel & 0x001f];
      ((uint8_t *) fb->buffer)[pixel_offset + 1] = COLOR_MAP_CGB[(pixel & 0x03e0) >> 5];
      ((uint8_t *) fb->buffer)[pixel_offset + 2] = COLOR_MAP_CGB[(pixel & 0x7c00) >> 10];
      ((uint8_t *) fb->buffer)[pixel_offset + 3] = 0xff;
    } else {
#endif
      /* TODO palette */
      ((uint32_t *) fb->buffer)[pixel_offset] = COLOR_MAP_32[pixels[x] & 3];
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif
  }
}

uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
  const struct priv_s * const priv = gb->direct.priv;
  return priv->rom[addr];
}

uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
  const struct priv_s * const priv = gb->direct.priv;
  return priv->cart_ram[addr];
}

void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
  const struct priv_s * const priv = gb->direct.priv;
  priv->cart_ram[addr] = val;
}

void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr) {
  const char *gb_err_str[GB_INVALID_MAX] = {
    "UNKNOWN",
    "INVALID OPCODE",
    "INVALID READ",
    "INVALID WRITE",
    "HALT FOREVER"
  };
  char location[64] = "";
  uint8_t instr_byte;

  _input_poller_end(gb);

  /* Record save file. */
  _write_save(gb, "recovery.sav");

  /* Stop workers (if they are running). */
  mutekix_time_fini();
  _sound_off(gb);

  if(addr >= 0x4000 && addr < 0x8000)
  {
    uint32_t rom_addr;
    rom_addr = (uint32_t)addr * (uint32_t)gb->selected_rom_bank;
    sniprintf(location, sizeof(location),
      " (bank %d mode %d, file offset %" PRIu32 ")",
      gb->selected_rom_bank, gb->cart_mode_select, rom_addr);
  }

  instr_byte = __gb_read(gb, addr);

  messagebox_format(MB_ICON_ERROR | MB_BUTTON_OK,
    "Error: %s at 0x%04X%s with instruction %02X.\n"
    "Cart RAM saved to recovery.sav\n"
    "Exiting.\n",
    gb_err_str[gb_err], addr, location, instr_byte);

  /* Free memory and then exit. */
  exit_cleanup(gb);
  exit(1);
}

static void _set_rtc(struct gb_s *gb) {
  time_t rawtime;
  struct tm timeinfo;

  time(&rawtime);
  localtime_r(&rawtime, &timeinfo);

  gb_set_rtc(gb, &timeinfo);
}

static int rom_file_picker(struct priv_s * const priv) {
  UTF16 utf16path[FILEPICKER_CONTEXT_OUTPUT_MAX_LFN] = {0};

  /* Invoke file picker. */
  filepicker_context_t ctx = {0};
  void *ctx_out = FILEPICKER_CONTEXT_OUTPUT_ALLOC(calloc, 1, true);
  if (ctx_out == NULL) {
    return 2;
  }

  ctx.paths = ctx_out;
  ctx.ctx_size = sizeof(ctx);
  ctx.path_max_cu = FILEPICKER_CONTEXT_OUTPUT_MAX_LFN;
  ctx.type_list = (
    "Game Boy ROM Files (*.gb, *.gbc)\0*.gb|*.gbc\0"
    "DMG ROM Files (*.gb)\0*.gb\0"
    "CGB ROM Files (*.gbc)\0*.gbc\0"
    "All Files (*.*)\0*.*\0"
    "\0\0\0"
  );

  bool ret = _GetOpenFileName(&ctx);

  if (!ret || _GetNextFileName(&ctx, utf16path) != 0) {
    free(ctx_out);
    return 1;
  }

  free(ctx_out);

  if (wcstombs(priv->rom_file_name, utf16path, sizeof(priv->rom_file_name)) == (size_t) -1) {
    return 2;
  };

  /* Copy the ROM file name to allocated space. */
  strcpy(priv->save_file_name, priv->rom_file_name);

  char *str_replace;

  /* If the file name does not have a dot, or the only dot is at
   * the start of the file name, set the pointer to begin
   * replacing the string to the end of the file name, otherwise
   * set it to the dot. */
  if ((str_replace = strrchr(priv->save_file_name, '.')) == NULL || str_replace == priv->save_file_name) {
    str_replace = priv->save_file_name + strlen(priv->save_file_name);
  }

  /* Copy extension to string including terminating null byte. */
  for (unsigned int i = 0; i <= strlen(SAVE_FILE_SUFFIX); i++) {
    *(str_replace++) = SAVE_FILE_SUFFIX[i];
  }

  return 0;
}

static inline void sleep_with_double_rtc(unsigned short ms) {
  datetime_t dt;

  if (ms >= 499) {
    return;
  }

  GetSysTime(&dt);
  int start = dt.millis;
  while (true) {
    OSSleep(1);
    GetSysTime(&dt);
    int ticks_elapsed = ((dt.millis < start) ? (1000 + dt.millis - start) : (dt.millis - start));
    // fudge
    if (ticks_elapsed >= ms * 2 - 3) {
      return;
    }
  }
}

static void loop(struct gb_s * const gb) {
  struct priv_s * const priv = gb->direct.priv;
  unsigned long long current_time = 0, last_time = 0, power_event_start = 0;
  short frame_advance_cnt = 0;
  short auto_save_counter = 0;
#if MANUAL_RTC_NEEDED
  short rtc_counter = 0;
#endif
  bool holding_quit_key = false, holding_mute_key = false, holding_save_key = false;
  short delay_factor_counter = 0;
  int delay_millis_sum = 0;

  bool debug_show_delay_factor = priv->config.debug_show_delay_factor;
  bool sram_auto_commit = priv->config.sram_auto_commit;
  short button_hold_compensation_num = priv->config.button_hold_compensation_num;
  short button_hold_compensation_denom = priv->config.button_hold_compensation_denom;

  while (true) {
    /* Power event handling. */
    if (power_event) {
      power_event_start = mutekix_time_get_usecs();
      power_event = false;
    }
    if (power_event_start != 0 && mutekix_time_get_usecs() - power_event_start >= 500000ull) {
      if (priv->config.sync_rtc_on_resume) {
        _set_rtc(gb);
      }
      power_event_start = 0;
    }

    last_time = mutekix_time_get_ticks();

    /* Cache the key code values in register to avoid repeated LDRs. */
    unsigned int emu_key_state_current = emu_key_state;

    if (emu_key_state_current & EMU_KEY_QUIT) {
      if (!holding_quit_key) {
        holding_quit_key = true;
        _input_poller_end(gb);
        unsigned int ret = MessageBox(
          _BUL("Are you sure you want to quit?"),
          MB_ICON_QUESTION | MB_BUTTON_YES | MB_BUTTON_NO
        );
        if (ret == MB_RESULT_YES) {
          break;
        }
        if (priv->config.sync_rtc_on_resume) {
          _set_rtc(gb);
        }
        _input_poller_begin(gb);
        continue;
      }
    } else {
      holding_quit_key = false;
    }

    if (emu_key_state_current & EMU_KEY_MUTE) {
      if (!holding_mute_key) {
        if (priv->sound_on) {
          _sound_off(gb);
        } else {
          _sound_on(gb);
        }
      }
      holding_mute_key = true;
    } else {
      holding_mute_key = false;
    }

    /* Handle vertical scrolling for 240x96 screens. */
    if (priv->height < LCD_HEIGHT) {
      if (emu_key_state_current & EMU_KEY_SCROLL_UP) {
        if (priv->yskip > 0) {
          priv->yskip--;
        }
      } else if (emu_key_state_current & EMU_KEY_SCROLL_DOWN) {
        if (priv->yskip < LCD_HEIGHT - priv->height) {
          priv->yskip++;
        }
      } else if (emu_key_state_current & EMU_KEY_SCROLL_TOP) {
        priv->yskip = 0;
      } else if (emu_key_state_current & EMU_KEY_SCROLL_CENTER) {
        priv->yskip = (LCD_HEIGHT - priv->height) / 2;
      } else if (emu_key_state_current & EMU_KEY_SCROLL_BOTTOM) {
        priv->yskip = LCD_HEIGHT - priv->height;
      }
    }

    if (emu_key_state_current & EMU_KEY_RESET) {
      gb_reset(gb);
    }

    gb->direct.joypad = ~pad_key_state;

    gb_run_frame(gb);
    if (priv->sound_on) {
      uint8_t pbuf = audio_buffer_producer_offset;
      if (((pbuf + 1) & 3) != audio_buffer_consumer_offset) {
        audio_callback_wrapper(&audio_buffer[pbuf * AUDIO_SAMPLES_TOTAL]);
        pbuf++;
        pbuf &= 3;
        audio_buffer_producer_offset = pbuf;
      }
    }

    if (priv->fallback_blit && !priv->p4_1line_buffer) {
      _BitBlt(priv->real_fb, priv->canvas_x, priv->canvas_y, priv->width, priv->height, priv->fb, 0, 0, BLIT_NONE);
    }

    if (emu_key_state_current & EMU_KEY_SRAM_COMMIT) {
      if (!holding_save_key) {
        _write_save(gb, priv->save_file_name);
        auto_save_counter = 0;
      }
      holding_save_key = true;
    } else {
      holding_save_key = false;
    }

    auto_save_counter++;
    if (auto_save_counter > 3600) {
      if (sram_auto_commit) {
        _write_save(gb, priv->save_file_name);
      }
      auto_save_counter = 0;
    }

#if MANUAL_RTC_NEEDED
    rtc_counter++;
    if (rtc_counter >= 60) {
      gb_tick_rtc(gb);
      rtc_counter -= 60;
    }
#endif

    /* Calculate frame advance time */
    short frame_advance = (frame_advance_cnt == 0) ? 16 : 17;
    frame_advance_cnt++;
    if (frame_advance_cnt >= 3) {
      frame_advance_cnt = 0;
    }

    current_time = mutekix_time_get_ticks();

    long long elapsed_time = (mutekix_time_get_quantum() == 500) ? (current_time - last_time) / 2 : current_time - last_time;

    short sleep_millis = frame_advance - elapsed_time;

    if (holding_any_key && (button_hold_compensation_denom != 1 || button_hold_compensation_denom != 1)) {
      sleep_millis -= elapsed_time * button_hold_compensation_num / button_hold_compensation_denom;
    }

    if (debug_show_delay_factor) {
      delay_millis_sum += sleep_millis;
      delay_factor_counter++;
      if (delay_factor_counter >= 32) {
        PrintfXY(0, 0, "%5d", delay_millis_sum >> 5);
        delay_factor_counter = 0;
        delay_millis_sum = 0;
      }
    }

    /* Yield from current thread so other threads (like the input poller) can be executed on-time */
    if (mutekix_time_get_quantum() == 500) {
      sleep_with_double_rtc(sleep_millis > 0 ? sleep_millis : 1);
    } else {
      OSSleep(sleep_millis > 0 ? sleep_millis : 1);
    }
  }
}

static void _load_config(struct priv_s *priv) {
  priv->config.enable_audio = !!_GetPrivateProfileInt("Config", "EnableAudio", 1, CONFIG_PATH);
  priv->config.interlace = !!_GetPrivateProfileInt("Config", "Interlace", 0, CONFIG_PATH);
  priv->config.half_refresh = !!_GetPrivateProfileInt("Config", "HalfRefresh", 0, CONFIG_PATH);
  priv->config.sram_auto_commit = !!_GetPrivateProfileInt("Config", "SRAMAutoCommit", 1, CONFIG_PATH);
  priv->config.button_hold_compensation_num = _GetPrivateProfileInt("Config", "ButtonHoldCompensationNum", 1, CONFIG_PATH) & 0xffff;
  priv->config.button_hold_compensation_denom = _GetPrivateProfileInt("Config", "ButtonHoldCompensationDenom", 1, CONFIG_PATH) & 0xffff;
  priv->config.multi_press_mode = _GetPrivateProfileInt("Config", "MultiPressMode", MULTI_PRESS_MODE_DIS, CONFIG_PATH);
  priv->config.sync_rtc_on_resume = !!_GetPrivateProfileInt("Config", "SyncRTCOnResume", 0, CONFIG_PATH);
  priv->config.debug_show_delay_factor = !!_GetPrivateProfileInt("Debug", "ShowDelayFactor", 0, CONFIG_PATH);
  priv->config.debug_force_safe_framebuffer = !!_GetPrivateProfileInt("Debug", "ForceSafeFramebuffer", 0, CONFIG_PATH);

  /* Filter out illegal values that may cause bad behavior. */
  if (priv->config.button_hold_compensation_num == 0) {
    priv->config.button_hold_compensation_num = 1;
  }
  if (priv->config.button_hold_compensation_denom == 0) {
    priv->config.button_hold_compensation_denom = 1;
  }
}

static void _load_key_binding(void) {
  g_key_binding.a = _GetPrivateProfileInt("KeyBinding", "A", KEY_X, CONFIG_PATH);
  g_key_binding.b = _GetPrivateProfileInt("KeyBinding", "B", KEY_Z, CONFIG_PATH);
  g_key_binding.select = _GetPrivateProfileInt("KeyBinding", "Select", KEY_A, CONFIG_PATH);
  g_key_binding.start = _GetPrivateProfileInt("KeyBinding", "Start", KEY_S, CONFIG_PATH);
  g_key_binding.right = _GetPrivateProfileInt("KeyBinding", "Right", KEY_RIGHT, CONFIG_PATH);
  g_key_binding.left = _GetPrivateProfileInt("KeyBinding", "Left", KEY_LEFT, CONFIG_PATH);
  g_key_binding.up = _GetPrivateProfileInt("KeyBinding", "Up", KEY_UP, CONFIG_PATH);
  g_key_binding.down = _GetPrivateProfileInt("KeyBinding", "Down", KEY_DOWN, CONFIG_PATH);
  g_key_binding.reset_combo = _GetPrivateProfileInt("KeyBinding", "ResetCombo", KEY_R, CONFIG_PATH);

  g_key_binding.quit = _GetPrivateProfileInt("KeyBinding", "Quit", KEY_ESC, CONFIG_PATH);
  g_key_binding.mute = _GetPrivateProfileInt("KeyBinding", "Mute", KEY_M, CONFIG_PATH);
  g_key_binding.reset_hard = _GetPrivateProfileInt("KeyBinding", "ResetHard", KEY_H, CONFIG_PATH);
  g_key_binding.scroll_up = _GetPrivateProfileInt("KeyBinding", "ScrollUp", KEY_PGUP, CONFIG_PATH);
  g_key_binding.scroll_down = _GetPrivateProfileInt("KeyBinding", "ScrollDown", KEY_PGDN, CONFIG_PATH);
  g_key_binding.scroll_top = _GetPrivateProfileInt("KeyBinding", "ScrollTop", KEY_1, CONFIG_PATH);
  g_key_binding.scroll_center = _GetPrivateProfileInt("KeyBinding", "ScrollCenter", KEY_2, CONFIG_PATH);
  g_key_binding.scroll_bottom = _GetPrivateProfileInt("KeyBinding", "ScrollBottom", KEY_3, CONFIG_PATH);
  g_key_binding.sram_commit = _GetPrivateProfileInt("KeyBinding", "SRAMCommit", KEY_SAVE, CONFIG_PATH);
}

int main(void) {
  static struct gb_s gb;
  static struct priv_s priv = {0};

  _load_config(&priv);
  _load_key_binding();

  int file_picker_result = rom_file_picker(&priv);
  if (file_picker_result > 0) {
    return file_picker_result - 1;
  }

  /* Setup emulator states. */
  lcd_t *lcd = GetActiveLCD();
  if (lcd == NULL || lcd->surface == NULL) {
    return 1;
  }

  rgbSetColor(lcd->surface->depth == LCD_SURFACE_PIXFMT_L4 ? 0x000000 : 0xffffff);
  rgbSetBkColor(lcd->surface->depth == LCD_SURFACE_PIXFMT_L4 ? 0xffffff : 0x000000);
  SetFontType(MONOSPACE_CJK);
  ClearScreen(false);
  WriteAlignString(
    lcd->width / 2,
    (lcd->height - GetFontHeight(MONOSPACE_CJK)) / 2,
    _BUL("Loading..."),
    lcd->width,
    STR_ALIGN_CENTER,
    PRINT_NONE
  );

  priv.rom = _read_file(priv.rom_file_name, 0, false);
  if (priv.rom == NULL) {
    return 1;
  }

  enum gb_init_error_e gb_ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read, &gb_cart_ram_write, &gb_error, &priv);

  switch(gb_ret) {
  case GB_INIT_NO_ERROR:
    break;

  case GB_INIT_CARTRIDGE_UNSUPPORTED:
    MessageBox(_BUL("Unsupported cartridge."), MB_DEFAULT);
    exit_cleanup(&gb);
    return 1;

  case GB_INIT_INVALID_CHECKSUM:
    MessageBox(_BUL("Invalid ROM: Checksum failure."), MB_DEFAULT);
    exit_cleanup(&gb);
    return 1;

  default: {
    messagebox_format(MB_DEFAULT, "Unknown error: %d", gb_ret);
    exit_cleanup(&gb);
    return 1;
  }
  }

  _set_rtc(&gb);
  audio_init();

  if (gb_get_save_size_s(&gb, &priv.cart_ram_size) < 0) {
    MessageBox(_BUL("Unable to get save size."), MB_BUTTON_OK | MB_ICON_ERROR);
    exit_cleanup(&gb);
    return 1;
  }
  if (priv.cart_ram_size != 0) {
    priv.cart_ram = _read_file(priv.save_file_name, priv.cart_ram_size, true);
    if (priv.cart_ram == NULL) {
      MessageBox(
        _BUL("Save data is available but the emulator is unable to load it."),
        MB_BUTTON_OK | MB_ICON_ERROR
      );
      exit_cleanup(&gb);
      return 1;
    }
  }

  if (lcd->surface->depth == LCD_SURFACE_PIXFMT_XRGB && !priv.config.debug_force_safe_framebuffer) {
    priv.fb = lcd->surface;
    priv.rotation = lcd->rotation;
    /* Only use the rotation-aware blit when absolutely needed. Saves about 1-2ms on BA802. */
    if (lcd->rotation != ROTATION_TOP_SIDE_FACING_UP) {
      gb_init_lcd(&gb, &lcd_draw_line_fast_xrgb_rot);
    } else {
      gb_init_lcd(&gb, &lcd_draw_line_fast_xrgb);
    }
  } else if (lcd->surface->depth == LCD_SURFACE_PIXFMT_L4 && !priv.config.debug_force_safe_framebuffer) {
    /* 4-bit LCD machines don't have a hardware-backed framebuffer and
     * we need to blit a 160x1 buffer to the screen line-by-line. */
    priv.fallback_blit = true;
    priv.p4_1line_buffer = true;
    priv.fb = (lcd_surface_t *) calloc(GetImageSizeExt(LCD_WIDTH, 1, LCD_SURFACE_PIXFMT_L4), 1);
    priv.real_fb = lcd->surface;
    InitGraphic(priv.fb, LCD_WIDTH, 1, LCD_SURFACE_PIXFMT_L4);
    memcpy(priv.fb->palette, PALETTE_P4, sizeof(PALETTE_P4));
    gb_init_lcd(&gb, &lcd_draw_line_fast_p4);
  } else {
    priv.fallback_blit = true;
    priv.fb = (lcd_surface_t *) calloc(GetImageSizeExt(LCD_WIDTH, LCD_HEIGHT, LCD_SURFACE_PIXFMT_L4), 1);
    priv.real_fb = lcd->surface;
    InitGraphic(priv.fb, LCD_WIDTH, LCD_HEIGHT, LCD_SURFACE_PIXFMT_L4);
    memcpy(priv.fb->palette, PALETTE_P4, sizeof(PALETTE_P4));
    gb_init_lcd(&gb, &lcd_draw_line_safe);
  }

  _set_blit_parameter(&gb, lcd->surface);
  _precompute_yoff(&gb);

  /* TODO make these toggle-able with hotkeys */
  gb.direct.interlace = priv.config.interlace;
  gb.direct.frame_skip = priv.config.half_refresh;

  if (priv.config.enable_audio) {
    _sound_on(&gb);
  }
  mutekix_time_init();

  _input_poller_begin(&gb);
  loop(&gb);
  _input_poller_end(&gb);

  _write_save(&gb, priv.save_file_name);

  mutekix_time_fini();
  _sound_off(&gb);

  exit_cleanup(&gb);
  return 0;
}

static void exit_cleanup(const struct gb_s * const gb) {
  struct priv_s * const priv = gb->direct.priv;
  if (priv->rom != NULL) {
    free(priv->rom);
    priv->rom = NULL;
  }
  
  if (priv->cart_ram != NULL) {
    free(priv->cart_ram);
    priv->cart_ram = NULL;
  }

  if (priv->fallback_blit) {
    free(priv->fb);
    priv->fb = NULL;
  }
}

static int messagebox_format(unsigned short type, const char *fmt, ...) {
  va_list va;

  static char message[256] = {0};
  static UTF16 messagew[256] = {0};

  va_start(va, fmt);
  vsniprintf(message, sizeof(message), fmt, va);
  va_end(va);

  ConvStrToUnicode(message, messagew, MB_ENCODING_UTF8);
  return MessageBox(messagew, type);
}
