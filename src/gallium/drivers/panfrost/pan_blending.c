/*
 * © Copyright 2018 Alyssa Rosenzweig
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <stdio.h>
#include "pan_blending.h"
#include "pan_context.h"
#include "gallium/auxiliary/util/u_blend.h"
#include "util/format/u_format.h"

/* Implements fixed-function blending on Midgard. */

/* Not all formats can be blended by fixed-function hardware */

bool
panfrost_can_fixed_blend(enum pipe_format format)
{
        /* Fixed-function can handle sRGB */
        format = util_format_linear(format);

        switch (panfrost_pipe_format_table[format].hw) {
        case MALI_RGB565:
        case MALI_RGB5_A1_UNORM:
        case MALI_RGB10_A2_UNORM:
        case MALI_RGBA4_UNORM:
        case MALI_R8_UNORM:
        case MALI_RG8_UNORM:
        case MALI_RGB8_UNORM:
        case MALI_RGBA8_UNORM:
                return true;
        default:
                return false;
        }
}

/* Helper to find the uncomplemented Gallium blend factor corresponding to a
 * complemented Gallium blend factor */

static int
complement_factor(int factor)
{
        switch (factor) {
        case PIPE_BLENDFACTOR_INV_SRC_COLOR:
                return PIPE_BLENDFACTOR_SRC_COLOR;

        case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
                return PIPE_BLENDFACTOR_SRC_ALPHA;

        case PIPE_BLENDFACTOR_INV_DST_ALPHA:
                return PIPE_BLENDFACTOR_DST_ALPHA;

        case PIPE_BLENDFACTOR_INV_DST_COLOR:
                return PIPE_BLENDFACTOR_DST_COLOR;

        case PIPE_BLENDFACTOR_INV_CONST_COLOR:
                return PIPE_BLENDFACTOR_CONST_COLOR;

        case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
                return PIPE_BLENDFACTOR_CONST_ALPHA;

        default:
                return -1;
        }
}

/* Helper to strip the complement from any Gallium blend factor */

static int
uncomplement_factor(int factor)
{
        int complement = complement_factor(factor);
        return (complement == -1) ? factor : complement;
}

/* Check if this is a special edge case blend factor, which may require the use
 * of clip modifiers */

static bool
is_edge_blendfactor(unsigned factor)
{
        return factor == PIPE_BLENDFACTOR_ONE || factor == PIPE_BLENDFACTOR_ZERO;
}

static bool
factor_is_supported(unsigned factor)
{
        return factor != PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE &&
               factor != PIPE_BLENDFACTOR_SRC1_COLOR &&
               factor != PIPE_BLENDFACTOR_SRC1_ALPHA &&
               factor != PIPE_BLENDFACTOR_INV_SRC1_COLOR &&
               factor != PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
}

static bool
can_use_fixed_function_blend(unsigned blend_func,
                             unsigned src_factor,
                             unsigned dest_factor)
{
        if (blend_func != PIPE_BLEND_ADD &&
            blend_func != PIPE_BLEND_SUBTRACT &&
            blend_func != PIPE_BLEND_REVERSE_SUBTRACT)
                return false;

        if (!factor_is_supported(src_factor) ||
            !factor_is_supported(dest_factor))
                return false;

        if (src_factor != dest_factor &&
            src_factor != complement_factor(dest_factor) &&
            complement_factor(src_factor) != dest_factor &&
            !is_edge_blendfactor(src_factor) &&
            !is_edge_blendfactor(dest_factor))
                return false;

        return true;
}

static void to_c_factor(unsigned factor, struct MALI_BLEND_FUNCTION *function)
{
        if (complement_factor(factor) >= 0)
                function->invert_c = true;

        switch (uncomplement_factor(factor)) {
        case PIPE_BLENDFACTOR_ONE:
        case PIPE_BLENDFACTOR_ZERO:
                function->invert_c = factor == PIPE_BLENDFACTOR_ONE;
                function->c = MALI_BLEND_OPERAND_C_ZERO;
                break;

        case PIPE_BLENDFACTOR_SRC_ALPHA:
                function->c = MALI_BLEND_OPERAND_C_SRC_ALPHA;
                break;

        case PIPE_BLENDFACTOR_DST_ALPHA:
                function->c = MALI_BLEND_OPERAND_C_DEST_ALPHA;
                break;

        case PIPE_BLENDFACTOR_SRC_COLOR:
                function->c = MALI_BLEND_OPERAND_C_SRC;
                break;

        case PIPE_BLENDFACTOR_DST_COLOR:
                function->c = MALI_BLEND_OPERAND_C_DEST;
                break;

        case PIPE_BLENDFACTOR_CONST_COLOR:
        case PIPE_BLENDFACTOR_CONST_ALPHA:
                function->c = MALI_BLEND_OPERAND_C_CONSTANT;
                break;
        default:
                unreachable("Invalid blend factor");
        }

}

static bool
to_panfrost_function(unsigned blend_func,
                     unsigned src_factor,
                     unsigned dest_factor,
                     struct MALI_BLEND_FUNCTION *function)
{
        if (!can_use_fixed_function_blend(blend_func, src_factor, dest_factor))
                return false;

        if (src_factor == PIPE_BLENDFACTOR_ZERO) {
                function->a = MALI_BLEND_OPERAND_A_ZERO;
                function->b = MALI_BLEND_OPERAND_B_DEST;
                if (blend_func == PIPE_BLEND_SUBTRACT)
                        function->negate_b = true;
                to_c_factor(dest_factor, function);
        } else if (src_factor == PIPE_BLENDFACTOR_ONE) {
                function->a = MALI_BLEND_OPERAND_A_SRC;
                function->b = MALI_BLEND_OPERAND_B_DEST;
                if (blend_func == PIPE_BLEND_SUBTRACT)
                        function->negate_b = true;
                else if (blend_func == PIPE_BLEND_REVERSE_SUBTRACT)
                        function->negate_a = true;
                to_c_factor(dest_factor, function);
        } else if (dest_factor == PIPE_BLENDFACTOR_ZERO) {
                function->a = MALI_BLEND_OPERAND_A_ZERO;
                function->b = MALI_BLEND_OPERAND_B_SRC;
                if (blend_func == PIPE_BLEND_REVERSE_SUBTRACT)
                        function->negate_b = true;
                to_c_factor(src_factor, function);
        } else if (dest_factor == PIPE_BLENDFACTOR_ONE) {
                function->a = MALI_BLEND_OPERAND_A_DEST;
                function->b = MALI_BLEND_OPERAND_B_SRC;
                if (blend_func == PIPE_BLEND_SUBTRACT)
                        function->negate_a = true;
                else if (blend_func == PIPE_BLEND_REVERSE_SUBTRACT)
                        function->negate_b = true;
                to_c_factor(src_factor, function);
        } else if (src_factor == dest_factor) {
                function->a = MALI_BLEND_OPERAND_A_ZERO;
                to_c_factor(src_factor, function);

                switch (blend_func) {
                case PIPE_BLEND_ADD:
                        function->b = MALI_BLEND_OPERAND_B_SRC_PLUS_DEST;
                        break;
                case PIPE_BLEND_REVERSE_SUBTRACT:
                        function->negate_b = true;
                        /* fall-through */
                case PIPE_BLEND_SUBTRACT:
                        function->b = MALI_BLEND_OPERAND_B_SRC_MINUS_DEST;
                        break;
                default:
                        unreachable("Invalid blend function");
                }
        } else {
                assert(src_factor == complement_factor(dest_factor) ||
                       complement_factor(src_factor) == dest_factor);

                function->a = MALI_BLEND_OPERAND_A_DEST;
                to_c_factor(src_factor, function);

                switch (blend_func) {
                case PIPE_BLEND_ADD:
                        function->b = MALI_BLEND_OPERAND_B_SRC_MINUS_DEST;
                        break;
                case PIPE_BLEND_REVERSE_SUBTRACT:
                        function->b = MALI_BLEND_OPERAND_B_SRC_PLUS_DEST;
                        function->negate_b = true;
                        break;
                case PIPE_BLEND_SUBTRACT:
                        function->b = MALI_BLEND_OPERAND_B_SRC_PLUS_DEST;
                        function->negate_a = true;
                        break;
                }
        }

        return true;
}

/* We can upload a single constant for all of the factors. So, scan
 * the factors for constants used to create a mask to check later. */

static unsigned
panfrost_constant_mask(unsigned *factors, unsigned num_factors)
{
        unsigned mask = 0;

        for (unsigned i = 0; i < num_factors; ++i) {
                unsigned factor = uncomplement_factor(factors[i]);

                if (factor == PIPE_BLENDFACTOR_CONST_COLOR)
                        mask |= 0b0111; /* RGB */
                else if (factor == PIPE_BLENDFACTOR_CONST_ALPHA)
                        mask |= 0b1000; /* A */
        }

        return mask;
}

/* Create the descriptor for a fixed blend mode given the corresponding Gallium
 * state, if possible. Return true and write out the blend descriptor into
 * blend_equation. If it is not possible with the fixed function
 * representating, return false to handle degenerate cases with a blend shader
 */

bool
panfrost_make_fixed_blend_mode(const struct pipe_rt_blend_state blend,
                               struct MALI_BLEND_EQUATION *equation,
                               unsigned *constant_mask)
{
        /* If no blending is enabled, default back on `replace` mode */

        if (!blend.blend_enable) {
                equation->color_mask = blend.colormask;
                equation->rgb.a = MALI_BLEND_OPERAND_A_SRC;
                equation->rgb.b = MALI_BLEND_OPERAND_B_SRC;
                equation->rgb.c = MALI_BLEND_OPERAND_C_ZERO;
                equation->alpha.a = MALI_BLEND_OPERAND_A_SRC;
                equation->alpha.b = MALI_BLEND_OPERAND_B_SRC;
                equation->alpha.c = MALI_BLEND_OPERAND_C_ZERO;
                return true;
        }

        /* At draw-time, we'll need to analyze the blend constant, so
         * precompute a mask for it -- even if we don't end up able to use
         * fixed-function blending */

        unsigned factors[] = {
                blend.rgb_src_factor, blend.rgb_dst_factor,
                blend.alpha_src_factor, blend.alpha_dst_factor,
        };

        *constant_mask = panfrost_constant_mask(factors, ARRAY_SIZE(factors));

        /* Try to compile the actual fixed-function blend */
        if (!to_panfrost_function(blend.rgb_func, blend.rgb_src_factor,
                                  blend.rgb_dst_factor,
                                  &equation->rgb))
                return false;

        if (!to_panfrost_function(blend.alpha_func, blend.alpha_src_factor,
                                  blend.alpha_dst_factor,
                                  &equation->alpha))
                return false;

        equation->color_mask = blend.colormask;
        return true;
}
