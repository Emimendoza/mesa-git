#include "nvk_shader.h"

#include "nvk_cmd_buffer.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_pipeline.h"
#include "nvk_sampler.h"

#include "nouveau_bo.h"
#include "nouveau_context.h"
#include "vk_nir.h"
#include "vk_nir_convert_ycbcr.h"
#include "vk_pipeline.h"
#include "vk_shader_module.h"
#include "vk_ycbcr_conversion.h"

#include "nir.h"
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/nir/nir_xfb_info.h"

#include "nv50_ir_driver.h"

#include "cla097.h"
#include "clc397.h"
#include "clc597.h"
#include "nvk_cl9097.h"
#include "nvk_clc397.h"
#include "nvk_clb197.h"

static void
shared_var_info(const struct glsl_type *type, unsigned *size, unsigned *align)
{
   assert(glsl_type_is_vector_or_scalar(type));

   uint32_t comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned length = glsl_get_vector_elements(type);
   *size = comp_size * length, *align = comp_size;
}

static inline enum pipe_shader_type
pipe_shader_type_from_mesa(gl_shader_stage stage)
{
   switch (stage) {
   case MESA_SHADER_VERTEX:
      return PIPE_SHADER_VERTEX;
   case MESA_SHADER_TESS_CTRL:
      return PIPE_SHADER_TESS_CTRL;
   case MESA_SHADER_TESS_EVAL:
      return PIPE_SHADER_TESS_EVAL;
   case MESA_SHADER_GEOMETRY:
      return PIPE_SHADER_GEOMETRY;
   case MESA_SHADER_FRAGMENT:
      return PIPE_SHADER_FRAGMENT;
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_KERNEL:
      return PIPE_SHADER_COMPUTE;
   default:
      unreachable("bad shader stage");
   }
}

const nir_shader_compiler_options *
nvk_physical_device_nir_options(const struct nvk_physical_device *pdev,
                                gl_shader_stage stage)
{
   enum pipe_shader_type p_stage = pipe_shader_type_from_mesa(stage);
   return nv50_ir_nir_shader_compiler_options(pdev->info.chipset, p_stage);
}

struct spirv_to_nir_options
nvk_physical_device_spirv_options(const struct nvk_physical_device *pdev,
                                  const struct vk_pipeline_robustness_state *rs)
{
   return (struct spirv_to_nir_options) {
      .caps = {
         .demote_to_helper_invocation = true,
         .descriptor_array_dynamic_indexing = true,
         .descriptor_array_non_uniform_indexing = true,
         .descriptor_indexing = true,
         .device_group = true,
         .draw_parameters = true,
         .geometry_streams = true,
         .image_read_without_format = true,
         .image_write_without_format = true,
         .min_lod = true,
         .multiview = true,
         .physical_storage_buffer_address = true,
         .runtime_descriptor_array = true,
         .shader_clock = true,
         .shader_viewport_index_layer = true,
         .tessellation = true,
         .transform_feedback = true,
         .variable_pointers = true,
      },
      .ssbo_addr_format = nvk_buffer_addr_format(rs->storage_buffers),
      .phys_ssbo_addr_format = nir_address_format_64bit_global,
      .ubo_addr_format = nvk_buffer_addr_format(rs->uniform_buffers),
      .shared_addr_format = nir_address_format_32bit_offset,
      .min_ssbo_alignment = NVK_MIN_SSBO_ALIGNMENT,
      .min_ubo_alignment = NVK_MIN_UBO_ALIGNMENT,
   };
}

static bool
lower_image_size_to_txs(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic != nir_intrinsic_image_deref_size)
      return false;

   b->cursor = nir_instr_remove(&intrin->instr);

   nir_deref_instr *img = nir_src_as_deref(intrin->src[0]);
   nir_def *lod = nir_tex_type_has_lod(img->type) ?
                      intrin->src[1].ssa : NULL;
   nir_def *size = nir_txs_deref(b, img, lod);

   if (glsl_get_sampler_dim(img->type) == GLSL_SAMPLER_DIM_CUBE) {
      /* Cube image descriptors are set up as simple arrays but SPIR-V wants
       * the number of cubes.
       */
      if (glsl_sampler_type_is_array(img->type)) {
         size = nir_vec3(b, nir_channel(b, size, 0),
                            nir_channel(b, size, 1),
                            nir_udiv_imm(b, nir_channel(b, size, 2), 6));
      } else {
         size = nir_vec3(b, nir_channel(b, size, 0),
                            nir_channel(b, size, 1),
                            nir_imm_int(b, 1));
      }
   }

   nir_def_rewrite_uses(&intrin->dest.ssa, size);

   return true;
}

static bool
lower_load_global_constant_offset_instr(nir_builder *b, nir_instr *instr,
                                        UNUSED void *_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic != nir_intrinsic_load_global_constant_offset &&
       intrin->intrinsic != nir_intrinsic_load_global_constant_bounded)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *base_addr = intrin->src[0].ssa;
   nir_def *offset = intrin->src[1].ssa;

   nir_def *zero = NULL;
   if (intrin->intrinsic == nir_intrinsic_load_global_constant_bounded) {
      nir_def *bound = intrin->src[2].ssa;

      unsigned bit_size = intrin->dest.ssa.bit_size;
      assert(bit_size >= 8 && bit_size % 8 == 0);
      unsigned byte_size = bit_size / 8;

      zero = nir_imm_zero(b, intrin->num_components, bit_size);

      unsigned load_size = byte_size * intrin->num_components;

      nir_def *sat_offset =
         nir_umin(b, offset, nir_imm_int(b, UINT32_MAX - (load_size - 1)));
      nir_def *in_bounds =
         nir_ilt(b, nir_iadd_imm(b, sat_offset, load_size - 1), bound);

      nir_push_if(b, in_bounds);
   }

   nir_def *val =
      nir_build_load_global(b, intrin->dest.ssa.num_components,
                            intrin->dest.ssa.bit_size,
                            nir_iadd(b, base_addr, nir_u2u64(b, offset)),
                            .access = nir_intrinsic_access(intrin),
                            .align_mul = nir_intrinsic_align_mul(intrin),
                            .align_offset = nir_intrinsic_align_offset(intrin));

   if (intrin->intrinsic == nir_intrinsic_load_global_constant_bounded) {
      nir_pop_if(b, NULL);
      val = nir_if_phi(b, val, zero);
   }

   nir_def_rewrite_uses(&intrin->dest.ssa, val);

   return true;
}

static nir_variable *
find_or_create_input(nir_builder *b, const struct glsl_type *type,
                     const char *name, unsigned location)
{
   nir_foreach_shader_in_variable(in, b->shader) {
      if (in->data.location == location)
         return in;
   }
   nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                          type, name);
   in->data.location = location;
   if (glsl_type_is_integer(type))
      in->data.interpolation = INTERP_MODE_FLAT;

   return in;
}

static bool
lower_fragcoord_instr(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   assert(b->shader->info.stage == MESA_SHADER_FRAGMENT);
   nir_variable *var;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *val;
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_frag_coord:
      var = find_or_create_input(b, glsl_vec4_type(),
                                 "gl_FragCoord",
                                 VARYING_SLOT_POS);
      val = nir_load_var(b, var);
      break;
   case nir_intrinsic_load_point_coord:
      var = find_or_create_input(b, glsl_vector_type(GLSL_TYPE_FLOAT, 2),
                                 "gl_PointCoord",
                                 VARYING_SLOT_PNTC);
      val = nir_load_var(b, var);
      break;
   case nir_intrinsic_load_sample_pos:
      var = find_or_create_input(b, glsl_vec4_type(),
                                 "gl_FragCoord",
                                 VARYING_SLOT_POS);
      val = nir_ffract(b, nir_trim_vector(b, nir_load_var(b, var), 2));
      break;
   case nir_intrinsic_load_layer_id:
      var = find_or_create_input(b, glsl_int_type(),
                                 "gl_Layer", VARYING_SLOT_LAYER);
      val = nir_load_var(b, var);
      break;

   default:
      return false;
   }

   nir_def_rewrite_uses(&intrin->dest.ssa, val);

   return true;
}

static bool
lower_system_value_first_vertex(nir_builder *b, nir_instr *instr, UNUSED void *_data)
{
   assert(b->shader->info.stage == MESA_SHADER_VERTEX);

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic != nir_intrinsic_load_first_vertex)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def *base_vertex = nir_load_base_vertex(b);
   nir_def_rewrite_uses(&intrin->dest.ssa, base_vertex);

   return true;
}

static int
count_location_slots(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
assign_io_locations(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_VERTEX) {
      unsigned location = 0;
      nir_foreach_variable_with_modes(var, nir, nir_var_shader_in) {
         var->data.driver_location = location;
         if (nir_is_arrayed_io(var, nir->info.stage)) {
            location += glsl_count_attribute_slots(glsl_get_array_element(var->type), false);
         } else {
            location += glsl_count_attribute_slots(var->type, false);
         }
      }
      nir->num_inputs = location;
   } else {
      nir_foreach_shader_in_variable(var, nir) {
         assert(var->data.location >= VERT_ATTRIB_GENERIC0);
         var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
      }
   }

   {
      unsigned location = 0;
      nir_foreach_variable_with_modes(var, nir, nir_var_shader_out) {
         var->data.driver_location = location;
         if (nir_is_arrayed_io(var, nir->info.stage)) {
            location += glsl_count_attribute_slots(glsl_get_array_element(var->type), false);
         } else {
            location += glsl_count_attribute_slots(var->type, false);
         }
      }
      nir->num_outputs = location;
   }
}

struct ycbcr_conversion_cb_data
{
   struct vk_descriptor_set_layout *set_layouts[VK_MESA_PIPELINE_LAYOUT_MAX_SETS];
   uint32_t set_count;
};

static const struct vk_ycbcr_conversion_state *
lookup_ycbcr_conversion(const void *_cb_data, uint32_t set,
                        uint32_t binding, uint32_t array_index)
{
   const struct ycbcr_conversion_cb_data *cb_data = _cb_data;
   assert(set < cb_data->set_count);
   const struct nvk_descriptor_set_layout *set_layout =
      vk_to_nvk_descriptor_set_layout(cb_data->set_layouts[set]);
   assert(binding < set_layout->binding_count);

   const struct nvk_descriptor_set_binding_layout *bind_layout =
      &set_layout->binding[binding];

   if (bind_layout->immutable_samplers == NULL)
      return NULL;

   array_index = MIN2(array_index, bind_layout->array_size - 1);

   const struct nvk_sampler *sampler =
      bind_layout->immutable_samplers[array_index];

   return sampler && sampler->vk.ycbcr_conversion ?
          &sampler->vk.ycbcr_conversion->state : NULL;
}

static void
nvk_optimize_nir(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS(progress, nir, nir_split_array_vars, nir_var_function_temp);
      NIR_PASS(progress, nir, nir_shrink_vec_array_vars, nir_var_function_temp);

      if (!nir->info.var_copies_lowered) {
         /* Only run this pass if nir_lower_var_copies was not called
          * yet. That would lower away any copy_deref instructions and we
          * don't want to introduce any more.
          */
         NIR_PASS(progress, nir, nir_opt_find_array_copies);
      }
      NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
      NIR_PASS(progress, nir, nir_opt_dead_write_vars);
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      if (nir_opt_trivial_continues(nir)) {
         progress = true;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_remove_phis);
         NIR_PASS(progress, nir, nir_opt_dce);
      }
      NIR_PASS(progress, nir, nir_opt_if,
               nir_opt_if_aggressive_last_continue | nir_opt_if_optimize_phi_true_false);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      /*
       * this should be fine, likely a backend problem,
       * but a bunch of tessellation shaders blow up.
       * we should revisit this when NAK is merged.
       */
      NIR_PASS(progress, nir, nir_opt_peephole_select, 2, true, true);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_algebraic);

      NIR_PASS(progress, nir, nir_opt_undef);

      if (nir->options->max_unroll_iterations) {
         NIR_PASS(progress, nir, nir_opt_loop_unroll);
      }
   } while (progress);

   NIR_PASS(progress, nir, nir_opt_shrink_vectors);
   NIR_PASS(progress, nir, nir_remove_dead_variables,
            nir_var_function_temp | nir_var_shader_in | nir_var_shader_out, NULL);
}

void
nvk_lower_nir(struct nvk_device *dev, nir_shader *nir,
              const struct vk_pipeline_robustness_state *rs,
              bool is_multiview,
              struct vk_descriptor_set_layout *const *set_layouts,
              uint32_t set_count)
{
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   NIR_PASS(_, nir, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);

   NIR_PASS(_, nir, nir_lower_system_values);
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      // codegen does not support SYSTEM_VALUE_FIRST_VERTEX
      NIR_PASS(_, nir, nir_shader_instructions_pass,
              lower_system_value_first_vertex,
              nir_metadata_block_index | nir_metadata_dominance, NULL);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_shader_instructions_pass, lower_fragcoord_instr,
               nir_metadata_block_index | nir_metadata_dominance, NULL);
      NIR_PASS(_, nir, nir_lower_input_attachments,
               &(nir_input_attachment_options) {
                  .use_layer_id_sysval = is_multiview,
                  .use_view_id_for_layer = is_multiview,
               });
   }


   struct ycbcr_conversion_cb_data cb_data = {};
   cb_data.set_count = set_count;
   for (uint32_t i = 0; i < set_count; ++i) {
      cb_data.set_layouts[i] = set_layouts[i];
   }
   NIR_PASS(_, nir, nir_vk_lower_ycbcr_tex, lookup_ycbcr_conversion, &cb_data);

   nir_lower_compute_system_values_options csv_options = {
      .has_base_workgroup_id = true,
   };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv_options);

   /* Vulkan uses the separate-shader linking model */
   nir->info.separate_shader = true;

   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_GEOMETRY ||
       nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir), true, true);
   } else if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS_V(nir, nir_lower_io_to_temporaries, nir_shader_get_entrypoint(nir), true, false);
   }
   NIR_PASS(_, nir, nir_split_var_copies);

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);


   nvk_optimize_nir(nir);

   NIR_PASS(_, nir, nir_lower_var_copies);

   /* Lower push constants before lower_descriptors */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);

   NIR_PASS(_, nir, nir_shader_instructions_pass, lower_image_size_to_txs,
            nir_metadata_block_index | nir_metadata_dominance, NULL);

   /* Lower non-uniform access before lower_descriptors */
   enum nir_lower_non_uniform_access_type lower_non_uniform_access_types =
      nir_lower_non_uniform_ubo_access;

   if (pdev->info.cls_eng3d < TURING_A) {
      lower_non_uniform_access_types |= nir_lower_non_uniform_texture_access |
                                        nir_lower_non_uniform_image_access;
   }

   /* In practice, most shaders do not have non-uniform-qualified accesses
    * thus a cheaper and likely to fail check is run first.
    */
   if (nir_has_non_uniform_access(nir, lower_non_uniform_access_types)) {
      struct nir_lower_non_uniform_access_options opts = {
         .types = lower_non_uniform_access_types,
         .callback = NULL,
      };
      NIR_PASS(_, nir, nir_opt_non_uniform_access);
      NIR_PASS(_, nir, nir_lower_non_uniform_access, &opts);
   }

   NIR_PASS(_, nir, nvk_nir_lower_descriptors, rs, set_layouts, set_count);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo,
            nvk_buffer_addr_format(rs->storage_buffers));
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo,
            nvk_buffer_addr_format(rs->uniform_buffers));
   NIR_PASS(_, nir, nir_shader_instructions_pass,
            lower_load_global_constant_offset_instr,
            nir_metadata_block_index | nir_metadata_dominance, NULL);

   if (!nir->info.shared_memory_explicit_layout) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
               nir_var_mem_shared, shared_var_info);
   }
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
            nir_address_format_32bit_offset);


   uint32_t indirect_mask = nir_var_function_temp;

   NIR_PASS(_, nir, nir_lower_indirect_derefs, indirect_mask, 16);

   nvk_optimize_nir(nir);
   if (nir->info.stage != MESA_SHADER_COMPUTE)
      assign_io_locations(nir);

   NIR_PASS(_, nir, nir_lower_int64);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

#ifndef NDEBUG
static void
nvk_shader_dump(struct nvk_shader *shader)
{
   unsigned pos;

   if (shader->stage != MESA_SHADER_COMPUTE) {
      _debug_printf("dumping HDR for %s shader\n",
                    _mesa_shader_stage_to_string(shader->stage));
      for (pos = 0; pos < ARRAY_SIZE(shader->hdr); ++pos)
         _debug_printf("HDR[%02"PRIxPTR"] = 0x%08x\n",
                      pos * sizeof(shader->hdr[0]), shader->hdr[pos]);
   }
   _debug_printf("shader binary code (0x%x bytes):", shader->code_size);
   for (pos = 0; pos < shader->code_size / 4; ++pos) {
      if ((pos % 8) == 0)
         _debug_printf("\n");
      _debug_printf("%08x ", ((uint32_t *)shader->code_ptr)[pos]);
   }
   _debug_printf("\n");
}
#endif

#include "tgsi/tgsi_ureg.h"

/* NOTE: Using a[0x270] in FP may cause an error even if we're using less than
 * 124 scalar varying values.
 */
static uint32_t
nvc0_shader_input_address(unsigned sn, unsigned si)
{
   switch (sn) {
   case TGSI_SEMANTIC_TESSOUTER:    return 0x000 + si * 0x4;
   case TGSI_SEMANTIC_TESSINNER:    return 0x010 + si * 0x4;
   case TGSI_SEMANTIC_PATCH:        return 0x020 + si * 0x10;
   case TGSI_SEMANTIC_PRIMID:       return 0x060;
   case TGSI_SEMANTIC_LAYER:        return 0x064;
   case TGSI_SEMANTIC_VIEWPORT_INDEX:return 0x068;
   case TGSI_SEMANTIC_PSIZE:        return 0x06c;
   case TGSI_SEMANTIC_POSITION:     return 0x070;
   case TGSI_SEMANTIC_GENERIC:      return 0x080 + si * 0x10;
   case TGSI_SEMANTIC_FOG:          return 0x2e8;
   case TGSI_SEMANTIC_COLOR:        return 0x280 + si * 0x10;
   case TGSI_SEMANTIC_BCOLOR:       return 0x2a0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPDIST:     return 0x2c0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPVERTEX:   return 0x270;
   case TGSI_SEMANTIC_PCOORD:       return 0x2e0;
   case TGSI_SEMANTIC_TESSCOORD:    return 0x2f0;
   case TGSI_SEMANTIC_INSTANCEID:   return 0x2f8;
   case TGSI_SEMANTIC_VERTEXID:     return 0x2fc;
   case TGSI_SEMANTIC_TEXCOORD:     return 0x300 + si * 0x10;
   default:
      assert(!"invalid TGSI input semantic");
      return ~0;
   }
}

static uint32_t
nvc0_shader_output_address(unsigned sn, unsigned si)
{
   switch (sn) {
   case TGSI_SEMANTIC_TESSOUTER:     return 0x000 + si * 0x4;
   case TGSI_SEMANTIC_TESSINNER:     return 0x010 + si * 0x4;
   case TGSI_SEMANTIC_PATCH:         return 0x020 + si * 0x10;
   case TGSI_SEMANTIC_PRIMID:        return 0x060;
   case TGSI_SEMANTIC_LAYER:         return 0x064;
   case TGSI_SEMANTIC_VIEWPORT_INDEX:return 0x068;
   case TGSI_SEMANTIC_PSIZE:         return 0x06c;
   case TGSI_SEMANTIC_POSITION:      return 0x070;
   case TGSI_SEMANTIC_GENERIC:       return 0x080 + si * 0x10;
   case TGSI_SEMANTIC_FOG:           return 0x2e8;
   case TGSI_SEMANTIC_COLOR:         return 0x280 + si * 0x10;
   case TGSI_SEMANTIC_BCOLOR:        return 0x2a0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPDIST:      return 0x2c0 + si * 0x10;
   case TGSI_SEMANTIC_CLIPVERTEX:    return 0x270;
   case TGSI_SEMANTIC_TEXCOORD:      return 0x300 + si * 0x10;
   case TGSI_SEMANTIC_VIEWPORT_MASK: return 0x3a0;
   case TGSI_SEMANTIC_EDGEFLAG:      return ~0;
   default:
      assert(!"invalid TGSI output semantic");
      return ~0;
   }
}

static int
nvc0_vp_assign_input_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned i, c, n;

   for (n = 0, i = 0; i < info->numInputs; ++i) {
      switch (info->in[i].sn) {
      case TGSI_SEMANTIC_INSTANCEID: /* for SM4 only, in TGSI they're SVs */
      case TGSI_SEMANTIC_VERTEXID:
         info->in[i].mask = 0x1;
         info->in[i].slot[0] =
            nvc0_shader_input_address(info->in[i].sn, 0) / 4;
         continue;
      default:
         break;
      }
      for (c = 0; c < 4; ++c)
         info->in[i].slot[c] = (0x80 + n * 0x10 + c * 0x4) / 4;
      ++n;
   }

   return 0;
}

static int
nvc0_sp_assign_input_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned offset;
   unsigned i, c;

   for (i = 0; i < info->numInputs; ++i) {
      offset = nvc0_shader_input_address(info->in[i].sn, info->in[i].si);

      for (c = 0; c < 4; ++c)
         info->in[i].slot[c] = (offset + c * 0x4) / 4;
   }

   return 0;
}

static int
nvc0_fp_assign_output_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned count = info->prop.fp.numColourResults * 4;
   unsigned i, c;

   /* Compute the relative position of each color output, since skipped MRT
    * positions will not have registers allocated to them.
    */
   unsigned colors[8] = {0};
   for (i = 0; i < info->numOutputs; ++i)
      if (info->out[i].sn == TGSI_SEMANTIC_COLOR)
         colors[info->out[i].si] = 1;
   for (i = 0, c = 0; i < 8; i++)
      if (colors[i])
         colors[i] = c++;
   for (i = 0; i < info->numOutputs; ++i)
      if (info->out[i].sn == TGSI_SEMANTIC_COLOR)
         for (c = 0; c < 4; ++c)
            info->out[i].slot[c] = colors[info->out[i].si] * 4 + c;

   if (info->io.sampleMask < NV50_CODEGEN_MAX_VARYINGS)
      info->out[info->io.sampleMask].slot[0] = count++;
   else
   if (info->target >= 0xe0)
      count++; /* on Kepler, depth is always last colour reg + 2 */

   if (info->io.fragDepth < NV50_CODEGEN_MAX_VARYINGS)
      info->out[info->io.fragDepth].slot[2] = count;

   return 0;
}

static int
nvc0_sp_assign_output_slots(struct nv50_ir_prog_info_out *info)
{
   unsigned offset;
   unsigned i, c;

   for (i = 0; i < info->numOutputs; ++i) {
      offset = nvc0_shader_output_address(info->out[i].sn, info->out[i].si);

      for (c = 0; c < 4; ++c)
         info->out[i].slot[c] = (offset + c * 0x4) / 4;
   }

   return 0;
}

static int
nvc0_program_assign_varying_slots(struct nv50_ir_prog_info_out *info)
{
   int ret;

   if (info->type == PIPE_SHADER_VERTEX)
      ret = nvc0_vp_assign_input_slots(info);
   else
      ret = nvc0_sp_assign_input_slots(info);
   if (ret)
      return ret;

   if (info->type == PIPE_SHADER_FRAGMENT)
      ret = nvc0_fp_assign_output_slots(info);
   else
      ret = nvc0_sp_assign_output_slots(info);
   return ret;
}

static inline void
nvk_vtgs_hdr_update_oread(struct nvk_shader *vs, uint8_t slot)
{
   uint8_t min = (vs->hdr[4] >> 12) & 0xff;
   uint8_t max = (vs->hdr[4] >> 24);

   min = MIN2(min, slot);
   max = MAX2(max, slot);

   vs->hdr[4] = (max << 24) | (min << 12);
}

static int
nvk_vtgp_gen_header(struct nvk_shader *vs, struct nv50_ir_prog_info_out *info)
{
   unsigned i, c, a;

   for (i = 0; i < info->numInputs; ++i) {
      if (info->in[i].patch)
         continue;
      for (c = 0; c < 4; ++c) {
         a = info->in[i].slot[c];
         if (info->in[i].mask & (1 << c))
            vs->hdr[5 + a / 32] |= 1 << (a % 32);
      }
   }

   for (i = 0; i < info->numOutputs; ++i) {
      if (info->out[i].patch)
         continue;
      for (c = 0; c < 4; ++c) {
         if (!(info->out[i].mask & (1 << c)))
            continue;
         assert(info->out[i].slot[c] >= 0x40 / 4);
         a = info->out[i].slot[c] - 0x40 / 4;
         vs->hdr[13 + a / 32] |= 1 << (a % 32);
         if (info->out[i].oread)
            nvk_vtgs_hdr_update_oread(vs, info->out[i].slot[c]);
      }
   }

   for (i = 0; i < info->numSysVals; ++i) {
      switch (info->sv[i].sn) {
      case SYSTEM_VALUE_PRIMITIVE_ID:
         vs->hdr[5] |= 1 << 24;
         break;
      case SYSTEM_VALUE_INSTANCE_ID:
         vs->hdr[10] |= 1 << 30;
         break;
      case SYSTEM_VALUE_VERTEX_ID:
         vs->hdr[10] |= 1 << 31;
         break;
      case SYSTEM_VALUE_TESS_COORD:
         /* We don't have the mask, nor the slots populated. While this could
          * be achieved, the vast majority of the time if either of the coords
          * are read, then both will be read.
          */
         nvk_vtgs_hdr_update_oread(vs, 0x2f0 / 4);
         nvk_vtgs_hdr_update_oread(vs, 0x2f4 / 4);
         break;
      default:
         break;
      }
   }

   vs->vs.clip_enable = (1 << info->io.clipDistances) - 1;
   vs->vs.cull_enable =
      ((1 << info->io.cullDistances) - 1) << info->io.clipDistances;
   for (i = 0; i < info->io.cullDistances; ++i)
      vs->vs.clip_mode |= 1 << ((info->io.clipDistances + i) * 4);

   vs->vs.layer_viewport_relative = info->io.layer_viewport_relative;

   return 0;
}

static int
nvk_vs_gen_header(struct nvk_shader *vs, struct nv50_ir_prog_info_out *info)
{
   vs->hdr[0] = 0x20061 | (1 << 10);
   vs->hdr[4] = 0xff000;

   return nvk_vtgp_gen_header(vs, info);
}

static int
nvk_gs_gen_header(struct nvk_shader *gs,
                  const struct nir_shader *nir,
                  struct nv50_ir_prog_info_out *info)
{
   gs->hdr[0] = 0x20061 | (4 << 10);

   gs->hdr[2] = MIN2(info->prop.gp.instanceCount, 32) << 24;

   switch (info->prop.gp.outputPrim) {
   case MESA_PRIM_POINTS:
      gs->hdr[3] = 0x01000000;
      break;
   case MESA_PRIM_LINE_STRIP:
      gs->hdr[3] = 0x06000000;
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      gs->hdr[3] = 0x07000000;
      break;
   default:
      assert(0);
      break;
   }

   gs->hdr[4] = CLAMP(info->prop.gp.maxVertices, 1, 1024);

   gs->hdr[0] |= nir->info.gs.active_stream_mask << 28;

   return nvk_vtgp_gen_header(gs, info);
}

static void
nvk_generate_tessellation_parameters(const struct nv50_ir_prog_info_out *info,
                                     struct nvk_shader *shader)
{
   /* Before compiling we call merge_tess_info() so all the required execution
    * modes should already exist in the tessellation evaluation shader
    */
   uint32_t domain_type;
   switch (info->prop.tp.domain) {
   case MESA_PRIM_LINES:
      domain_type = NV9097_SET_TESSELLATION_PARAMETERS_DOMAIN_TYPE_ISOLINE;
      break;
   case MESA_PRIM_TRIANGLES:
      domain_type = NV9097_SET_TESSELLATION_PARAMETERS_DOMAIN_TYPE_TRIANGLE;
      break;
   case MESA_PRIM_QUADS:
      domain_type = NV9097_SET_TESSELLATION_PARAMETERS_DOMAIN_TYPE_QUAD;
      break;
   default:
      unreachable("Invalid tessellation domain type");
   }
   shader->tes.domain_type = domain_type;

   uint32_t spacing;
   switch (info->prop.tp.partitioning) {
   case PIPE_TESS_SPACING_EQUAL:
      spacing = NV9097_SET_TESSELLATION_PARAMETERS_SPACING_INTEGER;
      break;
   case PIPE_TESS_SPACING_FRACTIONAL_ODD:
      spacing = NV9097_SET_TESSELLATION_PARAMETERS_SPACING_FRACTIONAL_ODD;
      break;
   case PIPE_TESS_SPACING_FRACTIONAL_EVEN:
      spacing = NV9097_SET_TESSELLATION_PARAMETERS_SPACING_FRACTIONAL_EVEN;
      break;
   default:
      unreachable("Invalid tessellator partitioning");
   }
   shader->tes.spacing = spacing;

   uint32_t output_prims;
   if (info->prop.tp.outputPrim == MESA_PRIM_POINTS) { // point_mode
      output_prims = NV9097_SET_TESSELLATION_PARAMETERS_OUTPUT_PRIMITIVES_POINTS;
   } else if (info->prop.tp.domain == MESA_PRIM_LINES) { // isoline domain
      output_prims = NV9097_SET_TESSELLATION_PARAMETERS_OUTPUT_PRIMITIVES_LINES;
   } else {  // triangle/quad domain
      if (info->prop.tp.winding > 0) {
         output_prims = NV9097_SET_TESSELLATION_PARAMETERS_OUTPUT_PRIMITIVES_TRIANGLES_CW;
      } else {
         output_prims = NV9097_SET_TESSELLATION_PARAMETERS_OUTPUT_PRIMITIVES_TRIANGLES_CCW;
      }
   }
   shader->tes.output_prims = output_prims;
}

static int
nvk_tcs_gen_header(struct nvk_shader *tcs, struct nv50_ir_prog_info_out *info)
{
   unsigned opcs = 6; /* output patch constants (at least the TessFactors) */

   if (info->numPatchConstants)
      opcs = 8 + info->numPatchConstants * 4;

   tcs->hdr[0] = 0x20061 | (2 << 10);

   tcs->hdr[1] = opcs << 24;
   tcs->hdr[2] = info->prop.tp.outputPatchSize << 24;

   tcs->hdr[4] = 0xff000; /* initial min/max parallel output read address */

   nvk_vtgp_gen_header(tcs, info);

   if (info->target >= NVISA_GM107_CHIPSET) {
      /* On GM107+, the number of output patch components has moved in the TCP
       * header, but it seems like blob still also uses the old position.
       * Also, the high 8-bits are located in between the min/max parallel
       * field and has to be set after updating the outputs. */
      tcs->hdr[3] = (opcs & 0x0f) << 28;
      tcs->hdr[4] |= (opcs & 0xf0) << 16;
   }

   return 0;
}

static int
nvk_tes_gen_header(struct nvk_shader *tes, struct nv50_ir_prog_info_out *info)
{
   tes->hdr[0] = 0x20061 | (3 << 10);
   tes->hdr[4] = 0xff000;

   nvk_vtgp_gen_header(tes, info);

   nvk_generate_tessellation_parameters(info, tes);

   tes->hdr[18] |= 0x3 << 12; /* ? */

   return 0;
}

#define NVC0_INTERP_FLAT          (1 << 0)
#define NVC0_INTERP_PERSPECTIVE   (2 << 0)
#define NVC0_INTERP_LINEAR        (3 << 0)
#define NVC0_INTERP_CENTROID      (1 << 2)

static uint8_t
nvk_hdr_interp_mode(const struct nv50_ir_varying *var)
{
   if (var->linear)
      return NVC0_INTERP_LINEAR;
   if (var->flat)
      return NVC0_INTERP_FLAT;
   return NVC0_INTERP_PERSPECTIVE;
}


static int
nvk_fs_gen_header(struct nvk_shader *fs, struct nv50_ir_prog_info_out *info)
{
   unsigned i, c, a, m;

   /* just 00062 on Kepler */
   fs->hdr[0] = 0x20062 | (5 << 10);
   fs->hdr[5] = 0x80000000; /* getting a trap if FRAG_COORD_UMASK.w = 0 */

   if (info->prop.fp.usesDiscard)
      fs->hdr[0] |= 0x8000;
   if (!info->prop.fp.separateFragData)
      fs->hdr[0] |= 0x4000;
   if (info->io.sampleMask < 80 /* PIPE_MAX_SHADER_OUTPUTS */)
      fs->hdr[19] |= 0x1;
   if (info->prop.fp.writesDepth) {
      fs->hdr[19] |= 0x2;
      fs->flags[0] = 0x11; /* deactivate ZCULL */
   }

   for (i = 0; i < info->numInputs; ++i) {
      m = nvk_hdr_interp_mode(&info->in[i]);
      if (info->in[i].sn == TGSI_SEMANTIC_COLOR) {
         fs->fs.colors |= 1 << info->in[i].si;
         if (info->in[i].sc)
            fs->fs.color_interp[info->in[i].si] = m | (info->in[i].mask << 4);
      }
      for (c = 0; c < 4; ++c) {
         if (!(info->in[i].mask & (1 << c)))
            continue;
         a = info->in[i].slot[c];
         if (info->in[i].slot[0] >= (0x060 / 4) &&
             info->in[i].slot[0] <= (0x07c / 4)) {
            fs->hdr[5] |= 1 << (24 + (a - 0x060 / 4));
         } else
         if (info->in[i].slot[0] >= (0x2c0 / 4) &&
             info->in[i].slot[0] <= (0x2fc / 4)) {
            fs->hdr[14] |= (1 << (a - 0x280 / 4)) & 0x07ff0000;
         } else {
            if (info->in[i].slot[c] < (0x040 / 4) ||
                info->in[i].slot[c] > (0x380 / 4))
               continue;
            a *= 2;
            if (info->in[i].slot[0] >= (0x300 / 4))
               a -= 32;
            fs->hdr[4 + a / 32] |= m << (a % 32);
         }
      }
   }
   /* GM20x+ needs TGSI_SEMANTIC_POSITION to access sample locations */
   if (info->prop.fp.readsSampleLocations && info->target >= NVISA_GM200_CHIPSET)
      fs->hdr[5] |= 0x30000000;

   for (i = 0; i < info->numOutputs; ++i) {
      if (info->out[i].sn == TGSI_SEMANTIC_COLOR)
         fs->hdr[18] |= 0xf << (4 * info->out[i].si);
   }

   /* There are no "regular" attachments, but the shader still needs to be
    * executed. It seems like it wants to think that it has some color
    * outputs in order to actually run.
    */
   if (info->prop.fp.numColourResults == 0 && !info->prop.fp.writesDepth)
      fs->hdr[18] |= 0xf;

   fs->fs.early_z = info->prop.fp.earlyFragTests;
   fs->fs.sample_mask_in = info->prop.fp.usesSampleMaskIn;
   fs->fs.reads_framebuffer = info->prop.fp.readsFramebuffer;
   fs->fs.post_depth_coverage = info->prop.fp.postDepthCoverage;

   /* Mark position xy and layer as read */
   if (fs->fs.reads_framebuffer)
      fs->hdr[5] |= 0x32000000;

   return 0;
}

static uint8_t find_register_index_for_xfb_output(const struct nir_shader *nir,
                                                  nir_xfb_output_info output)
{
   nir_foreach_shader_out_variable(var, nir) {
      uint32_t slots = glsl_count_vec4_slots(var->type, false, false);
      for (uint32_t i = 0; i < slots; ++i) {
         if (output.location == (var->data.location+i)) {
            return var->data.driver_location+i;
         }
      }
   }
   // should not be reached
   return 0;
}

static struct nvk_transform_feedback_state *
nvk_fill_transform_feedback_state(struct nir_shader *nir,
                                  const struct nv50_ir_prog_info_out *info)
{
   const uint8_t max_buffers = 4;
   const uint8_t dw_bytes = 4;
   const struct nir_xfb_info *nx = nir->xfb_info;
   //nir_print_xfb_info(nx, stdout);

   struct nvk_transform_feedback_state *xfb =
      malloc(sizeof(struct nvk_transform_feedback_state));

   if (!xfb)
      return NULL;

   for (uint8_t b = 0; b < max_buffers; ++b) {
      xfb->stride[b] = b < nx->buffers_written ? nx->buffers[b].stride : 0;
      xfb->varying_count[b] = 0;
      xfb->stream[b] = nx->buffer_to_stream[b];
   }
   memset(xfb->varying_index, 0xff, sizeof(xfb->varying_index)); /* = skip */

   if (info->numOutputs == 0)
      return xfb;

   for (uint32_t i = 0; i < nx->output_count; ++i) {
      const nir_xfb_output_info output = nx->outputs[i];
      const uint8_t b = output.buffer;
      const uint8_t r = find_register_index_for_xfb_output(nir, output);
      uint32_t p = output.offset / dw_bytes;

      assert(r < info->numOutputs && p < ARRAY_SIZE(xfb->varying_index[b]));

      u_foreach_bit(c, nx->outputs[i].component_mask)
         xfb->varying_index[b][p++] = info->out[r].slot[c];

      xfb->varying_count[b] = MAX2(xfb->varying_count[b], p);
   }

   /* zero unused indices */
   for (uint8_t b = 0; b < 4; ++b)
      for (uint32_t c = xfb->varying_count[b]; c & 3; ++c)
         xfb->varying_index[b][c] = 0;

   return xfb;
}

VkResult
nvk_compile_nir(struct nvk_physical_device *pdev, nir_shader *nir,
                const struct nvk_fs_key *fs_key,
                struct nvk_shader *shader)
{
   struct nv50_ir_prog_info *info;
   struct nv50_ir_prog_info_out info_out = {};
   int ret;

   info = CALLOC_STRUCT(nv50_ir_prog_info);
   if (!info)
      return false;

   info->type = pipe_shader_type_from_mesa(nir->info.stage);
   info->target = pdev->info.chipset;
   info->bin.nir = nir;

   for (unsigned i = 0; i < 3; i++)
      shader->cp.block_size[i] = nir->info.workgroup_size[i];

   info->bin.smemSize = shader->cp.smem_size;
   info->dbgFlags = debug_get_num_option("NV50_PROG_DEBUG", 0);
   info->optLevel = debug_get_num_option("NV50_PROG_OPTIMIZE", 3);
   info->io.auxCBSlot = 1;
   info->io.uboInfoBase = 0;
   info->io.drawInfoBase = nvk_root_descriptor_offset(draw.base_vertex);
   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      info->prop.cp.gridInfoBase = 0;
   } else {
      info->assignSlots = nvc0_program_assign_varying_slots;
   }
   ret = nv50_ir_generate_code(info, &info_out);
   if (ret)
      return VK_ERROR_UNKNOWN;

   if (info_out.bin.fixupData) {
      nv50_ir_apply_fixups(info_out.bin.fixupData, info_out.bin.code,
                           fs_key && fs_key->force_per_sample,
                           false /* flatshade */, false /* alphatest */,
                           fs_key && fs_key->msaa);
   }

   shader->stage = nir->info.stage;
   shader->code_ptr = (uint8_t *)info_out.bin.code;
   shader->code_size = info_out.bin.codeSize;

   if (info_out.target >= NVISA_GV100_CHIPSET)
      shader->num_gprs = MAX2(4, info_out.bin.maxGPR + 3);
   else
      shader->num_gprs = MAX2(4, info_out.bin.maxGPR + 1);
   shader->cp.smem_size = info_out.bin.smemSize;
   shader->num_barriers = info_out.numBarriers;

   switch (info->type) {
   case PIPE_SHADER_VERTEX:
      ret = nvk_vs_gen_header(shader, &info_out);
      break;
   case PIPE_SHADER_FRAGMENT:
      ret = nvk_fs_gen_header(shader, &info_out);
      shader->fs.uses_sample_shading = nir->info.fs.uses_sample_shading;
      break;
   case PIPE_SHADER_GEOMETRY:
      ret = nvk_gs_gen_header(shader, nir, &info_out);
      break;
   case PIPE_SHADER_TESS_CTRL:
      ret = nvk_tcs_gen_header(shader, &info_out);
      break;
   case PIPE_SHADER_TESS_EVAL:
      ret = nvk_tes_gen_header(shader, &info_out);
      break;
   case PIPE_SHADER_COMPUTE:
      break;
   default:
      unreachable("Invalid shader stage");
      break;
   }
   assert(ret == 0);

   if (info_out.bin.tlsSpace) {
      assert(info_out.bin.tlsSpace < (1 << 24));
      shader->hdr[0] |= 1 << 26;
      shader->hdr[1] |= align(info_out.bin.tlsSpace, 0x10); /* l[] size */
      shader->slm_size = info_out.bin.tlsSpace;
   }

   if (info_out.io.globalAccess)
      shader->hdr[0] |= 1 << 26;
   if (info_out.io.globalAccess & 0x2)
      shader->hdr[0] |= 1 << 16;
   if (info_out.io.fp64)
      shader->hdr[0] |= 1 << 27;

   if (nir->xfb_info) {
      shader->xfb = nvk_fill_transform_feedback_state(nir, &info_out);
      if (shader->xfb == NULL) {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

VkResult
nvk_shader_upload(struct nvk_device *dev, struct nvk_shader *shader)
{
   uint32_t hdr_size = 0;
   if (shader->stage != MESA_SHADER_COMPUTE) {
      if (dev->pdev->info.cls_eng3d >= TURING_A)
         hdr_size = TU102_SHADER_HEADER_SIZE;
      else
         hdr_size = GF100_SHADER_HEADER_SIZE;
   }

   /* Fermi   needs 0x40 alignment
    * Kepler+ needs the first instruction to be 0x80 aligned, so we waste 0x30 bytes
    */
   int alignment = dev->pdev->info.cls_eng3d >= KEPLER_A ? 0x80 : 0x40;
   int offset = 0;

   if (dev->pdev->info.cls_eng3d >= KEPLER_A &&
       dev->pdev->info.cls_eng3d < TURING_A &&
       hdr_size > 0) {
      /* offset will be 0x30 */
      offset = alignment - hdr_size;
   }

   uint32_t total_size = shader->code_size + hdr_size + offset;
   char *data = malloc(total_size);
   if (data == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   memcpy(data + offset, shader->hdr, hdr_size);
   memcpy(data + offset + hdr_size, shader->code_ptr, shader->code_size);

#ifndef NDEBUG
   if (debug_get_bool_option("NV50_PROG_DEBUG", false))
      nvk_shader_dump(shader);
#endif

   VkResult result = nvk_heap_upload(dev, &dev->shader_heap, data,
                                     total_size, alignment, &shader->upload_addr);
   if (result == VK_SUCCESS) {
      shader->upload_size = total_size;
      shader->upload_padding = offset;
   }
   free(data);

   return result;
}

static VkResult nvk_create_spirv_shader(struct nvk_device *dev,
                                        const VkShaderCreateInfoEXT *pCreateInfo,
                                        const VkAllocationCallbacks *pAllocator,
                                        VkShaderEXT *pShader)
{
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   bool is_binary = pCreateInfo->codeType == VK_SHADER_CODE_TYPE_BINARY_EXT;
   const uint32_t *spirv_code = is_binary ? pCreateInfo->pCode+20 :
                                                 pCreateInfo->pCode;
   const size_t spirv_code_size = is_binary ? pCreateInfo->codeSize-20 :
                                              pCreateInfo->codeSize;

   const gl_shader_stage stage = vk_to_mesa_shader_stage(pCreateInfo->stage);
   struct vk_pipeline_robustness_state rs;
   vk_pipeline_robustness_state_fill(&dev->vk, &rs, NULL, NULL);

   const nir_shader_compiler_options *nir_options =
      nvk_physical_device_nir_options(pdev, stage);
   const struct spirv_to_nir_options spirv_options =
      nvk_physical_device_spirv_options(pdev, &rs);

   assert(vk_spirv_version(spirv_code, spirv_code_size) <= 0x10600);
   // TODO: do better, as this is copied from vk common
   nir_shader *nir = vk_spirv_to_nir(&dev->vk, spirv_code, spirv_code_size,
                                     stage, pCreateInfo->pName,
                                     stage == MESA_SHADER_COMPUTE ? SUBGROUP_SIZE_FULL_SUBGROUPS : SUBGROUP_SIZE_API_CONSTANT,
                                     pCreateInfo->pSpecializationInfo,
                                     &spirv_options, nir_options, NULL);

   if (nir == NULL) {
      return vk_errorf(dev, VK_ERROR_UNKNOWN, "spirv_to_nir_failed");
   }

   // no need to call merge tess info because all execution modes must be in tess

   // TODO: check for null
   struct vk_descriptor_set_layout *set_layouts[VK_MESA_PIPELINE_LAYOUT_MAX_SETS];
   for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; ++i) {
      VK_FROM_HANDLE(vk_descriptor_set_layout, layout, pCreateInfo->pSetLayouts[i]);
      set_layouts[i] = layout;
   }

   // TODO multiview
   nvk_lower_nir(dev, nir, &rs, false, set_layouts, pCreateInfo->setLayoutCount);

   struct nvk_shader *shader;
   VkResult result = VK_SUCCESS;

   shader = vk_object_zalloc(&dev->vk, pAllocator, sizeof(*shader),
                             VK_OBJECT_TYPE_SHADER_EXT);
   // TODO nir leak
   if (shader == NULL) {
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   // populate nvk_fs_key
   struct nvk_fs_key *fs_key = NULL;
   result = nvk_compile_nir(pdev, nir, fs_key, shader);
   ralloc_free(nir);

   if (result != VK_SUCCESS) {
      goto fail;
   }

   result = nvk_shader_upload(dev, shader);
   if (result != VK_SUCCESS) {
      goto fail;
   }

   *pShader = nvk_shader_to_handle(shader);

   unsigned char key[20];
   struct mesa_sha1 sha1_ctx;
   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, spirv_code, spirv_code_size);
   _mesa_sha1_final(&sha1_ctx, key);

   shader->temp_shader_binary_size = sizeof(key) + spirv_code_size;
   shader->temp_shader_binary = vk_zalloc(&dev->vk.alloc,
                                          shader->temp_shader_binary_size, 16,
                                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   memcpy(shader->temp_shader_binary, key, sizeof(key));
   memcpy(shader->temp_shader_binary+sizeof(key), spirv_code, spirv_code_size);

   return VK_SUCCESS;

fail:
   vk_object_free(&dev->vk, pAllocator, shader);
   return result;
}

static bool
check_shader_binary_compatibility(const VkShaderCreateInfoEXT *info)
{
   const unsigned char *binary = info->pCode;
   const size_t size = info->codeSize;

   unsigned char key[20];
   if (size <= sizeof(key)) {
      return false;
   }

   const unsigned char *spirv_code = binary+sizeof(key);
   size_t spirv_code_size = size-sizeof(key);

   struct mesa_sha1 sha1_ctx;
   _mesa_sha1_init(&sha1_ctx);
   _mesa_sha1_update(&sha1_ctx, spirv_code, spirv_code_size);
   _mesa_sha1_final(&sha1_ctx, key);

   return !memcmp(key, binary, sizeof(key));
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateShadersEXT(VkDevice _device, uint32_t createInfoCount,
                     const VkShaderCreateInfoEXT *pCreateInfos,
                     const VkAllocationCallbacks *pAllocator,
                     VkShaderEXT *pShaders)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   VkResult result = VK_SUCCESS;

   uint32_t i = 0;
   for (; i < createInfoCount; i++) {
      // check if binary (spirv) is compatible
      if (pCreateInfos[i].codeType == VK_SHADER_CODE_TYPE_BINARY_EXT &&
          !check_shader_binary_compatibility(&pCreateInfos[i])) {
         result = VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
         pShaders[i] = VK_NULL_HANDLE;
         break;
      }

      VkResult r = nvk_create_spirv_shader(dev, &pCreateInfos[i],
            pAllocator, &pShaders[i]);

      /*
       *  When this function returns, whether or not it succeeds,
       *  it is guaranteed that every element of pShaders will have been
       *  overwritten by either VK_NULL_HANDLE or a valid VkShaderEXT handle.

       *  This means that whenever shader creation fails, the application can
       *  determine which shader the returned error pertains to by locating
       *  the first VK_NULL_HANDLE element in pShaders.
       *  It also means that an application can reliably clean up from a
       *  failed call by iterating over the pShaders array and destroying
       *  every element that is not VK_NULL_HANDLE.
       */

      if (r != VK_SUCCESS) {
         result = r;
         pShaders[i] = VK_NULL_HANDLE;
         break;
      }
   }

   for (; i < createInfoCount; ++i)
      pShaders[i] = VK_NULL_HANDLE;

   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyShaderEXT(VkDevice device, VkShaderEXT shader,
                     const VkAllocationCallbacks *pAllocator)
{
   // TODO
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetShaderBinaryDataEXT(VkDevice device, VkShaderEXT _shader,
                           size_t *pDataSize, void *pData)
{
   assert(pDataSize);
   VK_FROM_HANDLE(nvk_shader, shader, _shader);

   if (pData == NULL) {
      *pDataSize = shader->temp_shader_binary_size;
      return VK_SUCCESS;
   } else {
      if (*pDataSize < shader->temp_shader_binary_size) {
         *pDataSize = 0;
         return VK_INCOMPLETE;
      } else {
         memcpy(pData, shader->temp_shader_binary,
                shader->temp_shader_binary_size);
         *pDataSize = shader->temp_shader_binary_size;
         return VK_SUCCESS;
      }
   }
}

// duplicates to nvk_graphics_pipeline
static const uint32_t mesa_to_nv9097_shader_type[] = {
   [MESA_SHADER_VERTEX]    = NV9097_SET_PIPELINE_SHADER_TYPE_VERTEX,
   [MESA_SHADER_TESS_CTRL] = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION_INIT,
   [MESA_SHADER_TESS_EVAL] = NV9097_SET_PIPELINE_SHADER_TYPE_TESSELLATION,
   [MESA_SHADER_GEOMETRY]  = NV9097_SET_PIPELINE_SHADER_TYPE_GEOMETRY,
   [MESA_SHADER_FRAGMENT]  = NV9097_SET_PIPELINE_SHADER_TYPE_PIXEL,
};

static void nvk_cmd_bind_empty_shader(struct nvk_cmd_buffer *cmd,
                                      gl_shader_stage stage)
{
   struct nv_push *p = nvk_cmd_buffer_push(cmd, 2);
   uint32_t idx = mesa_to_nv9097_shader_type[stage];
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(idx), {
      .enable  = ENABLE_FALSE,
      .type    = mesa_to_nv9097_shader_type[stage],
   });
}

static void nvk_cmd_bind_shader(struct nvk_cmd_buffer *cmd,
                                gl_shader_stage stage,
                                struct nvk_shader *shader)
{
   struct nv_push *p = nvk_cmd_buffer_push(cmd, 16);
   assert(shader->upload_size > 0);
   struct nvk_device *dev = nvk_cmd_buffer_device(cmd);

   assert(shader->stage == stage);
   uint32_t idx = mesa_to_nv9097_shader_type[stage];
   P_IMMD(p, NV9097, SET_PIPELINE_SHADER(idx), {
      .enable  = ENABLE_TRUE,
      .type    = mesa_to_nv9097_shader_type[stage], });

   uint64_t addr = nvk_shader_address(shader);
   if (dev->pdev->info.cls_eng3d >= VOLTA_A) {
      P_MTHD(p, NVC397, SET_PIPELINE_PROGRAM_ADDRESS_A(idx));
      P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_A(p, idx, addr >> 32);
      P_NVC397_SET_PIPELINE_PROGRAM_ADDRESS_B(p, idx, addr);
   } else {
      assert(addr < 0xffffffff);
      P_IMMD(p, NV9097, SET_PIPELINE_PROGRAM(idx), addr);
   }

   P_IMMD(p, NV9097, SET_PIPELINE_REGISTER_COUNT(idx), shader->num_gprs);

   switch (stage) {
      case MESA_SHADER_VERTEX:
      case MESA_SHADER_GEOMETRY:
      case MESA_SHADER_TESS_CTRL:
         break;

      case MESA_SHADER_FRAGMENT:
         P_IMMD(p, NV9097, SET_SUBTILING_PERF_KNOB_A, {
            .fraction_of_spm_register_file_per_subtile         = 0x10,
            .fraction_of_spm_pixel_output_buffer_per_subtile   = 0x40,
            .fraction_of_spm_triangle_ram_per_subtile          = 0x16,
            .fraction_of_max_quads_per_subtile                 = 0x20,
         });
         P_NV9097_SET_SUBTILING_PERF_KNOB_B(p, 0x20);

         P_IMMD(p, NV9097, SET_API_MANDATED_EARLY_Z, shader->fs.early_z);

         // TODO: fix
         P_IMMD(p, NVB197, SET_POST_Z_PS_IMASK,
               shader->fs.post_depth_coverage);
         //if (dev->pdev->info.cls_eng3d >= MAXWELL_B) {
         //   P_IMMD(p, NVB197, SET_POST_Z_PS_IMASK,
         //         shader->fs.post_depth_coverage);
         //} else {
         //   assert(!shader->fs.post_depth_coverage);
         //}

         P_MTHD(p, NV9097, SET_ZCULL_BOUNDS);
         P_INLINE_DATA(p, shader->flags[0]);

#if 0
         /* If we're using the incoming sample mask and doing sample shading,
          * we have to do sample shading "to the max", otherwise there's no
          * way to tell which sets of samples are covered by the current
          * invocation.
          */
         force_max_samples = shader->fs.sample_mask_in ||
            shader->fs.uses_sample_shading;
#endif
         break;

      case MESA_SHADER_TESS_EVAL:
         P_MTHD(p, NV9097, SET_TESSELLATION_PARAMETERS);
         P_NV9097_SET_TESSELLATION_PARAMETERS(p, {
            shader->tes.domain_type,
            shader->tes.spacing,
            shader->tes.output_prims
         });
         break;

      default:
         unreachable("Unsupported shader stage");
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindShadersEXT(VkCommandBuffer commandBuffer, uint32_t stageCount,
                      const VkShaderStageFlagBits* pStages,
                      const VkShaderEXT* pShaders)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   /* If pShaders is NULL, vkCmdBindShadersEXT behaves as if pShaders was an
    * array of stageCount VK_NULL_HANDLE values
    * (i.e., any shaders bound to the stages specified in pStages are unbound).
    */
   for (uint32_t i = 0; i < stageCount; ++i) {
      gl_shader_stage stage = vk_to_mesa_shader_stage(pStages[i]);
      if (!pShaders || pShaders[i] == VK_NULL_HANDLE) {
         nvk_cmd_bind_empty_shader(cmd, stage);
      } else {
         VK_FROM_HANDLE(nvk_shader, shader, pShaders[i]);
         nvk_cmd_bind_shader(cmd, stage, shader);
      }
   }
}


