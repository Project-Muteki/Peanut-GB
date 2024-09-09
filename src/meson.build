c = meson.get_compiler('c')
mutekix_lib = c.find_library('mutekix', required : true)

elf = executable('Peanut-GB',
  'main.c',
  include_directories : ext_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_lib,
  install : false,
  c_args: ['-DMANUAL_RTC_NEEDED'])

custom_target('Peanut-GB-bestape',
  input: elf,
  output: 'Peanut-GB.exe',
  command: [elf2bestape, '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true)

elf = executable('Peanut-GBC',
  'main.c',
  include_directories : ext_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_lib,
  install : false,
  c_args: ['-DPEANUT_FULL_GBC_SUPPORT', '-DMANUAL_RTC_NEEDED'])

custom_target('Peanut-GBC-bestape',
  input: elf,
  output: 'Peanut-GBC.exe',
  command: [elf2bestape, '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: false)
