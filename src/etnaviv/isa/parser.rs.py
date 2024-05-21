#!/usr/bin/env python3
#
# Copyright © 2024 Igalia S.L.
# SPDX-License-Identifier: MIT

import argparse
import os
from mako.template import Template

template = """// Copyright © 2024 Igalia S.L.
// SPDX-License-Identifier: MIT

use pest::Parser;

#[derive(pest_derive::Parser)]
#[grammar = "${pest}"]
pub struct IsaParser;

% for include in includes:
include!("${include}");
% endfor

use std::ffi::CStr;
use std::os::raw::c_char;

#[no_mangle]
pub extern "C" fn isa_parse_str(
    c_str: *const c_char,
    instruction_ptr: *mut etna_inst,
    dual_16_mode: bool,
) -> bool {
    // Ensure the provided pointers are not null
    if c_str.is_null() {
        return false;
    }

    if instruction_ptr.is_null() {
        return false;
    }

    // Convert the C string to a Rust string
    let c_str_safe = unsafe {
        CStr::from_ptr(c_str)
    };

    let instruction = unsafe { &mut *instruction_ptr };

    // Convert CStr to a Rust String
    match c_str_safe.to_str() {
        Ok(str) => return parse(str, instruction, dual_16_mode),
        Err(_) => false,
    }
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pest', required=True, type=str, action="store",
                        help='pest grammar file')
    parser.add_argument('includes', metavar='includes', type=str, nargs='+',
                        help='rs files to include')
    parser.add_argument('--output', required=True, type=str, action="store",
                        help='rust sourcce file')
    args = parser.parse_args()

    includes = []
    for include in args.includes:
        if os.path.isabs(include):
            includes.append(include)
        else:
            includes.append(os.path.basename(include))

    with open(args.output, "w", encoding="UTF-8") as fh:
        print(Template(template).render(pest=args.pest, includes=includes), file=fh)


if __name__ == '__main__':
    main()
