ext_minigb_apu_include = include_directories('Peanut-GB/examples/sdl2/minigb_apu')
ext_peanut_gb_include = include_directories('Peanut-GB')
ext_include = [ext_minigb_apu_include, ext_peanut_gb_include]

ext_lib = static_library('ext',
  'Peanut-GB/examples/sdl2/minigb_apu/minigb_apu.c',
  include_directories : ext_include,
  pic : false,
  c_args: ['-DMINIGB_APU_AUDIO_FORMAT_S16SYS'],
)
