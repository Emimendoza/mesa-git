/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "perf/xe/intel_perf.h"

#include <sys/stat.h>

#include "perf/intel_perf.h"
#include "intel_perf_common.h"
#include "intel/common/i915/intel_gem.h"

#include "drm-uapi/xe_drm.h"

#define FIELD_PREP_ULL(_mask, _val) (((_val) << (ffsll(_mask) - 1)) & (_mask))

uint64_t xe_perf_get_oa_format(struct intel_perf_config *perf)
{
   uint64_t fmt = FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_FMT_TYPE, DRM_XE_OA_FMT_TYPE_OAG);

   /* same as I915_OA_FORMAT_A24u40_A14u32_B8_C8 and
    * I915_OA_FORMAT_A32u40_A4u32_B8_C8 returned for gfx 125+ and gfx 120
    * respectively.
    */
   fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SEL, 5);
   fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE, 256);
   fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_BC_REPORT, 0);

   return fmt;
}

bool
xe_oa_metrics_available(struct intel_perf_config *perf, int fd, bool use_register_snapshots)
{
   uint64_t invalid_config = UINT64_MAX;
   struct drm_xe_perf_param perf_param = {
      .perf_type = DRM_XE_PERF_TYPE_OA,
      .perf_op = DRM_XE_PERF_OP_REMOVE_CONFIG,
      .param = (uintptr_t)&invalid_config,
   };
   bool perf_oa_available = false;

   /* TODO: INTEL_PERF_FEATURE_HOLD_PREEMPTION is not yet actually supported
    * by Xe KMD it is not even supported in i915 with GuC submission but lets
    * fake support so ANV can use intel/perf.
    */
   perf->features_supported = INTEL_PERF_FEATURE_HOLD_PREEMPTION;

   /* check for KMD support */
   if (intel_ioctl(fd, DRM_IOCTL_XE_PERF, &perf_param) != 0) {
      /* perf_stream_paranoid == 1 and not privileges */
      if (errno == EACCES)
         return false;

      /* expected error for removing a invalid config is UINT64_MAX, return
       * not available for anything else
       */
      perf_oa_available = errno == ENOENT;
   }

   return perf_oa_available;
}
