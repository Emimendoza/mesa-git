/*
 * Copyright 2024 Red Hat, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Compatibility stub for Xorg. This responds to just enough of the legacy DRI
 * interface to allow the X server to initialize GLX and enable direct
 * rendering clients. It implements the screen creation hook and provides a
 * (static, unambitious) list of framebuffer configs. It will not create an
 * indirect context; Indirect contexts have been disabled by default since
 * 2014 and would be limited to GL 1.4 in any case, so this is no great loss.
 *
 * If you do want indirect contexts to work, you have options. This stub is
 * new with Mesa 24.1, so one option is to use an older Mesa release stream.
 * Another option is to use an X server that does not need this interface. For
 * Xwayland and Xephyr that's XX.X or newer, and for Xorg drivers using glamor
 * for acceleration that's YY.Y or newer.
 */

#include "main/glconfig.h"
#include <GL/internal/dri_interface.h>

/* avoid needing X11 headers */
#define GLX_NONE 0x8000
#define GLX_DONT_CARE 0xFFFFFFFF

#define CONFIG(color, zs) \
   { \
      .color_format = color, \
      .zs_format = zs, \
   }

static const struct gl_config drilConfigs[] = {
   CONFIG(PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_FORMAT_Z24_UNORM_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8X8_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R8G8B8X8_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R8G8B8X8_UNORM, PIPE_FORMAT_Z24_UNORM_S8_UINT),
   CONFIG(PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_NONE),
   CONFIG(PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_S8_UINT),
   CONFIG(PIPE_FORMAT_R5G6B5_UNORM, PIPE_FORMAT_Z16_UNORM),
   // etc...
};

#define RGB UTIL_FORMAT_COLORSPACE_RGB
#define RED 0
#define GREEN 1
#define BLUE 2
#define ALPHA 3
#define ZS UTIL_FORMAT_COLORSPACE_ZS
#define DEPTH 0
#define STENCIL 1

#define CASE(ATTRIB, VALUE) \
   case __DRI_ATTRIB_ ## ATTRIB : \
      *value = VALUE; \
      break;

#define SIZE(f, cs, chan)  util_format_get_component_bits(f, cs, chan)
#define SHIFT(f, cs, chan) util_format_get_component_shift(f, cs, chan)
#define MASK(f, cs, chan) \
   (((1 << SIZE(f, cs, chan)) - 1) << SHIFT(f, cs, chan))

static int
drilIndexConfigAttrib(const __DRIconfig *_config, int index,
                      unsigned int *attrib, unsigned int *value)
{
   struct gl_config *config = (void *)_config;
   enum pipe_format color_format = config->color_format;
   enum pipe_format zs_format = config->zs_format;
   enum pipe_format accum_format = config->accum_format;

   if (index >= __DRI_ATTRIB_MAX)
      return 0;

   switch (index) {
      case __DRI_ATTRIB_BUFFER_SIZE:
         unsigned int red = 0, green = 0, blue = 0;
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_RED_SIZE, attrib, &red);
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_GREEN_SIZE, attrib, &green);
         drilIndexConfigAttrib(_config, __DRI_ATTRIB_BLUE_SIZE, attrib, &blue);
         *value = red + green + blue;
         break;

      CASE(RED_SIZE,          SIZE(color_format, RGB, 0));
      CASE(GREEN_SIZE,        SIZE(color_format, RGB, 1));
      CASE(BLUE_SIZE,         SIZE(color_format, RGB, 2));
      CASE(ALPHA_SIZE,        SIZE(color_format, RGB, 3));
      CASE(DEPTH_SIZE,        SIZE(zs_format,    ZS,  0));
      CASE(STENCIL_SIZE,      SIZE(zs_format,    ZS,  1));
      CASE(ACCUM_RED_SIZE,    SIZE(accum_format, RGB, 0));
      CASE(ACCUM_GREEN_SIZE,  SIZE(accum_format, RGB, 1));
      CASE(ACCUM_BLUE_SIZE,   SIZE(accum_format, RGB, 2));
      CASE(ACCUM_ALPHA_SIZE,  SIZE(accum_format, RGB, 3));

      CASE(RENDER_TYPE, __DRI_ATTRIB_RGBA_BIT);
      CASE(CONFORMANT, GL_TRUE);
      CASE(DOUBLE_BUFFER, GL_TRUE);

      CASE(TRANSPARENT_TYPE,        GLX_NONE);
      CASE(TRANSPARENT_INDEX_VALUE, GLX_NONE);
      CASE(TRANSPARENT_RED_VALUE,   GLX_DONT_CARE);
      CASE(TRANSPARENT_GREEN_VALUE, GLX_DONT_CARE);
      CASE(TRANSPARENT_BLUE_VALUE,  GLX_DONT_CARE);
      CASE(TRANSPARENT_ALPHA_VALUE, GLX_DONT_CARE);

      CASE(RED_MASK,   MASK(color_format, RGB, 0));
      CASE(GREEN_MASK, MASK(color_format, RGB, 1));
      CASE(BLUE_MASK,  MASK(color_format, RGB, 2));
      CASE(ALPHA_MASK, MASK(color_format, RGB, 3));

      CASE(SWAP_METHOD, __DRI_ATTRIB_SWAP_UNDEFINED);
      CASE(MAX_SWAP_INTERVAL, INT_MAX);
      CASE(BIND_TO_TEXTURE_RGB, GL_TRUE);
      CASE(BIND_TO_TEXTURE_RGBA, GL_TRUE);
      CASE(BIND_TO_TEXTURE_TARGETS,
           __DRI_ATTRIB_TEXTURE_1D_BIT |
           __DRI_ATTRIB_TEXTURE_2D_BIT |
           __DRI_ATTRIB_TEXTURE_RECTANGLE_BIT);
      CASE(YINVERTED, GL_TRUE);

      CASE(RED_SHIFT,   SHIFT(color_format, RGB, 0));
      CASE(GREEN_SHIFT, SHIFT(color_format, RGB, 1));
      CASE(BLUE_SHIFT,  SHIFT(color_format, RGB, 2));
      CASE(ALPHA_SHIFT, SHIFT(color_format, RGB, 3));

      default:
         *value = 0;
         break;
   }

   *attrib = index;
   return 1;
}

static void
drilDestroyScreen(__DRIscreen *screen)
{
   /* At the moment this is just the bounce table for the configs */
   free(screen);
}

/* This has to return a pointer to NULL, not just NULL */
static const __DRIextension **
drilGetExtensions(__DRIscreen *screen)
{
   static const __DRIextension *nothing = NULL;
   return &nothing;
}

static void
drilDestroyDrawable(__DRIdrawable *draw)
{
}

static __DRIcontext *
drilCreateNewContext(__DRIscreen *screen, const __DRIconfig *config,
                     __DRIcontext *shared, void *data)
{
   return NULL;
}

static const __DRIcoreExtension drilCoreExtension = {
   .base = { __DRI_CORE, 1 },

   .destroyScreen       = drilDestroyScreen,
   .getExtensions       = drilGetExtensions,
   .getConfigAttrib     = NULL, // XXX not actually used!
   .indexConfigAttrib   = drilIndexConfigAttrib,
   .destroyDrawable     = drilDestroyDrawable,
   .createNewContext    = drilCreateNewContext,
};

static __DRIscreen *
drilCreateNewScreen(int screen, const __DRIextension **extensions,
                    const __DRIconfig ***driver_configs,
                    void *loaderPrivate)
{
   // allocate an array of pointers
   const __DRIconfig **configs = calloc(ARRAY_SIZE(drilConfigs), sizeof(void *));
   // set them to point to our config list
   for (int i = 0; i < ARRAY_SIZE(drilConfigs); i++)
      configs[i] = (const __DRIconfig *) &(drilConfigs[i]);
   // outpointer it
   *driver_configs = configs;

   // XXX and also return it as our screen state, so we can clean it up in
   // destroyScreen. If we had any additional screen state we'd need to do
   // something less hacky.
   return (void *) configs;
}

static __DRIdrawable *
drilSWCreateNewDrawable(__DRIscreen *screen, const __DRIconfig *config,
                        void *loaderPrivate)
{
   return (void *)1;
}

static const __DRIswrastExtension drilSWRastExtension = {
   .base = { __DRI_SWRAST, 1 },
   
   .createNewScreen = drilCreateNewScreen,
   .createNewDrawable = drilSWCreateNewDrawable,
};

const __DRIextension *__driDriverExtensions[] = {
   &drilCoreExtension.base,
   &drilSWRastExtension.base,
   // &drilDRI2Extension.base,
};

#if 0
const __DRIextension **
__driDriverGetExtensions_swrast(void)
{
   return NULL;
}
#endif
