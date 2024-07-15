#include <inttypes.h>
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
#include <muteki/utils.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/event.h>
#include <muteki/ui/surface.h>
#include <muteki/ui/views/filepicker.h>
#include <muteki/ui/views/messagebox.h>

#include <mutekix/threading.h>

#define ENABLE_SOUND 1

#include "minigb_apu.h"
#include "peanut_gb.h"

#ifndef AUDIO_SAMPLES_TOTAL
#define AUDIO_SAMPLES_TOTAL AUDIO_SAMPLES * 2
#endif

static int _audio_worker(void *user_data);
static int _tim1_emulator_worker(void *user_data);
static int _sched_timer_worker(void *user_data);

static mutekix_thread_arg_t audio_worker_arg = {
  .func = &_audio_worker,
  .user_data = NULL,
};

static mutekix_thread_arg_t tim1_worker_arg = {
  .func = &_tim1_emulator_worker,
  .user_data = NULL,
};

static mutekix_thread_arg_t sched_timer_worker_arg = {
  .func = &_sched_timer_worker,
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

const char SAVE_FILE_SUFFIX[] = ".sav";

const key_press_event_config_t KEY_EVENT_CONFIG_DRAIN = {65535, 65535, 1};
const key_press_event_config_t KEY_EVENT_CONFIG_TURBO = {0, 0, 0};

volatile short pressing0 = 0, pressing1 = 0;
volatile uint8_t audio_buffer_consumer_offset;
volatile uint8_t audio_buffer_producer_offset;
volatile bool audio_running = false;
volatile bool tim1_emulator_running = false;
volatile unsigned short sched_timer_ticks = 0;
int16_t audio_buffer[4][AUDIO_SAMPLES_TOTAL];
thread_t *audio_worker_inst = NULL;
thread_t *tim1_worker_inst = NULL;
thread_t *sched_timer_worker_inst = NULL;
event_t *audio_shutdown_ack = NULL;
event_t *tim1_shutdown_ack = NULL;

struct priv_config_s {
  bool enable_audio;
  bool interlace;
  bool half_refresh;
  bool use_scheduler_timer;
  bool sram_auto_commit;
  bool debug_show_delay_factor;
};

struct priv_s {
  /* Pointer to allocated memory holding GB file. */
  uint8_t *rom;
  /* Pointer to allocated memory holding save file. */
  uint8_t *cart_ram;

  /* Framebuffer objects. */
  lcd_surface_t *fb;
  lcd_surface_t *real_fb;

  /* Blit offset and limit. In fallback blit mode, these are passed to _BitBlt. In fast mode, these are used
     directly by the fast blit routine. */
  unsigned short x;
  unsigned short y;
  unsigned short yskip;
  unsigned short width;
  unsigned short height;
  size_t yoff[LCD_HEIGHT];

  /* Backup of key press event parameters. */
  key_press_event_config_t old_hold_cfg;

  /* Direct Input Simulation (DIS) and sound emulation state. */
  bool dis_active;
  bool sound_on;

  /* Use fallback blit algorithm. */
  bool fallback_blit;
  bool p4_1line_buffer;

  /* Filenames for future reference. */
  char save_file_name[260 * 3 + sizeof(SAVE_FILE_SUFFIX)];
  char rom_file_name[260 * 3];

  struct priv_config_s config;

};

static inline bool _test_events_no_shift(ui_event_t *uievent) {
    // Deactivate shift key because it may cause the keycode to change.
    // This means we need to handle shift behavior ourselves (if we really need it) but that's a fair tradeoff.
    SetShiftState(TOGGLE_KEY_INACTIVE);
    return TestPendEvent(uievent) || TestKeyEvent(uievent);
}

static inline void _ext_ticker() {
  static ui_event_t uievent = {0};
  static uint_fast16_t down_counter = 0;
  bool hit = false;

  /* TODO this still seem to lose track presses on BA110. Find out why. */
  if (down_counter <= 3) {
    while (_test_events_no_shift(&uievent)) {
      hit = true;
      if (GetEvent(&uievent) && uievent.event_type == 0x10) {
        pressing0 = uievent.key_code0;
        pressing1 = uievent.key_code1;
        down_counter = 7;
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
      WriteFile(pcmdev, audio_buffer[cbuf], AUDIO_SAMPLES_TOTAL * 2, &actual_size, NULL);
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

static int _tim1_emulator_worker(void *user_data) {
  (void) user_data;

  OSResetEvent(tim1_shutdown_ack);

  tim1_emulator_running = true;

  while (tim1_emulator_running) {
    _ext_ticker();
    OSSleep(5);
  }
  OSSetEvent(tim1_shutdown_ack);

  return 0;
}

static int _sched_timer_worker(void *user_data) {
  (void) user_data;
  while (true) {
    sched_timer_ticks++;
    if (sched_timer_ticks >= 1000) {
      sched_timer_ticks = 0;
    }
    OSSleep(1);
  }
  return 0;
}

static inline void _drain_all_events() {
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

static void _direct_input_sim_begin(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  if (!priv->dis_active) {
    GetSysKeyState(&priv->old_hold_cfg);

    tim1_shutdown_ack = OSCreateEvent(true, 1);
    tim1_worker_inst = OSCreateThread(&mutekix_thread_wrapper, &tim1_worker_arg, 16384, false);

    SetSysKeyState(&KEY_EVENT_CONFIG_TURBO);
    OSSleep(1);
    priv->dis_active = true;
  }
}

static void _direct_input_sim_end(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  if (priv->dis_active) {
    SetSysKeyState(&KEY_EVENT_CONFIG_DRAIN);

    tim1_emulator_running = false;
    while (OSWaitForEvent(tim1_shutdown_ack, 1000) != WAIT_RESULT_RESOLVED) {};
    OSCloseEvent(tim1_shutdown_ack);
    OSSleep(1);
    if (tim1_worker_inst != NULL) {
      OSTerminateThread(tim1_worker_inst, 0);
      tim1_worker_inst = NULL;
    }

    _drain_all_events();

    SetSysKeyState(&priv->old_hold_cfg);
    priv->dis_active = false;
  }
}

static void _sound_on(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;

  if (!priv->sound_on) {
    audio_buffer_consumer_offset = 0;
    audio_buffer_producer_offset = 0;
    memset(audio_buffer, 0, sizeof(audio_buffer));
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
    priv->sound_on = false;
  }
}

static void _sched_timer_start() {
  if (sched_timer_worker_inst == NULL) {
    sched_timer_worker_inst = OSCreateThread(&mutekix_thread_wrapper, &sched_timer_worker_arg, 4096, false);
  }
}

static void _sched_timer_stop() {
  if (sched_timer_worker_inst != NULL) {
    OSTerminateThread(sched_timer_worker_inst, 0);
    sched_timer_worker_inst = NULL;
  }
}

static void _set_blit_parameter(struct gb_s *gb, const lcd_surface_t * const surface) {
  struct priv_s *priv = gb->direct.priv;

  if (surface == NULL) {
    priv->x = 0;
    priv->y = 0;
    priv->width = LCD_WIDTH;
    priv->height = LCD_HEIGHT;
    return;
  }

  int xoff = (surface->width - LCD_WIDTH) / 2, yoff = (surface->height - LCD_HEIGHT) / 2;
  if (xoff <= 0) {
    priv->x = 0;
    priv->width = surface->width;
  } else {
    priv->x = xoff;
    priv->width = LCD_WIDTH;
  }
  if (yoff <= 0) {
    priv->y = 0;
    priv->height = surface->height;
  } else {
    priv->y = yoff;
    priv->height = LCD_HEIGHT;
  }
}

static void _precompute_yoff(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  bool use_intermediate_fb = priv->fallback_blit;

  unsigned short x = use_intermediate_fb ? 0 : priv->x;
  unsigned short y = use_intermediate_fb ? 0 : priv->y;

  size_t xoff = x;

  switch (priv->fb->depth) {
  case LCD_SURFACE_PIXFMT_L4:
    xoff = x / 2;
    break;
  case LCD_SURFACE_PIXFMT_XRGB:
    xoff = x * 4;
    break;
  }

  for (size_t i = 0; i < LCD_HEIGHT; i++) {
    priv->yoff[i] = (y + i) * priv->fb->xsize + xoff;
  }
}

static uint8_t *_read_file(const char *path, size_t size, bool allocate_anyway) {
  struct stat st = {0};

  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    if (allocate_anyway && size > 0) {
      return (uint8_t *) malloc(size);
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
  size_t save_size = gb_get_save_size(gb);
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

static inline uint8_t _map_pad_state(short key) {
  switch (key) {
    case KEY_RIGHT:
      return JOYPAD_RIGHT;
    case KEY_LEFT:
      return JOYPAD_LEFT;
    case KEY_DOWN:
      return JOYPAD_DOWN;
    case KEY_UP:
      return JOYPAD_UP;
    case KEY_S:
      return JOYPAD_START; // Start
    case KEY_A:
      return JOYPAD_SELECT; // Select
    case KEY_Z:
      return JOYPAD_B; // B
    case KEY_X:
      return JOYPAD_A; // A
    case KEY_R:
      return JOYPAD_A | JOYPAD_B | JOYPAD_SELECT | JOYPAD_START; // Game soft reset
    default:
      return 0;
  }
}

void lcd_draw_line_safe(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
  const struct priv_s * const priv = gb->direct.priv;
  lcd_surface_t *fb = priv->fb;

  for (size_t x = 0; x < LCD_WIDTH; x += 2) {
    ((uint8_t *) fb->buffer)[priv->yoff[line] + x / 2] = (
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
    ((uint8_t *) fb->buffer)[x / 2] = (
      ((COLOR_MAP[pixels[x] & 3] & 0xf) << 4) |
      (COLOR_MAP[pixels[x + 1] & 3] & 0xf)
    );
  }

  _BitBlt(priv->real_fb, priv->x & 0xfffe, priv->y + line - priv->yskip, LCD_WIDTH, 1, priv->fb, 0, 0, BLIT_NONE);
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
    size_t pixel_offset = priv->yoff[line] + x * 4;
    /* TODO palette */
    ((uint8_t *) fb->buffer)[pixel_offset + 0] = COLOR_MAP[pixels[x] & 3];
    ((uint8_t *) fb->buffer)[pixel_offset + 1] = COLOR_MAP[pixels[x] & 3];
    ((uint8_t *) fb->buffer)[pixel_offset + 2] = COLOR_MAP[pixels[x] & 3];
    ((uint8_t *) fb->buffer)[pixel_offset + 3] = 0xff;
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
  struct priv_s *priv = gb->direct.priv;
  char error_msg[256];
  UTF16 error_msg_w[256];
  char location[64] = "";
  uint8_t instr_byte;

  _direct_input_sim_end(gb);

  /* Record save file. */
  _write_save(gb, "recovery.sav");

  /* Stop workers (if they are running). */
  _sched_timer_stop();
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

  sniprintf(error_msg, sizeof(error_msg),
    "Error: %s at 0x%04X%s with instruction %02X.\n"
    "Cart RAM saved to recovery.sav\n"
    "Exiting.\n",
    gb_err_str[gb_err], addr, location, instr_byte);

  ConvStrToUnicode(error_msg, error_msg_w, MB_ENCODING_UTF8);
  MessageBox(error_msg_w, MB_ICON_ERROR | MB_BUTTON_OK);

  /* Free memory and then exit. */
  free(priv->rom);
  if (priv->cart_ram != NULL) {
    free(priv->cart_ram);
  }
  if (priv->fallback_blit) {
    free(priv->fb);
  }
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
  UTF16 utf16path[260] = {0};

  /* Invoke file picker. */
  filepicker_context_t ctx = {0};
  void *ctx_out = FILEPICKER_CONTEXT_OUTPUT_ALLOC(calloc, 1, true);
  if (ctx_out == NULL) {
    return 2;
  }

  ctx.paths = ctx_out;
  ctx.ctx_size = sizeof(ctx);
  ctx.unk_0x30 = 0xffff;
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

static void loop(struct gb_s * const gb) {
  struct priv_s * const priv = gb->direct.priv;
  datetime_t dt;
  int last_millis = 0, current_millis = 0;
  short frame_advance_cnt = 0;
  short auto_save_counter = 0;
  bool holding_mute_key = false;
  short delay_factor_counter = 0;
  int delay_millis_sum = 0;

  bool use_scheduler_timer = priv->config.use_scheduler_timer;
  bool debug_show_delay_factor = priv->config.debug_show_delay_factor;
  bool sram_auto_commit = priv->config.sram_auto_commit;

  while (true) {
    if (use_scheduler_timer) {
      last_millis = sched_timer_ticks;
    } else {
      GetSysTime(&dt);
      last_millis = dt.millis;
    }

    /* Cache the key code values in register to avoid repeated LDRs. */
    short pressing0_curr = pressing0, pressing1_curr = pressing1;

    if (pressing0_curr == KEY_ESC || pressing1_curr == KEY_ESC) {
      break;
    }

    if (pressing0_curr == KEY_M || pressing1_curr == KEY_M) {
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
      if (pressing0_curr == KEY_PGUP || pressing1_curr == KEY_PGUP) {
        if (priv->yskip > 0) {
          priv->yskip--;
        }
      } else if (pressing0_curr == KEY_PGDN || pressing1_curr == KEY_PGDN) {
        if (priv->yskip < LCD_HEIGHT - priv->height) {
          priv->yskip++;
        }
      } else if (pressing0_curr == KEY_1 || pressing1_curr == KEY_1) {
        priv->yskip = 0;
      } else if (pressing0_curr == KEY_2 || pressing1_curr == KEY_2) {
        priv->yskip = (LCD_HEIGHT - priv->height) / 2;
      } else if (pressing0_curr == KEY_3 || pressing1_curr == KEY_3) {
        priv->yskip = LCD_HEIGHT - priv->height;
      }
    }

    if (pressing0_curr == KEY_H || pressing1_curr == KEY_H) {
      gb_reset(gb);
    }

    gb->direct.joypad = ~(_map_pad_state(pressing0_curr) | _map_pad_state(pressing1_curr));
    gb_run_frame(gb);
    if (priv->sound_on) {
      uint8_t pbuf = audio_buffer_producer_offset;
      if (((pbuf + 1) & 3) != audio_buffer_consumer_offset) {
        audio_callback(NULL, (uint8_t *) audio_buffer[pbuf], AUDIO_SAMPLES_TOTAL * 2);
        pbuf++;
        pbuf &= 3;
        audio_buffer_producer_offset = pbuf;
      }
    }

    if (priv->fallback_blit && !priv->p4_1line_buffer) {
      _BitBlt(priv->real_fb, priv->x, priv->y, priv->width, priv->height, priv->fb, 0, 0, BLIT_NONE);
    }

    auto_save_counter++;
    if (auto_save_counter > 3600) {
      if (sram_auto_commit) {
        _write_save(gb, priv->save_file_name);
      }
      auto_save_counter = 0;
    }

    /* Calculate frame advance time */
    short frame_advance = (frame_advance_cnt == 0) ? 16 : 17;
    frame_advance_cnt++;
    if (frame_advance_cnt >= 3) {
      frame_advance_cnt = 0;
    }

    if (use_scheduler_timer) {
      current_millis = sched_timer_ticks;
    } else {
      GetSysTime(&dt);
      current_millis = dt.millis;
    }

    short elapsed_millis = (current_millis >= last_millis) ? (current_millis - last_millis) : (1000 + current_millis - last_millis);
    short sleep_millis = frame_advance - elapsed_millis;

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
    OSSleep(sleep_millis > 0 ? sleep_millis : 1);
  }
}

int main(void) {
  static struct gb_s gb;
  static struct priv_s priv = {0};

  priv.config.enable_audio = !!_GetPrivateProfileInt("Config", "EnableAudio", 1, "pgbcfg.ini");
  priv.config.interlace = !!_GetPrivateProfileInt("Config", "Interlace", 0, "pgbcfg.ini");
  priv.config.half_refresh = !!_GetPrivateProfileInt("Config", "HalfRefresh", 0, "pgbcfg.ini");
  priv.config.use_scheduler_timer = !!_GetPrivateProfileInt("Config", "UseSchedulerTimer", 0, "pgbcfg.ini");
  priv.config.sram_auto_commit = !!_GetPrivateProfileInt("Config", "SRAMAutoCommit", 1, "pgbcfg.ini");
  priv.config.debug_show_delay_factor = !!_GetPrivateProfileInt("Debug", "ShowDelayFactor", 0, "pgbcfg.ini");

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
    free(priv.rom);
    priv.rom = NULL;
    return 1;

  case GB_INIT_INVALID_CHECKSUM:
    MessageBox(_BUL("Invalid ROM: Checksum failure."), MB_DEFAULT);
    free(priv.rom);
    priv.rom = NULL;
    return 1;

  default: {
    char message[64] = {0};
    UTF16 messagew[64] = {0};
    sniprintf(message, sizeof(message), "Unknown error: %d", gb_ret);
    ConvStrToUnicode(message, messagew, MB_ENCODING_UTF8);
    MessageBox(messagew, MB_DEFAULT);
    free(priv.rom);
    priv.rom = NULL;
    return 1;
  }
  }

  _set_rtc(&gb);
  audio_init();

  if (gb_get_save_size(&gb) != 0) {
    priv.cart_ram = _read_file(priv.save_file_name, gb_get_save_size(&gb), true);
    if (priv.cart_ram == NULL) {
      free(priv.rom);
      priv.rom = NULL;
      return 1;
    }
  }

  if (lcd->surface->depth == LCD_SURFACE_PIXFMT_XRGB) {
    priv.fb = lcd->surface;
    gb_init_lcd(&gb, &lcd_draw_line_fast_xrgb);
  } else if (lcd->surface->depth == LCD_SURFACE_PIXFMT_L4) {
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

  if (priv.config.use_scheduler_timer) {
    _sched_timer_start();
  }

  _direct_input_sim_begin(&gb);
  loop(&gb);
  _direct_input_sim_end(&gb);

  _write_save(&gb, priv.save_file_name);

  _sched_timer_stop();
  _sound_off(&gb);

  free(priv.rom);
  if (priv.cart_ram != NULL) {
    free(priv.cart_ram);
  }
  if (priv.fallback_blit) {
    free(priv.fb);
  }
  return 0;
}
