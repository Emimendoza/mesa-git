/*
 * Copyright © 2020 Valve Corporation
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
 *
 */
#include "helpers.h"

using namespace aco;

BEGIN_TEST(optimize.neg)
   for (unsigned i = GFX9; i <= GFX10; i++) {
      //>> v1: %a, v1: %b, s1: %c, s1: %d, s2: %_:exec = p_startpgm
      if (!setup_cs("v1 v1 s1 s1", (chip_class)i))
         continue;

      //! v1: %res0 = v_mul_f32 %a, -%b
      //! p_unit_test 0, %res0
      Temp neg_b = bld.vop2(aco_opcode::v_xor_b32, bld.def(v1), Operand(0x80000000u), inputs[1]);
      writeout(0, bld.vop2(aco_opcode::v_mul_f32, bld.def(v1), inputs[0], neg_b));

      //! v1: %neg_a = v_xor_b32 0x80000000, %a
      //~gfx[6-9]! v1: %res1 = v_mul_f32 0x123456, %neg_a
      //~gfx10! v1: %res1 = v_mul_f32 0x123456, -%a
      //! p_unit_test 1, %res1
      Temp neg_a = bld.vop2(aco_opcode::v_xor_b32, bld.def(v1), Operand(0x80000000u), inputs[0]);
      writeout(1, bld.vop2(aco_opcode::v_mul_f32, bld.def(v1), Operand(0x123456u), neg_a));

      //! v1: %res2 = v_mul_f32 %a, %b
      //! p_unit_test 2, %res2
      Temp neg_neg_a = bld.vop2(aco_opcode::v_xor_b32, bld.def(v1), Operand(0x80000000u), neg_a);
      writeout(2, bld.vop2(aco_opcode::v_mul_f32, bld.def(v1), neg_neg_a, inputs[1]));

      /* we could optimize this case into just an abs(), but NIR already does this */
      //! v1: %res3 = v_mul_f32 |%neg_a|, %b
      //! p_unit_test 3, %res3
      Temp abs_neg_a = bld.vop2(aco_opcode::v_and_b32, bld.def(v1), Operand(0x7FFFFFFFu), neg_a);
      writeout(3, bld.vop2(aco_opcode::v_mul_f32, bld.def(v1), abs_neg_a, inputs[1]));

      //! v1: %res4 = v_mul_f32 -|%a|, %b
      //! p_unit_test 4, %res4
      Temp abs_a = bld.vop2(aco_opcode::v_and_b32, bld.def(v1), Operand(0x7FFFFFFFu), inputs[0]);
      Temp neg_abs_a = bld.vop2(aco_opcode::v_xor_b32, bld.def(v1), Operand(0x80000000u), abs_a);
      writeout(4, bld.vop2(aco_opcode::v_mul_f32, bld.def(v1), neg_abs_a, inputs[1]));

      //! v1: %res5 = v_mul_f32 -%a, %b row_shl:1 bound_ctrl:1
      //! p_unit_test 5, %res5
      writeout(5, bld.vop2_dpp(aco_opcode::v_mul_f32, bld.def(v1), neg_a, inputs[1], dpp_row_sl(1)));

      //! v1: %res6 = v_subrev_f32 %a, %b
      //! p_unit_test 6, %res6
      writeout(6, bld.vop2(aco_opcode::v_add_f32, bld.def(v1), neg_a, inputs[1]));

      //! v1: %res7 = v_sub_f32 %b, %a
      //! p_unit_test 7, %res7
      writeout(7, bld.vop2(aco_opcode::v_add_f32, bld.def(v1), inputs[1], neg_a));

      //! v1: %res8 = v_mul_f32 %a, -%c
      //! p_unit_test 8, %res8
      Temp neg_c = bld.vop2(aco_opcode::v_xor_b32, bld.def(v1), Operand(0x80000000u), bld.copy(bld.def(v1), inputs[2]));
      writeout(8, bld.vop2(aco_opcode::v_mul_f32, bld.def(v1), inputs[0], neg_c));

      finish_opt_test();
   }
END_TEST

Temp create_mad_u32_u16(Operand a, Operand b, Operand c)
{
   VOP3A_instruction *mad =
      static_cast<VOP3A_instruction *>(bld.vop3(aco_opcode::v_mad_u32_u16,
                                       bld.def(v1), a, b, c).instr);
   return mad->definitions[0].getTemp();
}

BEGIN_TEST(optimize.mad_u32_u16)
   for (unsigned i = GFX9; i <= GFX10; i++) {
      //>> v1: %a, v1: %b, s1: %c, s2: %_:exec = p_startpgm
      if (!setup_cs("v1 v1 s1", (chip_class)i))
         continue;

      //! v1: %res0 = v_mul_u32_u24 %a, %b
      //! p_unit_test 0, %res0
      writeout(0, create_mad_u32_u16(Operand(inputs[0]), Operand(inputs[1]), Operand(0u)));

      //! v1: %res1 = v_mul_u32_u24 42, %a
      //! p_unit_test 1, %res1
      writeout(1, create_mad_u32_u16(Operand(42u), Operand(inputs[0]), Operand(0u)));

      //! v1: %res2 = v_mul_u32_u24 42, %a
      //! p_unit_test 2, %res2
      writeout(2, create_mad_u32_u16(Operand(inputs[0]), Operand(42u), Operand(0u)));

      //! v1: %res3 = v_mul_u32_u24 %c, %a
      //! p_unit_test 3, %res3
      writeout(3, create_mad_u32_u16(Operand(inputs[2]), Operand(inputs[0]), Operand(0u)));

      //! v1: %res4 = v_mul_u32_u24 42, %c
      //! p_unit_test 4, %res4
      writeout(4, create_mad_u32_u16(Operand(42u), Operand(inputs[2]), Operand(0u)));

      //~gfx9! v1: %res5 = v_mad_u32_u16 %a, %b, %b
      //~gfx10! v1: %mul = v_mul_lo_u16_e64 %a, %b
      //~gfx10! v1: %res5 = v_add_u32 %mul, %b
      //! p_unit_test 5, %res5
      Temp mul;
      if (i >= GFX10) {
         mul = bld.vop3(aco_opcode::v_mul_lo_u16_e64, bld.def(v1), inputs[0], inputs[1]);
      } else {
         mul = bld.vop2(aco_opcode::v_mul_lo_u16, bld.def(v1), inputs[0], inputs[1]);
      }
      writeout(5, bld.vadd32(bld.def(v1), mul, inputs[1]));

      finish_opt_test();
   }
END_TEST
