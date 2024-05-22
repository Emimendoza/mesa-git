/*
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_constant_expressions.h"

#define NIR_MAX_SUBGROUP_SIZE           128
#define FOTID_MAX_RECURSION_DEPTH 16 /* totally arbitrary */

/* This pass uses constant folding to determine which invocation is read by
 * shuffle for each invocation. Then, it detects patterns in the result and
 * uses more a specialized intrinsic if possible.
 */

static inline unsigned
src_get_fotid_mask(nir_src src)
{
   return src.ssa->parent_instr->pass_flags;
}

static inline unsigned
alu_src_get_fotid_mask(nir_alu_instr *instr, unsigned idx)
{
   unsigned unswizzled = src_get_fotid_mask(instr->src[idx].src);
   unsigned result = 0;
   for (unsigned i = 0; i < nir_ssa_alu_instr_src_components(instr, idx); i++) {
      bool is_fotid = unswizzled & (1u << instr->src[idx].swizzle[i]);
      result |= is_fotid << i;
   }
   return result;
}

static void
update_fotid_alu(nir_builder *b, nir_alu_instr *instr,
                 const nir_opt_tid_function_options *options)
{
   /* For legacy reasons these are ALU instructions
    * when they should be intrinsics.
    */
   switch (instr->op) {
   case nir_op_fddx:
   case nir_op_fddy:
   case nir_op_fddx_fine:
   case nir_op_fddy_fine:
   case nir_op_fddx_coarse:
   case nir_op_fddy_coarse:
      return;
   default:
      break;
   }

   const nir_op_info *info = &nir_op_infos[instr->op];

   unsigned res = BITFIELD_MASK(instr->def.num_components);
   for (unsigned i = 0; i < info->num_inputs; i++) {
      unsigned src_mask = alu_src_get_fotid_mask(instr, i);
      if (info->input_sizes[i] == 0)
         res &= src_mask;
      else if (src_mask != BITFIELD_MASK(info->input_sizes[i]))
         res = 0;
   }

   instr->instr.pass_flags = res;
}

static void
update_fotid_intrinsic(nir_builder *b, nir_intrinsic_instr *instr,
                          const nir_opt_tid_function_options *options)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_load_subgroup_invocation: {
      instr->instr.pass_flags = 1;
      break;
   }
   case nir_intrinsic_load_local_invocation_id: {
      /* TODO: is this always correct or do we need a callback? */
      unsigned partial_size = 1;
      for (unsigned i = 0; i < 3; i++) {
         partial_size *= b->shader->info.workgroup_size[i];
         if (partial_size == options->subgroup_size)
            instr->instr.pass_flags = BITFIELD_MASK(i + 1);
      }
      if (partial_size <= options->subgroup_size)
         instr->instr.pass_flags = 0x7;
      break;
   }
   case nir_intrinsic_load_local_invocation_index: {
      unsigned workgroup_size = b->shader->info.workgroup_size[0] *
                                b->shader->info.workgroup_size[1] *
                                b->shader->info.workgroup_size[2];
      if (workgroup_size <= options->subgroup_size)
         instr->instr.pass_flags = 0x1;
      break;
   }
   case nir_intrinsic_inverse_ballot: {
      if (src_get_fotid_mask(instr->src[0]) ==
          BITFIELD_MASK(instr->src[0].ssa->num_components)) {
         instr->instr.pass_flags = 0x1;
      }
      break;
   }
   default: {
      break;
   }
   }
}

static void
update_fotid_load_const(nir_load_const_instr *instr)
{
   instr->instr.pass_flags = BITFIELD_MASK(instr->def.num_components);
}

static bool
update_fotid_instr(nir_builder *b, nir_instr *instr, void* params)
{
   const nir_opt_tid_function_options *options = params;

   /* Gather a mask of components that are functions of tid. */
   instr->pass_flags = 0;

   switch (instr->type) {
   case nir_instr_type_alu:
      update_fotid_alu(b, nir_instr_as_alu(instr), options);
      break;
   case nir_instr_type_intrinsic:
      update_fotid_intrinsic(b, nir_instr_as_intrinsic(instr), options);
      break;
   case nir_instr_type_load_const:
      update_fotid_load_const(nir_instr_as_load_const(instr));
      break;
   default:
      break;
   }

   return false;
}

struct ocs_context {
   const nir_opt_tid_function_options *options;
   uint8_t invocation_read[NIR_MAX_SUBGROUP_SIZE];
   bool zero_invocations[NIR_MAX_SUBGROUP_SIZE];
   nir_shader *shader;
};

static bool
constant_fold_scalar(nir_scalar s, unsigned invocation_id,
                     nir_shader *shader, nir_const_value *dest,
                     unsigned depth)
{
   if (depth > FOTID_MAX_RECURSION_DEPTH)
      return false;

   memset(dest, 0, sizeof(*dest));

   if (nir_scalar_is_alu(s)) {
      nir_alu_instr *alu = nir_instr_as_alu(s.def->parent_instr);
      nir_const_value sources[NIR_MAX_VEC_COMPONENTS][NIR_MAX_VEC_COMPONENTS];
      const nir_op_info *op_info = &nir_op_infos[alu->op];

      unsigned bit_size = 0;
      if (!nir_alu_type_get_type_size(op_info->output_type))
         bit_size = alu->def.bit_size;

      for (unsigned i = 0; i < op_info->num_inputs; i++) {
         if (!bit_size && !nir_alu_type_get_type_size(op_info->input_types[i]))
            bit_size = alu->src[i].src.ssa->bit_size;

         unsigned offset = 0;
         unsigned num_comp = op_info->input_sizes[i];
         if (num_comp == 0) {
            num_comp = 1;
            offset = s.comp;
         }

         for (unsigned j = 0; j < num_comp; j++) {
            nir_scalar ss = nir_get_scalar(alu->src[i].src.ssa,
                                           alu->src[i].swizzle[offset + j]);
            if (!constant_fold_scalar(ss, invocation_id, shader,
                                      &sources[i][j], depth + 1))
               return false;
         }
      }

      if (!bit_size)
         bit_size = 32;

      unsigned exec_mode = shader->info.float_controls_execution_mode;

      nir_const_value *srcs[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < op_info->num_inputs; ++i)
         srcs[i] = sources[i];
      nir_const_value dests[NIR_MAX_VEC_COMPONENTS];
      if (op_info->output_size == 0) {
         nir_eval_const_opcode(alu->op, dests, 1, bit_size, srcs, exec_mode);
         *dest = dests[0];
      } else {
         nir_eval_const_opcode(alu->op, dests, s.def->num_components, bit_size, srcs, exec_mode);
         *dest = dests[s.comp];
      }
      return true;
   } else if (nir_scalar_is_intrinsic(s)) {
      switch (nir_scalar_intrinsic_op(s)) {
      case nir_intrinsic_load_subgroup_invocation:
      case nir_intrinsic_load_local_invocation_index: {
         *dest = nir_const_value_for_uint(invocation_id, s.def->bit_size);
         return true;
      }
      case nir_intrinsic_load_local_invocation_id: {
         unsigned local_ids[3];
         local_ids[2] = invocation_id / (shader->info.workgroup_size[0] * shader->info.workgroup_size[1]);
         unsigned xy = invocation_id % (shader->info.workgroup_size[0] * shader->info.workgroup_size[1]);
         local_ids[1] = xy / shader->info.workgroup_size[0];
         local_ids[0] = xy % shader->info.workgroup_size[0];
         *dest = nir_const_value_for_uint(local_ids[s.comp], s.def->bit_size);
         return true;
      }
      case nir_intrinsic_inverse_ballot: {
         nir_def *src = nir_instr_as_intrinsic(s.def->parent_instr)->src[0].ssa;
         unsigned comp = invocation_id / src->bit_size;
         unsigned bit = invocation_id % src->bit_size;
         if (!constant_fold_scalar(nir_get_scalar(src, comp), invocation_id,
                                   shader, dest, depth + 1))
            return false;
         uint64_t ballot = nir_const_value_as_uint(*dest, src->bit_size);
         *dest = nir_const_value_for_bool(ballot & (1ull << bit), 1);
         return true;
      }
      default:
         return false;
      }
   } else if (nir_scalar_is_const(s)) {
      *dest = nir_scalar_as_const_value(s);
      return true;
   }

   return false;
}

static bool
gather_read_invocation_shuffle(nir_def *src,
                               struct ocs_context *ctx)
{
   nir_scalar s = { src, 0 };

   /* Recursive constant folding for each lane */
   for (unsigned invocation_id = 0;
        invocation_id < ctx->options->subgroup_size; invocation_id++) {
      nir_const_value value;
      if (!constant_fold_scalar(s, invocation_id, ctx->shader, &value, 0))
         return false;
      ctx->invocation_read[invocation_id] =
         MIN2(nir_const_value_as_uint(value, src->bit_size), UINT8_MAX);
   }

   return true;
}

static nir_alu_instr *
gather_invocation_uses(nir_def *def, struct ocs_context *ctx)
{
   if (def->num_components != 1 || !list_is_singular(&def->uses))
      return NULL;

   nir_alu_instr *bcsel = NULL;
   unsigned src_idx = 0;
   nir_foreach_use_including_if_safe(src, def) {
      if (nir_src_is_if(src) || nir_src_parent_instr(src)->type != nir_instr_type_alu)
         return NULL;
      bcsel = nir_instr_as_alu(nir_src_parent_instr(src));
      if (bcsel->op != nir_op_bcsel)
         return false;
      src_idx = list_entry(src, nir_alu_src, src) - bcsel->src;
      break;
   }

   assert(src_idx < 3);

   if (src_idx == 0 || !alu_src_get_fotid_mask(bcsel, 0))
      return NULL;

   nir_scalar s = { bcsel->src[0].src.ssa, bcsel->src[0].swizzle[0] };

   bool return_bcsel = nir_src_is_const(bcsel->src[3 - src_idx].src) &&
                       nir_src_as_uint(bcsel->src[3 - src_idx].src) == 0;

   /* Recursive constant folding for each lane */
   for (unsigned invocation_id = 0;
        invocation_id < ctx->options->subgroup_size; invocation_id++) {
      nir_const_value value;
      if (!constant_fold_scalar(s, invocation_id, ctx->shader, &value, 0)) {
         return_bcsel = false;
         continue;
      }

      /* If this lane selects the other source, so we can read an undefined
       * result. */
      if (nir_const_value_as_bool(value, 1) == (src_idx != 1)) {
         ctx->invocation_read[invocation_id] = UINT8_MAX;
         ctx->zero_invocations[invocation_id] = return_bcsel;
      }
   }

   if (return_bcsel) {
      return bcsel;
   } else {
      memset(ctx->zero_invocations, 0, sizeof(ctx->zero_invocations));
      return NULL;
   }
}

static bool
compute_bitmasks(struct ocs_context *ctx, unsigned *and_mask, unsigned *xor_mask)
{
   unsigned one = NIR_MAX_SUBGROUP_SIZE - 1;
   unsigned zero = NIR_MAX_SUBGROUP_SIZE - 1;
   unsigned copy = NIR_MAX_SUBGROUP_SIZE - 1;
   unsigned invert = NIR_MAX_SUBGROUP_SIZE - 1;

   for (unsigned i = 0; i < ctx->options->subgroup_size; i++) {
      unsigned read = ctx->invocation_read[i];
      if (read >= ctx->options->subgroup_size)
         continue; /* undefined result */

      copy &= ~(read ^ i);
      invert &= read ^ i;
      one &= read;
      zero &= ~read;
   }

   if ((copy | zero | one | invert) != NIR_MAX_SUBGROUP_SIZE - 1) {
      /* We didn't find valid masks for at least one bit. */
      return false;
   }

   *and_mask = copy | invert;
   *xor_mask = (one | invert) & ~copy;

   return true;
}

static nir_def *
try_opt_bitwise_mask(nir_builder *b, nir_def *src_def,
                     struct ocs_context *ctx)
{
   unsigned and_mask, xor_mask;
   if (!compute_bitmasks(ctx, &and_mask, &xor_mask))
      return NULL;

#if 0
   fprintf(stderr, "and %x, xor %x \n", and_mask, xor_mask);

   assert(false);
#endif

   if ((and_mask & (ctx->options->subgroup_size - 1)) == 0) {
      return nir_read_invocation(b, src_def, nir_imm_int(b, xor_mask));
   } else if (and_mask == 0x7f && xor_mask == 0) {
      return src_def;
   } else if (ctx->options->use_shuffle_xor && and_mask == 0x7f) {
      return nir_shuffle_xor(b, src_def, nir_imm_int(b, xor_mask));
   } else if (ctx->options->use_masked_swizzle_amd &&
              (and_mask & 0x60) == 0x60 && xor_mask <= 0x1f) {
      return nir_masked_swizzle_amd(b, src_def,
                                   (xor_mask << 10) | (and_mask & 0x1f));
   }

   return NULL;
}

static nir_def *
try_opt_rotate(nir_builder *b, nir_def *src_def, struct ocs_context *ctx)
{
   u_foreach_bit(i, ctx->options->rotate_cluster_sizes) {
      unsigned csize = 1u << i;
      unsigned cmask = csize - 1;

      unsigned delta;
      for (unsigned invocation = 0; invocation < ctx->options->subgroup_size;
           invocation++) {
         if (ctx->invocation_read[invocation] >= ctx->options->subgroup_size)
            continue;

         if (ctx->invocation_read[invocation] >= invocation)
            delta = ctx->invocation_read[invocation] - invocation;
         else
            delta = csize - invocation + ctx->invocation_read[invocation];

         if (delta < csize && delta != 0)
            goto delta_found;
      }

      continue;

   delta_found:
      for (unsigned invocation = 0; invocation < ctx->options->subgroup_size;
           invocation++) {
         if (ctx->invocation_read[invocation] >= ctx->options->subgroup_size)
            continue;
         unsigned read =
            ((invocation + delta) & cmask) + (invocation & ~cmask);
         if (read != ctx->invocation_read[invocation])
            goto continue_outerloop;
      }

      return nir_rotate(b, src_def, nir_imm_int(b, delta),
                        .execution_scope = SCOPE_SUBGROUP,
                        .cluster_size = csize);

   continue_outerloop:
   }

   return NULL;
}

static nir_def *
try_opt_shuffle_up_down(nir_builder *b, nir_def *src_def,
                        struct ocs_context *ctx)
{
   u_foreach_bit(i, ctx->options->shuffle_zero_fill_cluster_sizes) {
      int csize = 1 << i;
      unsigned cmask = csize - 1;

      int delta;
      for (unsigned invocation = 0; invocation < ctx->options->subgroup_size;
           invocation++) {
         if (ctx->invocation_read[invocation] >= ctx->options->subgroup_size)
            continue;

         delta = ctx->invocation_read[invocation] - invocation;

         if (delta < csize && delta > -csize && delta != 0)
            goto delta_found;
      }

      continue;

   delta_found:
      for (unsigned invocation = 0; invocation < ctx->options->subgroup_size;
           invocation++) {
         int read = invocation + delta;
         bool out_of_bounds = (read & ~cmask) != (invocation & ~cmask);
         if (ctx->zero_invocations[invocation] && !out_of_bounds)
            goto continue_outerloop;
         if (ctx->invocation_read[invocation] >= ctx->options->subgroup_size)
            continue;
         if (read != ctx->invocation_read[invocation] || out_of_bounds)
            goto continue_outerloop;
      }

      if (delta < 0)
         return nir_shuffle_up(b, src_def, nir_imm_int(b, -delta),
                               .cluster_size = csize,
                               .zero_shuffle_in = true);
      else
         return nir_shuffle_down(b, src_def, nir_imm_int(b, delta),
                                 .cluster_size = csize,
                                 .zero_shuffle_in = true);

   continue_outerloop:
   }

   return NULL;
}

static bool
opt_fotid_shuffle(nir_builder *b, nir_intrinsic_instr *instr,
                  const nir_opt_tid_function_options *options)
{
   struct ocs_context ctx = {
      .options = options,
      .zero_invocations = {},
      .shader = b->shader,
   };

   memset(ctx.invocation_read, 0xff, sizeof(ctx.invocation_read));

   if (!gather_read_invocation_shuffle(instr->src[1].ssa, &ctx))
      return false;


   /* Generalize invocation_read by taking into account which lanes
    * do not use the shuffle result because of bcsel.
    */
   nir_alu_instr *bcsel = gather_invocation_uses(&instr->def, &ctx);

#if 0
   for (int i = 0; i < options->subgroup_size; i++) {
      fprintf(stderr, "lane %d reads %d\n", i, ctx.invocation_read[i]);
   }

   for (int i = 0; i < options->subgroup_size; i++) {
      fprintf(stderr, "lane %d zero %d\n", i, ctx.zero_invocations[i]);
   }
#endif

   b->cursor = nir_after_instr(&instr->instr);

   nir_def *res = NULL;

   if (bcsel) {
      res = try_opt_shuffle_up_down(b, instr->src[0].ssa, &ctx);
      if (res) {
         nir_def_rewrite_uses(&bcsel->def, res);
         nir_instr_remove(&bcsel->instr);
         nir_instr_remove(&instr->instr);
         return true;
      }
   }

   if (!res)
      res = try_opt_bitwise_mask(b, instr->src[0].ssa, &ctx);
   if (!res)
      res = try_opt_rotate(b, instr->src[0].ssa, &ctx);

   if (res) {
      nir_def_rewrite_uses(&instr->def, res);
      nir_instr_remove(&instr->instr);
      return true;
   } else {
      return false;
   }
}

static bool
opt_fotid_bool(nir_builder *b, nir_alu_instr *instr,
               const nir_opt_tid_function_options *options)
{
   nir_scalar s = { &instr->def, 0 };

   b->cursor = nir_after_instr(&instr->instr);

   nir_def *ballot_comp[NIR_MAX_VEC_COMPONENTS];

   for (unsigned comp = 0; comp < options->ballot_num_comp; comp++) {
      uint64_t cballot = 0;
      for (unsigned invocation_id = comp * options->ballot_bit_size;
           invocation_id < options->subgroup_size; invocation_id++) {
         nir_const_value value;
         if (!constant_fold_scalar(s, invocation_id, b->shader, &value, 0))
            return false;
         cballot |= nir_const_value_as_uint(value, 1) << invocation_id;
      }
      ballot_comp[comp] = nir_imm_intN_t(b, cballot, options->ballot_bit_size);
   }

   nir_def *ballot = nir_vec(b, ballot_comp, options->ballot_num_comp);
   nir_def *res = nir_inverse_ballot(b, 1, ballot);
   res->parent_instr->pass_flags = 1;

   nir_def_rewrite_uses(&instr->def, res);
   nir_instr_remove(&instr->instr);
   return true;
}

static bool
visit_instr(nir_builder *b, nir_instr *instr, void *params)
{
   const nir_opt_tid_function_options *options = params;

   switch (instr->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      if (!options->ballot_bit_size || !options->ballot_num_comp)
         return false;
      if (alu->def.bit_size != 1 || alu->def.num_components > 1 || !instr->pass_flags)
         return false;
      return opt_fotid_bool(b, alu, options);
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      if (intrin->intrinsic != nir_intrinsic_shuffle)
         return false;
      if (!intrin->src[1].ssa->parent_instr->pass_flags)
         return false;
      return opt_fotid_shuffle(b, intrin, options);
   }
   default:
      return false;
   }
}

bool
nir_opt_tid_function(nir_shader *shader,
                     const nir_opt_tid_function_options *options)
{
   nir_shader_instructions_pass(shader, update_fotid_instr,
                                nir_metadata_none, (void *)options);

   return nir_shader_instructions_pass(
      shader, visit_instr,
      nir_metadata_block_index | nir_metadata_dominance, (void *)options);
}
