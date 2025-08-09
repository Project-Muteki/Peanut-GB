c = meson.get_compiler('c')
mutekix_lib = c.find_library('mutekix', required : true)

wb = executable('wb',
  'main.c',
  include_directories : ext_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_lib,
  install : false,
  c_args: ['-DMINIGB_APU_AUDIO_FORMAT_S16SYS'],
  link_args: ['-Wl,--gc-sections'])

wbc = executable('wbc',
  'main.c',
  include_directories : ext_cgb_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_cgb_lib,
  install : false,
  c_args: ['-DPEANUT_FULL_GBC_SUPPORT=1', '-DMINIGB_APU_AUDIO_FORMAT_S16SYS'],
  link_args: ['-Wl,--gc-sections'])

if romtool.found() and elf2bestape.found()
  romspec = custom_target('romspec',
    input: 'romspec.toml',
    output: 'romspec.rom',
    command: [romtool, 'build', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: true)

  romspec_cgb = custom_target('romspec-cgb',
    input: 'romspec.cgb.toml',
    output: 'romspec.cgb.rom',
    command: [romtool, 'build', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: true)

  custom_target('wb-bestape',
    input: [wb, romspec],
    output: 'wb.exe',
    command: [elf2bestape, '-o', '@OUTPUT@', '-r', '@INPUT1@', '@INPUT0@'],
    build_by_default: true)

  custom_target('wbc-bestape',
    input: [wbc, romspec_cgb],
    output: 'wbc.exe',
    command: [elf2bestape, '-o', '@OUTPUT@', '-r', '@INPUT1@', '@INPUT0@'],
    build_by_default: true)
endif
