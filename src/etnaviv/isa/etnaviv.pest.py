#!/usr/bin/env python3
#
# Copyright © 2024 Igalia S.L.
# SPDX-License-Identifier: MIT

import argparse
import os
import re
import sys
sys.path.append(os.path.dirname(os.path.abspath(__file__)) + "/../../compiler/isaspec")

from mako.template import Template
from isa import ISA

template = """/*
 * Copyright © 2024 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

WHITESPACE = _{ " " | "\\t" }
COMMENT = _{ ";" ~ (!NEWLINE ~ ANY)* ~ NEWLINE? }

Dst_full = { ".hp" }
Sat = { ".sat" }
Skphp = { ".skpHp" }
Pmode = { ".pack" }
Denorm = { ".denorm" }
Local = { ".local" }

Amount = { ASCII_DIGIT* }
Left_shift = { ".ls" ~ Amount }

SrcVoid = { "void" }
DestVoid = { "void" }
Negate = { "-" }
Absolute = { "|" }

% for name, values in enums.items():
${name} = { ${' | '.join(values.keys())} }
% for k, v in values.items():
    ${k} = { "${v}" }
% endfor

% endfor

Immediate_Minus_Nan = @{ Negate ~ "nan" }
Immediate_float = @{ Negate? ~ ASCII_DIGIT* ~ "." ~ ASCII_DIGIT* }
Immediate_int = @{ Negate ~ ASCII_DIGIT* }
Immediate_uint = @{ ASCII_DIGIT* }
Immediate = _{ Immediate_Minus_Nan | Immediate_float | Immediate_int | Immediate_uint }

Register = { ASCII_DIGIT* }
DstRegister = ${"${"} RegGroup ~ Register ~ RegAddressingMode? ~ Wrmask? }
DstMemAddr = ${"${"} "mem" ~ Wrmask? }
SrcSwizzle = ${"${"} "." ~ Swiz ~ Swiz ~ Swiz ~ Swiz }
SrcRegister = ${"${"}
    ( Negate? ~ Absolute ~ RegGroup ~ Register ~ RegAddressingMode? ~ SrcSwizzle? ~ Absolute |
      Negate? ~ RegGroup ~ Register ~ RegAddressingMode? ~ SrcSwizzle? |
      Immediate )
    }

Dest = _{ DstRegister }
Src = _{ SrcRegister }
TexSrc = ${"${"} "tex" ~ Register ~ SrcSwizzle }
Target = { ASCII_DIGIT* }

instructions = _{ ${' | '.join(alu_instructions)} }
% for k, v in alu_instructions.items():
    ${k} = ${v}
% endfor

instruction = _{ SOI ~ instructions ~ NEWLINE? ~ EOI }
"""


def to_upper_camel_case(s, rep_underscore):
    # Remove some bad chars
    s = s.replace('.', '').replace('[', '').replace(']', '')

    if rep_underscore:
        s = s.replace('_', ' ')

    # Capitalize the first letter of each word and join them without spaces
    words = s.split()
    return ''.join(word.capitalize() for word in words)


def generate_enums(isa):
    enums = {}

    for name, enum in isa.enums.items():
        formatted_name = to_upper_camel_case(name[1:], True)
        # From the pest docs:
        # The choice operator, written as a vertical line |, is ordered. The PEG
        # expression first | second means "try first; but if it fails, try second instead".
        #
        # We need to sort our enum to be able to parse eg th1.xxxx and t1.xxxx
        sorted_enum_values = sorted(
            enum.values.items(),
            key=lambda item: item[1].get_displayname().lower() if item[1].get_displayname() is not None else "",
            reverse=True,
        )

        values = {to_upper_camel_case(v.get_name(), False): v.get_displayname() for _, v in sorted_enum_values if v.get_name()}
        enums[formatted_name] = values

    return enums


def generate_flags(isa):
    """Function returning dict of pest rules for a given type if instruction."""
    _ELEMENT_RE = re.compile(r'{(.*?)}')
    instruction_flags = {}

    for name, t in isa.templates.items():
        result = _ELEMENT_RE.findall(t.display)
        result.remove('NAME')

        if 'RMODE' in result:
            index = result.index('RMODE')
            result[index] = 'ROUNDING'

        instruction_flags[name] = '~ ' + ' ~ '.join(f'{to_upper_camel_case(elem, False)}?' for elem in result) if result else ''

    return instruction_flags


def generate_alu_instructions(isa):
    instruction_flags = generate_flags(isa)
    alu_instructions = {}

    for opc in isa.instructions():
        meta = opc.get_meta()
        type = meta['type'] if meta else ''
        valid_srcs = meta.get('valid_srcs', '').split('|') if meta else ''
        num_dests = int(meta.get('num_dests', '0')) if meta else 0

        flag_name = f'INSTR_{type.upper()}' if meta else ''
        if flag_name in instruction_flags:
            rule_parts = [f'"{opc.name}" ', instruction_flags[flag_name]]
        else:
            rule_parts = [f'"{opc.name}" ']

        if type == "load_store":
            dest_part = ' ~ DestVoid ~ "," ~ ' if num_dests == 0 else ' ~ (Dest | DstMemAddr) ~ "," ~ '
        else:
            dest_part = ' ~ DestVoid ~ "," ~ ' if num_dests == 0 else ' ~ Dest ~ "," ~ '

        rule_parts.append(dest_part)

        if type == 'tex':
            rule_parts.append('TexSrc ~ "," ~ ')

        possible_srcs = range(3)

        if type == 'cf':
            possible_srcs = range(2)


        valid_srcs = [int(num) for num in valid_srcs if num]
        for src in possible_srcs:
            rule_parts.append('Src ~ "," ~ ' if src in valid_srcs else 'SrcVoid ~ "," ~ ')

        if type == 'cf':
            rule_parts.append('Target ~ "," ~ ')

        rule = ''.join(rule_parts).rstrip(' ~ "," ~ ')
        alu_instructions[f'Opc{to_upper_camel_case(opc.get_c_name(), True)}'] = f'{{ {rule} }}'

    return alu_instructions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--xml', required=True, type=str, action="store",
                        help='source isaspec xml file')
    parser.add_argument('--output', required=True, type=str, action="store",
                        help='output C header file')
    args = parser.parse_args()

    isa = ISA(args.xml)
    enums = generate_enums(isa)
    alu_instructions = generate_alu_instructions(isa)

    with open(args.output, "w", encoding="UTF-8") as fh:
        fh.write(Template(template).render(enums=enums, alu_instructions=alu_instructions))


if __name__ == '__main__':
    main()
