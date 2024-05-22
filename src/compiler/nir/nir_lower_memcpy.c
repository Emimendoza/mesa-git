/*
 * Copyright © 2020 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nir_builder.h"

#include <string.h>

/** Returns the type to use for a copy of the given size.
 *
 * The actual type doesn't matter here all that much as we're just going to do
 * a load/store on it and never any arithmetic.
 */
static const struct glsl_type *
copy_type_for_byte_size(unsigned size)
{
   switch (size) {
   case 1:
      return glsl_vector_type(GLSL_TYPE_UINT8, 1);
   case 2:
      return glsl_vector_type(GLSL_TYPE_UINT16, 1);
   case 4:
      return glsl_vector_type(GLSL_TYPE_UINT, 1);
   case 8:
      return glsl_vector_type(GLSL_TYPE_UINT, 2);
   case 16:
      return glsl_vector_type(GLSL_TYPE_UINT, 4);
   default:
      unreachable("Unsupported size");
   }
}

static nir_deref_instr *
deref_offset_cast(nir_builder *b, nir_deref_instr *p,
                  nir_def *offset, unsigned offset_align,
                  const struct glsl_type *type)
{
   offset = nir_u2uN(b, offset, p->def.bit_size);

   nir_deref_instr *p_u8 =
      nir_build_deref_cast(b, &p->def, p->modes, glsl_uint8_t_type(), 1);

   nir_deref_instr *p_off_u8 =
      nir_build_deref_ptr_as_array(b, p_u8, offset);

   nir_deref_instr *p_off_t =
      nir_build_deref_cast(b, &p_off_u8->def, p->modes, type, 0);

   uint32_t align_mul, align_offset;
   if (nir_get_explicit_deref_align(p, true, &align_mul, &align_offset)) {
      p_off_t->cast.align_mul = MIN2(align_mul, offset_align);
      p_off_t->cast.align_offset = align_offset % p_off_t->cast.align_mul;
   }

   return p_off_t;
}

static nir_deref_instr *
deref_offset_cast_imm(nir_builder *b, nir_deref_instr *p,
                      uint64_t offset, const struct glsl_type *type)
{
   unsigned offset_align = offset ? (1 << (ffsll(offset) - 1)) : (1 << 16);
   nir_def *off = nir_imm_intN_t(b, offset, p->def.bit_size);
   return deref_offset_cast(b, p, off, offset_align, type);
}

static bool
lower_memcpy_instr(nir_builder *b, nir_instr *instr, void *data)
{
   const uint64_t max_unroll_size = 256;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *cpy = nir_instr_as_intrinsic(instr);
   if (cpy->intrinsic != nir_intrinsic_memcpy_deref)
      return false;

   b->cursor = nir_instr_remove(&cpy->instr);

   nir_deref_instr *dst = nir_src_as_deref(cpy->src[0]);
   nir_deref_instr *src = nir_src_as_deref(cpy->src[1]);
   if (nir_src_is_const(cpy->src[2]) &&
       nir_src_as_uint(cpy->src[2]) <= max_unroll_size) {
      uint64_t size = nir_src_as_uint(cpy->src[2]);
      uint64_t offset = 0;
      while (offset < size) {
         uint64_t remaining = size - offset;
         /* Find the largest chunk size power-of-two (MSB in remaining)
          * and limit our chunk to 16B (a vec4). It's important to do as
          * many 16B chunks as possible first so that the index
          * computation is correct for
          * memcpy_(load|store)_deref_elem_imm.
          */
         unsigned copy_size = 1u << MIN2(util_last_bit64(remaining) - 1, 4);
         const struct glsl_type *copy_type =
            copy_type_for_byte_size(copy_size);

         nir_deref_instr *dst_off =
            deref_offset_cast_imm(b, dst, offset, copy_type);
         nir_deref_instr *src_off =
            deref_offset_cast_imm(b, src, offset, copy_type);

         nir_store_deref(b, dst_off, nir_load_deref(b, src_off), 0xf);

         offset += copy_size;
      }
   } else {
      nir_def *size = cpy->src[2].ssa;

      /* In this case, we don't have any idea what the size is so we
       * emit a loop which copies one byte at a time.
       */
      const struct glsl_type *size_type = glsl_uintN_t_type(size->bit_size);
      nir_variable *pos = nir_local_variable_create(b->impl, size_type, NULL);
      nir_store_var(b, pos, nir_imm_intN_t(b, 0, size->bit_size), ~0);

      /* Byte loops are slow.  Start off copying whole vec4s */
      uint8_t max_copy_size = 16;
      nir_def *mcs_1 = nir_imm_intN_t(b, max_copy_size - 1, size->bit_size);
      nir_def *end = nir_usub_sat(b, size, mcs_1);
      nir_push_loop(b);
      {
         nir_def *p = nir_load_var(b, pos);
         nir_push_if(b, nir_uge(b, p, end));
         {
            nir_jump(b, nir_jump_break);
         }
         nir_pop_if(b, NULL);

         const struct glsl_type *copy_type =
            copy_type_for_byte_size(max_copy_size);
         nir_deref_instr *dst_off =
            deref_offset_cast(b, dst, p, max_copy_size, copy_type);
         nir_deref_instr *src_off =
            deref_offset_cast(b, src, p, max_copy_size, copy_type);

         nir_store_deref(b, dst_off, nir_load_deref(b, src_off), 0xf);

         nir_store_var(b, pos, nir_iadd_imm(b, p, max_copy_size), ~0);
      }
      nir_pop_loop(b, NULL);

      for (uint8_t copy_size = max_copy_size >> 1;
           copy_size > 0; copy_size >>= 1) {
         nir_def *p = nir_load_var(b, pos);
         nir_push_if(b, nir_uge_imm(b, nir_isub(b, size, p), copy_size));
         {
            const struct glsl_type *copy_type =
               copy_type_for_byte_size(copy_size);
            nir_deref_instr *dst_off =
               deref_offset_cast(b, dst, p, copy_size, copy_type);
            nir_deref_instr *src_off =
               deref_offset_cast(b, src, p, copy_size, copy_type);

            nir_store_deref(b, dst_off, nir_load_deref(b, src_off), 0xf);

            nir_store_var(b, pos, nir_iadd_imm(b, p, copy_size), ~0);
         }
         nir_pop_if(b, NULL);
      }
   }

   return true;
}

bool
nir_lower_memcpy(nir_shader *shader)
{
   return nir_shader_instructions_pass(shader, lower_memcpy_instr,
                                       nir_metadata_none, NULL);
}
