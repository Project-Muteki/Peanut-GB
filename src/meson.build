c = meson.get_compiler('c')
mutekix_lib = c.find_library('mutekix', required : true)

elf = executable('Peanut-GB',
  'main.c',
  include_directories : ext_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_lib,
  install : false,
  c_args: ['-DMINIGB_APU_AUDIO_FORMAT_S16SYS'])

if romtool.found() and elf2bestape.found()
  romspec = custom_target('romspec',
    input: 'romspec.toml',
    output: 'romspec.rom',
    command: [romtool, 'build', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: true)

  custom_target('Peanut-GB-bestape',
    input: [elf, romspec],
    output: 'Peanut-GB.exe',
    command: [elf2bestape, '-o', '@OUTPUT@', '-r', '@INPUT1@', '@INPUT0@'],
    build_by_default: true)
endif

elf = executable('Peanut-GBC',
  'main.c',
  include_directories : ext_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_lib,
  install : false,
  c_args: ['-DPEANUT_FULL_GBC_SUPPORT', '-DMANUAL_RTC_NEEDED', '-DLEGACY_APU'],
  build_by_default: false)

if romtool.found() and elf2bestape.found()
  romspec_cgb = custom_target('romspec-cgb',
    input: 'romspec.cgb.toml',
    output: 'romspec.cgb.rom',
    command: [romtool, 'build', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: false)

  custom_target('Peanut-GBC-bestape',
    input: [elf, romspec_cgb],
    output: 'Peanut-GBC.exe',
    command: [elf2bestape, '-o', '@OUTPUT@', '-r', '@INPUT1@', '@INPUT0@'],
    build_by_default: false)
endif