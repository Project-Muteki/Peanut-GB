project('Peanut-GB', 'c',
  version : '1.1.0',
  default_options : ['warning_level=3'])

elf2bestape = find_program('elf2bestape')

add_global_arguments('-DENABLE_SOUND=1', language : 'c')
#add_global_arguments('-DAUDIO_SAMPLE_RATE=44100', language : 'c')

subdir('ext')
subdir('src')
