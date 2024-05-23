/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_loop_analyze.h"

static bool
src_variance(nir_src *src, void *state)
{
   unsigned *variance_ptr = state;
   *variance_ptr = MAX2(src->ssa->parent_instr->pass_flags, *variance_ptr);
   return true;
}

static bool
is_loop_invariant(nir_instr *instr, unsigned loop_nest_depth)
{
   unsigned variance = 0;
   switch (instr->type) {
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      instr->pass_flags = 0;
      return true;

   case nir_instr_type_intrinsic:
      if (!nir_intrinsic_can_reorder(nir_instr_as_intrinsic(instr))) {
         instr->pass_flags = loop_nest_depth;
         return false;
      }
      FALLTHROUGH;

   case nir_instr_type_alu:
   case nir_instr_type_tex:
   case nir_instr_type_deref:
      nir_foreach_src(instr, src_variance, &variance);
      instr->pass_flags = variance;
      return variance < loop_nest_depth;

   case nir_instr_type_phi:
   case nir_instr_type_call:
   case nir_instr_type_jump:
   default:
      instr->pass_flags = loop_nest_depth;
      return false;
   }
}

static bool
visit_block(nir_block *block, unsigned loop_nest_depth, bool skip)
{
   if (skip) {
      nir_foreach_instr(instr, block)
         instr->pass_flags = loop_nest_depth;
      return false;
   }

   bool progress = false;
   nir_foreach_instr_safe(instr, block) {
      if (is_loop_invariant(instr, loop_nest_depth)) {
         nir_cf_node *cf_node = block->cf_node.parent;
         assert(cf_node->type == nir_cf_node_loop);
         nir_instr_remove(instr);
         instr->pass_flags = loop_nest_depth - 1;
         nir_instr_insert_before_cf(cf_node, instr);
         progress = true;
      }
   }

   return progress;
}

static bool
visit_cf_list(struct exec_list *list, unsigned loop_nest_depth, bool skip)
{
   bool progress = false;

   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         nir_block *block = nir_cf_node_as_block(node);
         progress |= visit_block(block, loop_nest_depth, skip);
         break;
      }
      case nir_cf_node_if: {
         /* Skip nested CF in order to avoid excessive register pressure.
          * As we do not replicate the CF, it could also decrease performance,
          * independently from register pressure changes.
          */
         nir_if *nif = nir_cf_node_as_if(node);
         progress |= visit_cf_list(&nif->then_list, loop_nest_depth, true);
         progress |= visit_cf_list(&nif->else_list, loop_nest_depth, true);

         /* Stop after encountering a break/continue statement as it is
          * generally not safe to speculatively execute arbitrary intrinsics,
          * even if can_reorder is true.
          */
         skip |= contains_other_jump(node, NULL);
         break;
      }
      case nir_cf_node_loop: {
         /* Ignore loops without back-edge */
         nir_loop *loop = nir_cf_node_as_loop(node);
         bool is_loop = loop_nest_depth < UINT8_MAX &&
                        nir_loop_first_block(loop)->predecessors->entries > 1;
         progress |= visit_cf_list(&loop->body, loop_nest_depth + is_loop, !is_loop);
         break;
      }
      case nir_cf_node_function:
         unreachable("NIR LICM: Unsupported cf_node type.");
      }
   }

   return progress;
}

bool
nir_opt_licm(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader) {
      if (visit_cf_list(&impl->body, 0, true)) {
         progress = true;
         nir_metadata_preserve(impl, nir_metadata_block_index |
                                        nir_metadata_dominance);
      } else {
         nir_metadata_preserve(impl, nir_metadata_all);
      }
   }

   return progress;
}
