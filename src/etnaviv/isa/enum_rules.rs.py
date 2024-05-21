#!/usr/bin/env python3
#
# Copyright © 2024 Igalia S.L.
# SPDX-License-Identifier: MIT

import argparse
import sys
import os

sys.path.append(os.path.dirname(os.path.abspath(__file__)) + "/../../compiler/isaspec")

from mako.template import Template
from isa import ISA

template = """
// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

% for name, rules in enum_rules:
impl ${name} {
    fn from_rule(rule: Rule) -> Self {
        match rule {
% for rule in rules:
            ${rule}
% endfor
            _ => panic!("Unexpected rule: {:?}", rule),
        }
    }
}

% endfor
"""


def to_upper_camel_case(s, rep_underscore):
    # Remove some bad chars
    s = s.replace('.', '').replace('[', '').replace(']', '')

    if rep_underscore:
        s = s.replace('_', ' ')

    # Capitalize the first letter of each word and join them without spaces
    words = s.split()
    return ''.join(word.capitalize() for word in words)


def to_enum_value(name, value):
    enum_value = f'{name}_{value}'.upper()

    return enum_value.replace('.', '').replace('[', '').replace(']', '')


def generate_enum_definitions(isa):
    opcodes_rules = []

    # Generate OpCode enum and rules
    for _, opc in enumerate(isa.instructions(), start=1):
        value_name = to_upper_camel_case(opc.get_c_name(), True)
        enum_value = to_enum_value('isa_opc', opc.get_c_name())
        opcodes_rules.append(f"Rule::Opc{value_name} => isa_opc::{enum_value},")

    yield 'isa_opc', opcodes_rules

    # Generate enums and rules for other ISA components
    for name, enum in isa.enums.items():

        # TODO: skip some
        if name in {'#swiz'}:
            continue

        formatted_name = f'isa_{name[1:]}'
        rules = []

        for v in enum.values.values():
            if not v.get_name():
                continue

            rule_name = to_upper_camel_case(v.get_name(), False)
            enum_value = to_enum_value(formatted_name, v.get_name())
            rules.append(f"Rule::{rule_name} => {formatted_name}::{enum_value},")

        yield formatted_name, rules


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--xml', required=True, type=str, action="store",
                        help='source isaspec xml file')
    parser.add_argument('--output', required=True, type=str, action="store",
                        help='output rs file')
    args = parser.parse_args()

    isa = ISA(args.xml)
    enum_rules = generate_enum_definitions(isa)

    with open(args.output, "w", encoding="UTF-8") as fh:
        fh.write(Template(template).render(isa=isa, enum_rules=enum_rules))


if __name__ == '__main__':
    main()
