/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#ifndef _ISA_H_
#define _ISA_H_

#include "compiler/isaspec/isaspec.h"

struct etna_inst;

#ifdef __cplusplus
extern "C" {
#endif

void isa_assemble_instruction(uint32_t *out, const struct etna_inst *instr);

extern bool isa_parse_str(const char *str, struct etna_inst *i, bool dual_16_mode);

#ifdef __cplusplus
}
#endif

#endif /* _ISA_H_ */
