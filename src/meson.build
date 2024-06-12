c = meson.get_compiler('c')
mutekix_lib = c.find_library('mutekix', required : true)

elf = executable('Peanut-GB',
  'main.c',
  include_directories : ext_include,
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  link_with : ext_lib,
  install : false)

custom_target('Peanut-GB-bestape',
  input: elf,
  output: 'Peanut-GB.exe',
  command: [elf2bestape, '-o', '@OUTPUT@', '@INPUT@'],
  build_by_default: true)
