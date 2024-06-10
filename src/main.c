#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include <muteki/datetime.h>
#include <muteki/system.h>
#include <muteki/utf16.h>
#include <muteki/utils.h>
#include <muteki/ui/canvas.h>
#include <muteki/ui/event.h>
#include <muteki/ui/surface.h>
#include <muteki/ui/views/filepicker.h>
#include <muteki/ui/views/messagebox.h>

#include "peanut_gb.h"
#include "minigb_apu.h"

/* TODO make non-OS-specific lcd draw function (detect board type and write
   Nuvoton registers directly from converted buffer? Or better, from pixel[160]
   to register directly.) */
typedef void (*lcd_draw_callback)(
    lcd_surface_t *dst,
    short xdstoffset,
    short ydstoffset,
    short xsize,
    short ysize
);
const lcd_draw_callback ca106_lcd_draw = (lcd_draw_callback) 0x300214dc;

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
  unsigned short width;
  unsigned short height;
  size_t yoff[LCD_HEIGHT];

  /* Backup of key press event parameters. */
  key_press_event_config_t old_hold_cfg;

  /* Direct Input Simulation (DIS) state. */
  bool dis_active;
  /* Use fallback blit algorithm. */
  bool fallback_blit;

  /* Filenames for future reference. */
  char save_file_name[260 * 3 + + sizeof(SAVE_FILE_SUFFIX)];
  char rom_file_name[260 * 3];
};

static inline bool _test_events_no_shift(ui_event_t *uievent) {
    // Deactivate shift key because it may cause the keycode to change.
    // This means we need to handle shift behavior ourselves (if we really need it) but that's a fair tradeoff.
    SetShiftState(TOGGLE_KEY_INACTIVE);
    return TestPendEvent(uievent) || TestKeyEvent(uievent);
}

static void _ext_ticker() {
  static ui_event_t uievent = {0};
  bool hit = false;

  /* TODO this still seem to lose track presses on BA110. Find out why. */
  while (_test_events_no_shift(&uievent)) {
    hit = true;
    if (GetEvent(&uievent) && uievent.event_type == 0x10) {
      pressing0 = uievent.key_code0;
      pressing1 = uievent.key_code1;
    } else {
      ClearEvent(&uievent);
    }
  }

  if (!hit) {
    pressing0 = 0;
    pressing1 = 0;
  }
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
    SetTimer1IntHandler(&_ext_ticker, 3);
    SetSysKeyState(&KEY_EVENT_CONFIG_TURBO);
    priv->dis_active = true;
  }
}

static void _direct_input_sim_end(struct gb_s *gb) {
  struct priv_s *priv = gb->direct.priv;
  if (priv->dis_active) {
    SetSysKeyState(&KEY_EVENT_CONFIG_DRAIN);
    SetTimer1IntHandler(NULL, 0);
    _drain_all_events();
    SetSysKeyState(&priv->old_hold_cfg);
    priv->dis_active = false;
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

  if (line >= priv->height) {
    return;
  }

  ((uint8_t *) fb->buffer)[0] = 0xff;

  for (size_t x = 0; x < LCD_WIDTH; x += 2) {
    if (x >= priv->width) {
      break;
    }
    /* TODO: handle misaligned pixels (i.e. when priv->x is odd). */
    ((uint8_t *) fb->buffer)[priv->yoff[line] + x / 2] = (
      ((COLOR_MAP[pixels[x] & 3] & 0xf) << 4) |
      (COLOR_MAP[pixels[x + 1] & 3] & 0xf)
    );
  }
  
  (*ca106_lcd_draw)(priv->fb, priv->x & 0xfffe, priv->y + line, LCD_WIDTH, 1);
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

  if(addr >= 0x4000 && addr < 0x8000)
  {
    uint32_t rom_addr;
    rom_addr = (uint32_t)addr * (uint32_t)gb->selected_rom_bank;
    snprintf(location, sizeof(location),
      " (bank %d mode %d, file offset %" PRIu32 ")",
      gb->selected_rom_bank, gb->cart_mode_select, rom_addr);
  }

  instr_byte = __gb_read(gb, addr);

  snprintf(error_msg, sizeof(error_msg),
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
    "Game Boy ROM Files (*.gb)\0*.gb\0"
    "Game Boy Color ROM Files (*.gbc)\0*.gbc\0"
    "All Files (*.*)\0*.*\0"
    "\0\0\0"
  );

  bool ret = _GetOpenFileName(&ctx);

  if (!ret || _GetNextFileName(&ctx, utf16path) != 0) {
    free(ctx_out);
    return 1;
  }

  free(ctx_out);

  /* TODO proper encoding conversion. */
  for (size_t i = 0; i < sizeof(priv->rom_file_name); i++) {
    if (utf16path[i] == 0) {
      break;
    }
    if (utf16path[i] > 0x7f) {
      return 2;
    }
    priv->rom_file_name[i] = utf16path[i] & 0x7f;
  }

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
  const struct priv_s * const priv = gb->direct.priv;
  datetime_t dt;
  int last_millis = 0;
  short frame_advance_cnt = 0;
  short auto_save_counter = 0;

  while (true) {
    GetSysTime(&dt);
    last_millis = dt.millis;

    if (pressing0 == KEY_ESC || pressing1 == KEY_ESC) {
      break;
    }
    gb->direct.joypad = ~(_map_pad_state(pressing0) | _map_pad_state(pressing1));
    gb_run_frame(gb);

    if (priv->fallback_blit) {
      _BitBlt(priv->real_fb, priv->x, priv->y, priv->width, priv->height, priv->fb, 0, 0, BLIT_NONE);
    }

    auto_save_counter++;
    if (auto_save_counter > 3600) {
      _write_save(gb, priv->save_file_name);
      auto_save_counter = 0;
    }
    
    /* Calculate frame advance time */
    short frame_advance = (frame_advance_cnt == 0) ? 16 : 17;
    frame_advance_cnt++;
    if (frame_advance_cnt >= 3) {
      frame_advance_cnt = 0;
    }

    GetSysTime(&dt);
    short elapsed_millis = (dt.millis >= last_millis) ? (dt.millis - last_millis) : (1000 + dt.millis - last_millis);
    short sleep_millis = frame_advance - elapsed_millis;

    if (sleep_millis > 0) {
      OSSleep(sleep_millis);
    } else {
      /* Yield from current thread so other threads (like the input poller) can be executed on-time */
      OSSleep(1);
    }
  }
}

int main(void) {
  static struct gb_s gb;
  static struct priv_s priv = {0};

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
    snprintf(message, sizeof(message), "Unknown error: %d", gb_ret);
    ConvStrToUnicode(message, messagew, MB_ENCODING_UTF8);
    MessageBox(messagew, MB_DEFAULT);
    free(priv.rom);
    priv.rom = NULL;
    return 1;
  }
  }

  if (gb_get_save_size(&gb) != 0) {
    priv.cart_ram = _read_file(priv.save_file_name, gb_get_save_size(&gb), true);
    if (priv.cart_ram == NULL) {
      free(priv.rom);
      priv.rom = NULL;
      return 1;
    }
  }

  switch (lcd->surface->depth) {
  case LCD_SURFACE_PIXFMT_XRGB:
    priv.fb = lcd->surface;
    gb_init_lcd(&gb, &lcd_draw_line_fast_xrgb);
    break;

  /* TODO: 4-bit LCD machines don't have a hardware-backed framebuffer and
     we need to blit a 160x1 buffer to the screen line-by-line, either
     somehow using the internal lcd write function or calling _BitBlt(),
     to be able to take advantage of e.g. interlaced rendering.
     Disabling this for now. */
  /*
  case LCD_SURFACE_PIXFMT_L4:
    priv.fb = lcd->surface;
    gb_init_lcd(&gb, &lcd_draw_line_fast_p4);
    break;
  */

  default: {
    priv.fallback_blit = true;
    priv.fb = (lcd_surface_t *) calloc(GetImageSizeExt(LCD_WIDTH, LCD_HEIGHT, LCD_SURFACE_PIXFMT_L4), 1);
    priv.real_fb = lcd->surface;
    InitGraphic(priv.fb, LCD_WIDTH, LCD_HEIGHT, LCD_SURFACE_PIXFMT_L4);
    memcpy(priv.fb->palette, PALETTE_P4, sizeof(PALETTE_P4));
    gb_init_lcd(&gb, &lcd_draw_line_safe);
  }
  }

  _set_blit_parameter(&gb, lcd->surface);
  _precompute_yoff(&gb);

  _direct_input_sim_begin(&gb);
  loop(&gb);
  _direct_input_sim_end(&gb);

  _write_save(&gb, priv.save_file_name);

  free(priv.rom);
  if (priv.cart_ram != NULL) {
    free(priv.cart_ram);
  }
  if (priv.fallback_blit) {
    free(priv.fb);
  }
  return 0;
}
