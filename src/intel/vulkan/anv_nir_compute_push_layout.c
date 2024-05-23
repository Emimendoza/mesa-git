/*
 * Copyright © 2019 Intel Corporation
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

#include "anv_nir.h"
#include "nir_builder.h"
#include "compiler/brw_nir.h"
#include "util/mesa-sha1.h"

struct push_range {
   unsigned start;
   unsigned end;
};

static void
push_range_reset(struct push_range *range)
{
   range->start = UINT_MAX;
   range->end   = 0;
}

static bool
push_range_empty(const struct push_range *range)
{
   return range->start > range->end;
}

struct push_context {
   bool has_const_ubo;
   struct push_range push;
   struct push_range driver;
};

static void
gather_push_ranges(struct push_context *ctx, nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_ubo:
               if (brw_nir_ubo_surface_index_is_pushable(intrin->src[0]) &&
                   nir_src_is_const(intrin->src[1]))
                  ctx->has_const_ubo = true;
               break;

            case nir_intrinsic_load_push_constant: {
               unsigned base = nir_intrinsic_base(intrin);
               unsigned range = nir_intrinsic_range(intrin);
               ctx->push.start = MIN2(ctx->push.start, base);
               ctx->push.end = MAX2(ctx->push.end, base + range);
               break;
            }

            case nir_intrinsic_load_driver_uniform_intel: {
               unsigned base = nir_intrinsic_base(intrin);
               unsigned range = nir_intrinsic_range(intrin);
               ctx->driver.start = MIN2(ctx->driver.start, base);
               ctx->driver.end = MAX2(ctx->driver.end, base + range);
               break;
            }

            default:
               break;
            }
         }
      }
   }
}

void
anv_nir_compute_push_layout(nir_shader *nir,
                            const struct anv_physical_device *pdevice,
                            enum brw_robustness_flags robust_flags,
                            bool fragment_dynamic,
                            struct brw_stage_prog_data *prog_data,
                            struct anv_pipeline_bind_map *map,
                            const struct anv_pipeline_push_map *push_map,
                            enum anv_descriptor_set_layout_type desc_type,
                            void *mem_ctx)
{
   const struct brw_compiler *compiler = pdevice->compiler;
   const struct intel_device_info *devinfo = compiler->devinfo;
   memset(map->push_ranges, 0, sizeof(map->push_ranges));

   struct push_context ctx = {};
   push_range_reset(&ctx.push);
   push_range_reset(&ctx.driver);
   gather_push_ranges(&ctx, nir);

   bool has_push_intrinsic = !push_range_empty(&ctx.push);

   const bool stage_can_push_ubo_ranges =
      brw_shader_stage_can_push_ubo(nir->info.stage);
   const bool stage_pulls_push_constants =
      brw_shader_stage_pulls_push_constants(devinfo, nir->info.stage);
   const bool push_ubo_ranges = ctx.has_const_ubo && stage_can_push_ubo_ranges;

   if (push_ubo_ranges && (robust_flags & BRW_ROBUSTNESS_UBO)) {
      /* We can't on-the-fly adjust our push ranges because doing so would
       * mess up the layout in the shader.  When robustBufferAccess is
       * enabled, we push a mask into the shader indicating which pushed
       * registers are valid and we zero out the invalid ones at the top of
       * the shader.
       */
      const uint32_t push_reg_mask_start =
         anv_drv_const_offset(push_reg_mask[nir->info.stage]);
      const uint32_t push_reg_mask_end = push_reg_mask_start +
                                         anv_drv_const_size(push_reg_mask[nir->info.stage]);
      ctx.driver.start = MIN2(ctx.driver.start, push_reg_mask_start);
      ctx.driver.end = MAX2(ctx.driver.end, push_reg_mask_end);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT && fragment_dynamic) {
      const uint32_t fs_msaa_flags_start =
         anv_drv_const_offset(gfx.fs_msaa_flags);
      const uint32_t fs_msaa_flags_end = fs_msaa_flags_start +
                                         anv_drv_const_size(gfx.fs_msaa_flags);
      ctx.driver.start = MIN2(ctx.driver.start, fs_msaa_flags_start);
      ctx.driver.end = MAX2(ctx.driver.end, fs_msaa_flags_end);
   }

   if (nir->info.stage == MESA_SHADER_COMPUTE && devinfo->verx10 < 125) {
      /* For compute shaders, we always have to have the subgroup ID.  The
       * back-end compiler will "helpfully" add it for us in the last push
       * constant slot.  Yes, there is an off-by-one error here but that's
       * because the back-end will add it so we want to claim the number of
       * push constants one dword less than the full amount including
       * gl_SubgroupId.
       */
      assert(ctx.driver.end <= anv_drv_const_offset(cs.subgroup_id));
      ctx.driver.start = MIN2(ctx.driver.start, anv_drv_const_offset(cs.subgroup_id));
      ctx.driver.end = anv_drv_const_offset(cs.subgroup_id);
   }

   /* Some stages cannot push/pull more than 1 range, so we have to merge
    * application & driver push constants.
    */
   const bool stage_has_single_push_range =
      (nir->info.stage == MESA_SHADER_COMPUTE && devinfo->verx10 < 125) ||
      brw_shader_stage_is_bindless(nir->info.stage);
   if (stage_has_single_push_range && !push_range_empty(&ctx.driver)) {
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
               if (intrin->intrinsic != nir_intrinsic_load_driver_uniform_intel)
                  continue;

               intrin->intrinsic = nir_intrinsic_load_push_constant;
               nir_intrinsic_set_base(intrin,
                                      nir_intrinsic_base(intrin) +
                                      MAX_PUSH_CONSTANTS_SIZE);
            }
         }
      }

      ctx.push.start = MIN2(ctx.push.start,
                            MAX_PUSH_CONSTANTS_SIZE + ctx.driver.start);
      ctx.push.end   = MAX_PUSH_CONSTANTS_SIZE + ctx.driver.end;
      push_range_reset(&ctx.driver);

      has_push_intrinsic = true;
   }

   /* Align push ranges down to the push constant alignment and make it no
    * larger than the range.end (no push constants is indicated by start =
    * UINT_MAX).
    */
   const uint32_t push_constant_align = stage_pulls_push_constants ? 4 : 32;
   ctx.push.start = MIN2(ctx.push.start, ctx.push.end);
   ctx.push.start = ROUND_DOWN_TO(ctx.push.start, push_constant_align);

   /* For the driver constants, also align push ranges down to the push
    * constant alignment unless the stage is pulling push constants, in which
    * case we let the shader add the offsets and do packing of constant
    * values.
    */
   ctx.driver.start = MIN2(ctx.driver.start, ctx.driver.end);
   ctx.driver.start = stage_pulls_push_constants ?
      0 : ROUND_DOWN_TO(ctx.driver.start, push_constant_align);

   const unsigned base_push_offset =
      gl_shader_stage_is_rt(nir->info.stage) ? 0 : ctx.push.start;

   /* For scalar, push data size needs to be aligned to a DWORD. */
   const unsigned alignment = 4;
   nir->num_uniforms = ALIGN(ctx.push.end - base_push_offset, alignment);
   prog_data->nr_params = nir->num_uniforms / 4;
   prog_data->param = rzalloc_array(mem_ctx, uint32_t, prog_data->nr_params);

   struct anv_push_range push_constant_range = {
      .set = ANV_DESCRIPTOR_SET_PUSH_CONSTANTS,
      .start_B = ctx.push.start,
      .length_B = align(ctx.push.end - ctx.push.start, push_constant_align),
   };
   struct anv_push_range driver_constant_range = {
      .set = ANV_DESCRIPTOR_SET_DRIVER_CONSTANTS,
      .start_B = ctx.driver.start,
      .length_B = align(ctx.driver.end - ctx.driver.start,
                        stage_pulls_push_constants ? 4 : 32),
   };

   if (has_push_intrinsic) {
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
               switch (intrin->intrinsic) {
               case nir_intrinsic_load_push_constant: {
                  intrin->intrinsic = nir_intrinsic_load_uniform;
                  nir_intrinsic_set_base(intrin,
                                         nir_intrinsic_base(intrin) - base_push_offset);
                  break;
               }

               default:
                  break;
               }
            }
         }
      }
   }

   int n_push = 0;

   if (push_constant_range.length_B > 0)
      map->push_ranges[n_push++] = push_constant_range;
   if (driver_constant_range.length_B > 0)
      map->push_ranges[n_push++] = driver_constant_range;

   if (push_ubo_ranges) {
      struct brw_ubo_range ubo_ranges[4];
      brw_nir_analyze_ubo_ranges(compiler, nir, ubo_ranges);

      /* Put the driver constants in the first UBO range. */
      if (driver_constant_range.length_B > 0) {
         prog_data->ubo_ranges[0] = (struct brw_ubo_range) {
            .block    = BRW_UBO_RANGE_DRIVER_INTERNAL,
            .start_B  = driver_constant_range.start_B,
            .length_B = driver_constant_range.length_B,
         };
         memcpy(&prog_data->ubo_ranges[1], ubo_ranges,
                (ARRAY_SIZE(ubo_ranges) - 1) * sizeof(ubo_ranges[0]));
      } else {
         memcpy(prog_data->ubo_ranges, ubo_ranges, sizeof(ubo_ranges));
      }

      const unsigned max_push_size = 64 * 32;

      unsigned total_push_size = push_constant_range.length_B;
      for (unsigned i = 0; i < 4; i++) {
         if (total_push_size + prog_data->ubo_ranges[i].length_B > max_push_size)
            prog_data->ubo_ranges[i].length_B = max_push_size - total_push_size;
         total_push_size += align(prog_data->ubo_ranges[i].length_B, 32);
      }
      assert(total_push_size <= max_push_size);

      if (robust_flags & BRW_ROBUSTNESS_UBO) {
         if (stage_has_single_push_range) {
            const uint32_t push_reg_mask_offset =
               MAX_PUSH_CONSTANTS_SIZE +
               anv_drv_const_offset(push_reg_mask[nir->info.stage]);
            assert(push_reg_mask_offset >= ctx.push.start);
            prog_data->push_reg_mask_param = (struct brw_push_param) {
               .block    = BRW_UBO_RANGE_PUSH_CONSTANT,
               .offset_B = push_reg_mask_offset - ctx.push.start,
            };
         } else {
            const uint32_t push_reg_mask_offset =
               anv_drv_const_offset(push_reg_mask[nir->info.stage]);
            assert(push_reg_mask_offset >= ctx.driver.start);
            prog_data->push_reg_mask_param = (struct brw_push_param) {
               .block    = BRW_UBO_RANGE_DRIVER_INTERNAL,
               .offset_B = push_reg_mask_offset,
            };
         }
      }

      unsigned range_start_reg = push_constant_range.length_B / 32;

      for (int i = 0; i < 4; i++) {
         struct brw_ubo_range *ubo_range = &prog_data->ubo_ranges[i];
         if (ubo_range->length_B == 0)
            continue;

         if (n_push >= 4) {
            memset(ubo_range, 0, sizeof(*ubo_range));
            continue;
         }

         /* Skip the driver constants, we put them in before. */
         if (ubo_range->block == BRW_UBO_RANGE_DRIVER_INTERNAL) {
            range_start_reg += DIV_ROUND_UP(ubo_range->length_B, 32);
            continue;
         }
         assert(ubo_range->block < push_map->block_count);
         const struct anv_pipeline_binding *binding =
            &push_map->block_to_descriptor[ubo_range->block];

         map->push_ranges[n_push++] = (struct anv_push_range) {
            .set = binding->set,
            .index = binding->index,
            .dynamic_offset_index = binding->dynamic_offset_index,
            .start_B = ubo_range->start_B,
            .length_B = ubo_range->length_B,
         };

         /* We only bother to shader-zero pushed client UBOs */
         if (binding->set < MAX_SETS &&
             (robust_flags & BRW_ROBUSTNESS_UBO)) {
            prog_data->zero_push_reg |= BITFIELD64_RANGE(
               range_start_reg, DIV_ROUND_UP(ubo_range->length_B, 32));
         }

         range_start_reg += DIV_ROUND_UP(ubo_range->length_B, 32);
      }
   } else if (!stage_has_single_push_range) {
      prog_data->ubo_ranges[0] = (struct brw_ubo_range) {
         .block    = BRW_UBO_RANGE_DRIVER_INTERNAL,
         .start_B  = driver_constant_range.start_B,
         .length_B = driver_constant_range.length_B,
      };
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT && fragment_dynamic) {
      struct brw_wm_prog_data *wm_prog_data =
         container_of(prog_data, struct brw_wm_prog_data, base);

      const uint32_t fs_msaa_flags_offset =
         anv_drv_const_offset(gfx.fs_msaa_flags);
      assert(fs_msaa_flags_offset >= ctx.driver.start);
      wm_prog_data->msaa_flags_param = (struct brw_push_param) {
         .block    = BRW_UBO_RANGE_DRIVER_INTERNAL,
         .offset_B = fs_msaa_flags_offset,
      };
   }

#if 0
   fprintf(stderr, "stage=%s push ranges:\n", gl_shader_stage_name(nir->info.stage));
   for (unsigned i = 0; i < ARRAY_SIZE(map->push_ranges); i++)
      fprintf(stderr, "   range%i: %04u-%04u set=%u index=%u\n", i,
              map->push_ranges[i].start_B,
              map->push_ranges[i].length_B,
              map->push_ranges[i].set,
              map->push_ranges[i].index);
#endif

   /* Now that we're done computing the push constant portion of the
    * bind map, hash it.  This lets us quickly determine if the actual
    * mapping has changed and not just a no-op pipeline change.
    */
   _mesa_sha1_compute(map->push_ranges,
                      sizeof(map->push_ranges),
                      map->push_sha1);
}

void
anv_nir_validate_push_layout(struct anv_device *device,
                             gl_shader_stage stage,
                             struct brw_stage_prog_data *prog_data,
                             struct anv_pipeline_bind_map *map)
{
#ifndef NDEBUG
   unsigned prog_data_push_size = align(prog_data->nr_params * 4, 32);
   for (unsigned i = 0; i < 4; i++)
      prog_data_push_size += align(prog_data->ubo_ranges[i].length_B, 32);

   unsigned bind_map_push_size = 0;
   for (unsigned i = 0; i < 4; i++)
      bind_map_push_size += align(map->push_ranges[i].length_B, 32);

   /* We could go through everything again but it should be enough to assert
    * that they push the same number of registers.  This should alert us if
    * the back-end compiler decides to re-arrange stuff or shrink a range.
    */
   assert(prog_data_push_size == bind_map_push_size);
#endif
}
