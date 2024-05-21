#
# Copyright © 2023 Igalia S.L.
# SPDX-License-Identifier: MIT
#

isa_depend_files = [
  'etnaviv.xml',
  isaspec_py_deps,
]

etnaviv_isa = custom_target(
  'etnaviv-isa',
  input: ['etnaviv.xml'],
  output: ['etnaviv-isa.c', 'etnaviv-isa.h'],
  command: [
    prog_isaspec_decode, '--xml', '@INPUT@',
    '--out-c', '@OUTPUT0@', '--out-h', '@OUTPUT1@',
  ],
  depend_files: isa_depend_files,
)

decode_files = [
  etnaviv_isa,
  'isa.h',
]

_libetnaviv_decode = static_library(
  'etnaviv_decode',
  decode_files,
  dependencies: [idep_mesautil, idep_isaspec_decode],
  link_with: [libisaspec],
  include_directories: [
    inc_include,
    inc_src,
  ],
  gnu_symbol_visibility: 'hidden',
)

idep_libetnaviv_decode = declare_dependency(
  include_directories : [
    include_directories('.'),
    inc_include,
    inc_src
  ],
  link_with : _libetnaviv_decode,
  dependencies: [idep_isaspec_decode],
  sources : [etnaviv_isa],
)

prog_enums_h = files('enums.h.py')
enums_h = custom_target(
  'enums.h',
  input: 'etnaviv.xml',
  output: 'enums.h',
  command: [prog_python, prog_enums_h, '--xml', '@INPUT@', '--output', '@OUTPUT@'],
  depend_files: [isa_depend_files, prog_enums_h],
)

encode_h = custom_target(
  'encode.h',
  input: ['etnaviv.xml'],
  output: 'encode.h',
  command: [
    prog_isaspec_encode, '--xml', '@INPUT@', '--out-h', '@OUTPUT@'
  ],
  depend_files: isa_depend_files,
)

encode_files = [
  enums_h,
  encode_h,
  'encode.c',
]

libetnaviv_encode = static_library(
  'etnaviv_encode',
  encode_files,
  gnu_symbol_visibility: 'hidden',
  include_directories: [
    inc_src,
  ],
)

disasm = executable(
  'etnaviv-disasm',
  ['disasm.c'],
  dependencies: idep_libetnaviv_decode,
  build_by_default: with_tools.contains('etnaviv'),
  include_directories: [
    inc_src,
  ],
  install: false,
)

if with_tools.contains('etnaviv')
  if meson.version().version_compare('< 1.4.0')
    error('etnaviv requires meson 1.4.0 or newer')
  endif

  add_languages('rust', required: true)
  rust = import('rust')

  asm_bindings = rust.bindgen(
    input : 'asm.h',
    output : 'asm_bindings.rs',
    include_directories : [inc_src],
    args : [
      '--disable-header-comment',
      '--ignore-functions',
      '--allowlist-type', 'etna_inst',
      '--default-enum-style', 'rust',
      '--with-derive-default',
    ],
  )

  prog_etnaviv_pest = files('etnaviv.pest.py')
  etnaviv_pest = custom_target(
    'etnaviv.pest',
    input: 'etnaviv.xml',
    output: 'etnaviv.pest',
    command: [prog_python, prog_etnaviv_pest, '--xml', '@INPUT@', '--output', '@OUTPUT@'],
    depend_files: [isa_depend_files, prog_etnaviv_pest],
  )

  prog_enum_rules = files('enum_rules.rs.py')
  enum_rules_rs = custom_target(
    'enum_rules.rs',
    input: 'etnaviv.xml',
    output: 'enum_rules.rs',
    command: [prog_python, prog_enum_rules, '--xml', '@INPUT@', '--output', '@OUTPUT@'],
    depend_files: [isa_depend_files, prog_enum_rules],
  )

  prog_parser = files('parser.rs.py')
  parser_rs = custom_target(
    'parser.rs',
    input: [
      etnaviv_pest,
      asm_bindings,
      enum_rules_rs,
      join_paths(meson.current_source_dir(), 'asm.rs'),
      ],
    output: 'parser.rs',
    command: [prog_python, prog_parser, '--pest', '@INPUT0@',  '--output', '@OUTPUT@', '@INPUT1@', '@INPUT2@', '@INPUT3@'],
    depend_files: [isa_depend_files, prog_parser],
  )

  dep_pest_derive = dependency('pest_derive',
    version: '>= 2.7.6',
    fallback: ['pest_derive', 'dep_pest_derive'],
    required: true,
  )

  dep_pest = dependency('pest',
    version: '>= 2.7.6',
    fallback: ['pest', 'dep_pest'],
    required: true,
  )

  libetnaviv_parser = static_library(
    '_assembler_lib',
    [parser_rs],
    gnu_symbol_visibility: 'hidden',
    rust_abi: 'c',
    rust_args: [
      '-Anon_camel_case_types',
    ],
    dependencies: [dep_pest, dep_pest_derive],
  )

  etnaviv_assembler = executable(
    'etnaviv-assembler',
    ['assembler.c'],
    gnu_symbol_visibility: 'hidden',
    dependencies: [ idep_libetnaviv_decode ],
    link_with: [libetnaviv_parser, libetnaviv_encode],
  )

  subdir('tests')
endif
