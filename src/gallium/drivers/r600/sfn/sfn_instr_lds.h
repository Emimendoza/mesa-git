/* -*- mesa-c++  -*-
 * Copyright 2022 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef LDSINSTR_H
#define LDSINSTR_H

#include "sfn_instr_alu.h"
#include "sfn_valuefactory.h"

namespace r600 {

class LDSReadInstr : public Instr {
public:
   LDSReadInstr(std::vector<PRegister, Allocator<PRegister>>& value,
                AluInstr::SrcValues& address);

   unsigned num_values() const { return m_dest_value.size(); }
   auto address(unsigned i) const { return m_address[i]; }
   auto dest(unsigned i) const { return m_dest_value[i]; }

   auto address(unsigned i) { return m_address[i]; }
   auto dest(unsigned i) { return m_dest_value[i]; }

   void accept(ConstInstrVisitor& visitor) const override;
   void accept(InstrVisitor& visitor) override;
   bool replace_dest(PRegister new_dest, AluInstr *move_instr) override;

   AluInstr *split(std::vector<AluInstr *>& out_block, AluInstr *last_lds_instr);

   bool is_equal_to(const LDSReadInstr& lhs) const;

   static auto from_string(std::istream& is, ValueFactory& value_factory) -> Pointer;

   bool remove_unused_components();

private:
   bool do_ready() const override;

   void do_print(std::ostream& os) const override;

   AluInstr::SrcValues m_address;
   std::vector<PRegister, Allocator<PRegister>> m_dest_value;
};

class LDSAtomicInstr : public Instr {
public:
   using SrcValues = AluInstr::SrcValues;

   LDSAtomicInstr(ESDOp op, PRegister dest, PVirtualValue address, const SrcValues& src);

   auto address() const { return m_address; }
   auto dest() const { return m_dest; }
   auto src0() const { return m_srcs[0]; }
   auto src1() const { return m_srcs.size() > 1 ? m_srcs[1] : nullptr; }

   PVirtualValue address() { return m_address; }
   PRegister dest() { return m_dest; }
   PVirtualValue src0() { return m_srcs[0]; }
   PVirtualValue src1() { return m_srcs.size() > 1 ? m_srcs[1] : nullptr; }

   unsigned op() const { return m_opcode; }

   void accept(ConstInstrVisitor& visitor) const override;
   void accept(InstrVisitor& visitor) override;

   AluInstr *split(std::vector<AluInstr *>& out_block, AluInstr *last_lds_instr);
   bool is_equal_to(const LDSAtomicInstr& lhs) const;

   static auto from_string(std::istream& is, ValueFactory& value_factory) -> Pointer;
   bool replace_source(PRegister old_src, PVirtualValue new_src) override;

private:
   bool do_ready() const override;
   void do_print(std::ostream& os) const override;

   ESDOp m_opcode;
   PVirtualValue m_address{nullptr};
   PRegister m_dest{nullptr};
   SrcValues m_srcs;
};

} // namespace r600

#endif // LDSINSTR_H
