/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_IMAGE_VIEW_H
#define PANVK_IMAGE_VIEW_H

#include <stdint.h>

#include "vulkan/runtime/vk_image.h"

#include "pan_texture.h"

#include "genxml/gen_macros.h"

struct panvk_priv_bo;

struct panvk_image_view {
   struct vk_image_view vk;

   struct pan_image_view pview;

   struct panvk_priv_bo *bo;

#ifdef PAN_ARCH
   struct {
      struct mali_texture_packed tex;
      struct mali_attribute_buffer_packed img_attrib_buf[2];
   } descs;
#endif
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_image_view, vk.base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW);

#endif