/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_SHADER_H
#define PANVK_SHADER_H

#include "util/u_dynarray.h"

#include "util/pan_ir.h"

#include "pan_desc.h"

#include "panvk_descriptor_set.h"
#include "panvk_macros.h"
#include "panvk_pipeline_layout.h"

struct nir_shader;
struct pan_blend_state;
struct panvk_device;

union panvk_sysval_vec4 {
   float f32[4];
   uint32_t u32[4];
};

struct panvk_sysvals {
   union {
      struct {
         /* Only for graphics */
         union panvk_sysval_vec4 viewport_scale;
         union panvk_sysval_vec4 viewport_offset;
         union panvk_sysval_vec4 blend_constants;

         uint32_t first_vertex;
         uint32_t base_vertex;
         uint32_t base_instance;
      };

      struct {
         /* Only for compute */
         union panvk_sysval_vec4 num_work_groups;
         union panvk_sysval_vec4 local_group_size;
      };
   };
};

struct panvk_shader {
   struct pan_shader_info info;
   struct util_dynarray binary;
   struct pan_compute_dim local_size;
   bool has_img_access;
};

void panvk_shader_destroy(struct panvk_device *dev, struct panvk_shader *shader,
                          const VkAllocationCallbacks *alloc);

#ifdef PAN_ARCH
bool panvk_per_arch(blend_needs_lowering)(const struct panvk_device *dev,
                                          const struct pan_blend_state *state,
                                          unsigned rt);

struct panvk_shader *panvk_per_arch(shader_create)(
   struct panvk_device *dev, gl_shader_stage stage,
   const VkPipelineShaderStageCreateInfo *stage_info,
   const struct panvk_pipeline_layout *layout,
   struct pan_blend_state *blend_state, bool static_blend_constants,
   const VkAllocationCallbacks *alloc);

bool panvk_per_arch(nir_lower_descriptors)(
   struct nir_shader *nir, struct panvk_device *dev,
   const struct panvk_pipeline_layout *layout, bool *has_img_access_out);
#endif

#endif
