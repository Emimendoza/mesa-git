# Copyright © 2021 Collabora, Ltd.
# Author: Antonio Caggiano <antonio.caggiano@collabora.com>
#
# SPDX-License-Identifier: MIT

if with_perfetto and dep_libdrm.found()
  subdir('pps')
endif

if with_tools.contains('dlclose-skip')
  subdir('dlclose-skip')
endif

# Mesa Debug Archive tool.
if with_tools.contains('mda')
  mda_tool = executable(
    'mda',
    files('mda.c'),
    dependencies : [
      idep_mesautil,
    ],
    include_directories : [inc_include, inc_src, inc_intel],
    c_args : [no_override_init_args],
    gnu_symbol_visibility : 'hidden',
    install : true
  )
endif
