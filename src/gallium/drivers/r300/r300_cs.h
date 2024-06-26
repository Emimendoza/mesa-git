/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/**
 * This file contains macros for immediate command submission.
 */

#ifndef R300_CS_H
#define R300_CS_H

#include "r300_reg.h"
#include "r300_context.h"

/* Yes, I know macros are ugly. However, they are much prettier than the code
 * that they neatly hide away, and don't have the cost of function setup,so
 * we're going to use them. */

/**
 * Command submission setup.
 */

#define CS_LOCALS(context) \
    struct radeon_cmdbuf *cs_copy = &(context)->cs; \
    struct radeon_winsys *cs_winsys = (context)->rws; \
    int cs_count = 0; (void) cs_count; (void) cs_winsys;

#if MESA_DEBUG

#define BEGIN_CS(size) do { \
    assert(size <= (cs_copy->current.max_dw - cs_copy->current.cdw)); \
    cs_count = size; \
} while (0)

#define END_CS do { \
    if (cs_count != 0) \
        debug_printf("r300: Warning: cs_count off by %d at (%s, %s:%i)\n", \
                     cs_count, __func__, __FILE__, __LINE__); \
    cs_count = 0; \
} while (0)

#define CS_USED_DW(x) cs_count -= (x)

#else

#define BEGIN_CS(size)
#define END_CS
#define CS_USED_DW(x)

#endif

/**
 * Writing pure DWORDs.
 */

#define OUT_CS(value) do { \
    cs_copy->current.buf[cs_copy->current.cdw++] = (value); \
    CS_USED_DW(1); \
} while (0)

#define OUT_CS_32F(value) \
    OUT_CS(fui(value))

#define OUT_CS_REG(register, value) do { \
    OUT_CS(CP_PACKET0(register, 0)); \
    OUT_CS(value); \
} while (0)

/* Note: This expects count to be the number of registers,
 * not the actual packet0 count! */
#define OUT_CS_REG_SEQ(register, count) \
    OUT_CS(CP_PACKET0((register), ((count) - 1)))

#define OUT_CS_ONE_REG(register, count) \
    OUT_CS(CP_PACKET0((register), ((count) - 1)) | RADEON_ONE_REG_WR)

#define OUT_CS_PKT3(op, count) \
    OUT_CS(CP_PACKET3(op, count))

#define OUT_CS_TABLE(values, count) do { \
    memcpy(cs_copy->current.buf + cs_copy->current.cdw, (values), (count) * 4); \
    cs_copy->current.cdw += (count); \
    CS_USED_DW(count); \
} while (0)


/**
 * Writing buffers.
 */

#define OUT_CS_RELOC(r) do { \
    assert((r)); \
    assert((r)->buf); \
    OUT_CS(0xc0001000); /* PKT3_NOP */ \
    OUT_CS(cs_winsys->cs_lookup_buffer(cs_copy, (r)->buf) * 4); \
} while (0)


/**
 * Command buffer emission.
 */

#define WRITE_CS_TABLE(values, count) do { \
    assert(cs_count == 0); \
    memcpy(cs_copy->current.buf + cs_copy->current.cdw, (values), (count) * 4); \
    cs_copy->current.cdw += (count); \
} while (0)

#endif /* R300_CS_H */
