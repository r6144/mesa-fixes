/*
 * Mesa 3-D graphics library
 * Version:  7.9
 *
 * Copyright (C) 2010 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "util/u_memory.h"
#include "util/u_string.h"
#include "util/u_inlines.h"
#include "util/u_dl.h"
#include "egldriver.h"
#include "eglimage.h"
#include "eglmutex.h"

#include "egl_g3d.h"
#include "egl_g3d_st.h"

struct egl_g3d_st_manager {
   struct st_manager base;
   _EGLDisplay *display;
};

static INLINE struct egl_g3d_st_manager *
egl_g3d_st_manager(struct st_manager *smapi)
{
   return (struct egl_g3d_st_manager *) smapi;
}

static struct egl_g3d_st_module {
   const char *filename;
   struct util_dl_library *lib;
   struct st_api *stapi;
} egl_g3d_st_modules[ST_API_COUNT];

static EGLBoolean
egl_g3d_search_path_callback(const char *dir, size_t len, void *callback_data)
{
   struct egl_g3d_st_module *stmod =
      (struct egl_g3d_st_module *) callback_data;
   char path[1024];
   int ret;

   ret = util_snprintf(path, sizeof(path),
         "%.*s/%s", len, dir, stmod->filename);
   if (ret > 0 && ret < sizeof(path))
      stmod->lib = util_dl_open(path);

   return !(stmod->lib);
}

static boolean
egl_g3d_load_st_module(struct egl_g3d_st_module *stmod,
                       const char *filename, const char *procname)
{
   struct st_api *(*create_api)(void);

   stmod->filename = filename;
   if (stmod->filename)
      _eglSearchPathForEach(egl_g3d_search_path_callback, (void *) stmod);
   else
      stmod->lib = util_dl_open(NULL);

   if (stmod->lib) {
      create_api = (struct st_api *(*)(void))
         util_dl_get_proc_address(stmod->lib, procname);
      if (create_api)
         stmod->stapi = create_api();

      if (!stmod->stapi) {
         util_dl_close(stmod->lib);
         stmod->lib = NULL;
      }
   }

   if (stmod->stapi) {
      return TRUE;
   }
   else {
      stmod->filename = NULL;
      return FALSE;
   }
}

void
egl_g3d_init_st_apis(struct st_api *stapis[ST_API_COUNT])
{
   const char *skip_checks[ST_API_COUNT], *symbols[ST_API_COUNT];
   const char *filenames[ST_API_COUNT][4];
   struct util_dl_library *self;
   int num_needed = 0, api;

   self = util_dl_open(NULL);

   /* collect the necessary data for loading modules */
   for (api = 0; api < ST_API_COUNT; api++) {
      int count = 0;

      switch (api) {
      case ST_API_OPENGL:
         skip_checks[api] = "glColor4d";
         symbols[api] = ST_CREATE_OPENGL_SYMBOL;
         filenames[api][count++] = "api_GL.so";
         break;
      case ST_API_OPENGL_ES1:
         skip_checks[api] = "glColor4x";
         symbols[api] = ST_CREATE_OPENGL_ES1_SYMBOL;
         filenames[api][count++] = "api_GLESv1_CM.so";
         filenames[api][count++] = "api_GL.so";
         break;
      case ST_API_OPENGL_ES2:
         skip_checks[api] = "glShaderBinary";
         symbols[api] = ST_CREATE_OPENGL_ES2_SYMBOL;
         filenames[api][count++] = "api_GLESv2.so";
         filenames[api][count++] = "api_GL.so";
         break;
      case ST_API_OPENVG:
         skip_checks[api] = "vgClear";
         symbols[api] = ST_CREATE_OPENVG_SYMBOL;
         filenames[api][count++]= "api_OpenVG.so";
         break;
      default:
         assert(!"Unknown API Type\n");
         skip_checks[api] = NULL;
         symbols[api] = NULL;
         break;
      }
      filenames[api][count++]= NULL;
      assert(count < Elements(filenames[api]));

      /* heuristicically decide if the module is needed */
      if (!self || !skip_checks[api] ||
          util_dl_get_proc_address(self, skip_checks[api])) {
         /* unset so the module is not skipped */
         skip_checks[api] = NULL;
         num_needed++;
      }
   }
   /* mark all moudles needed if we wrongly decided that none is needed */
   if (!num_needed)
      memset(skip_checks, 0, sizeof(skip_checks));

   if (self)
      util_dl_close(self);

   for (api = 0; api < ST_API_COUNT; api++) {
      struct egl_g3d_st_module *stmod = &egl_g3d_st_modules[api];
      const char **p;

      /* skip the module */
      if (skip_checks[api])
         continue;

      /* try all filenames, including NULL */
      for (p = filenames[api]; *p; p++) {
         if (egl_g3d_load_st_module(stmod, *p, symbols[api]))
            break;
      }
      if (!stmod->stapi)
         egl_g3d_load_st_module(stmod, NULL, symbols[api]);

      stapis[api] = stmod->stapi;
   }
}

void
egl_g3d_destroy_st_apis(void)
{
   int api;

   for (api = 0; api < ST_API_COUNT; api++) {
      struct egl_g3d_st_module *stmod = &egl_g3d_st_modules[api];

      if (stmod->stapi) {
         stmod->stapi->destroy(stmod->stapi);
         stmod->stapi = NULL;
      }
      if (stmod->lib) {
         util_dl_close(stmod->lib);
         stmod->lib = NULL;
      }
      stmod->filename = NULL;
   }
}

static boolean
egl_g3d_st_manager_get_egl_image(struct st_manager *smapi,
                                 struct st_egl_image *stimg)
{
   struct egl_g3d_st_manager *gsmapi = egl_g3d_st_manager(smapi);
   EGLImageKHR handle = (EGLImageKHR) stimg->egl_image;
   _EGLImage *img;
   struct egl_g3d_image *gimg;

   /* this is called from state trackers */
   _eglLockMutex(&gsmapi->display->Mutex);

   img = _eglLookupImage(handle, gsmapi->display);
   if (!img) {
      _eglUnlockMutex(&gsmapi->display->Mutex);
      return FALSE;
   }

   gimg = egl_g3d_image(img);

   stimg->texture = NULL;
   pipe_resource_reference(&stimg->texture, gimg->texture);
   stimg->face = gimg->face;
   stimg->level = gimg->level;
   stimg->zslice = gimg->zslice;

   _eglUnlockMutex(&gsmapi->display->Mutex);

   return TRUE;
}

struct st_manager *
egl_g3d_create_st_manager(_EGLDisplay *dpy)
{
   struct egl_g3d_display *gdpy = egl_g3d_display(dpy);
   struct egl_g3d_st_manager *gsmapi;

   gsmapi = CALLOC_STRUCT(egl_g3d_st_manager);
   if (gsmapi) {
      gsmapi->display = dpy;

      gsmapi->base.screen = gdpy->native->screen;
      gsmapi->base.get_egl_image = egl_g3d_st_manager_get_egl_image;
   }

   return &gsmapi->base;;
}

void
egl_g3d_destroy_st_manager(struct st_manager *smapi)
{
   struct egl_g3d_st_manager *gsmapi = egl_g3d_st_manager(smapi);
   FREE(gsmapi);
}

static boolean
egl_g3d_st_framebuffer_flush_front_pbuffer(struct st_framebuffer_iface *stfbi,
                                           enum st_attachment_type statt)
{
   return TRUE;
}

static boolean 
egl_g3d_st_framebuffer_validate_pbuffer(struct st_framebuffer_iface *stfbi,
                                        const enum st_attachment_type *statts,
                                        unsigned count,
                                        struct pipe_resource **out)
{
   _EGLSurface *surf = (_EGLSurface *) stfbi->st_manager_private;
   struct egl_g3d_surface *gsurf = egl_g3d_surface(surf);
   struct pipe_resource templ;
   unsigned i;

   for (i = 0; i < count; i++) {
      out[i] = NULL;

      if (gsurf->stvis.render_buffer != statts[i])
         continue;

      if (!gsurf->render_texture) {
         struct egl_g3d_display *gdpy =
            egl_g3d_display(gsurf->base.Resource.Display);
         struct pipe_screen *screen = gdpy->native->screen;

         memset(&templ, 0, sizeof(templ));
         templ.target = PIPE_TEXTURE_2D;
         templ.last_level = 0;
         templ.width0 = gsurf->base.Width;
         templ.height0 = gsurf->base.Height;
         templ.depth0 = 1;
         templ.format = gsurf->stvis.color_format;
         templ.bind = PIPE_BIND_RENDER_TARGET;

         gsurf->render_texture = screen->resource_create(screen, &templ);
      }

      pipe_resource_reference(&out[i], gsurf->render_texture);
   }

   return TRUE;
}

static boolean
egl_g3d_st_framebuffer_flush_front(struct st_framebuffer_iface *stfbi,
                                   enum st_attachment_type statt)
{
   _EGLSurface *surf = (_EGLSurface *) stfbi->st_manager_private;
   struct egl_g3d_surface *gsurf = egl_g3d_surface(surf);

   return gsurf->native->flush_frontbuffer(gsurf->native);
}

static boolean 
egl_g3d_st_framebuffer_validate(struct st_framebuffer_iface *stfbi,
                                const enum st_attachment_type *statts,
                                unsigned count,
                                struct pipe_resource **out)
{
   _EGLSurface *surf = (_EGLSurface *) stfbi->st_manager_private;
   struct egl_g3d_surface *gsurf = egl_g3d_surface(surf);
   struct pipe_resource *textures[NUM_NATIVE_ATTACHMENTS];
   uint attachment_mask = 0;
   unsigned i;

   for (i = 0; i < count; i++) {
      int natt;

      switch (statts[i]) {
      case ST_ATTACHMENT_FRONT_LEFT:
         natt = NATIVE_ATTACHMENT_FRONT_LEFT;
         break;
      case ST_ATTACHMENT_BACK_LEFT:
         natt = NATIVE_ATTACHMENT_BACK_LEFT;
         break;
      case ST_ATTACHMENT_FRONT_RIGHT:
         natt = NATIVE_ATTACHMENT_FRONT_RIGHT;
         break;
      case ST_ATTACHMENT_BACK_RIGHT:
         natt = NATIVE_ATTACHMENT_BACK_RIGHT;
         break;
      default:
         natt = -1;
         break;
      }

      if (natt >= 0)
         attachment_mask |= 1 << natt;
   }

   if (!gsurf->native->validate(gsurf->native, attachment_mask,
         &gsurf->sequence_number, textures, &gsurf->base.Width,
         &gsurf->base.Height))
      return FALSE;

   for (i = 0; i < count; i++) {
      struct pipe_resource *tex;
      int natt;

      switch (statts[i]) {
      case ST_ATTACHMENT_FRONT_LEFT:
         natt = NATIVE_ATTACHMENT_FRONT_LEFT;
         break;
      case ST_ATTACHMENT_BACK_LEFT:
         natt = NATIVE_ATTACHMENT_BACK_LEFT;
         break;
      case ST_ATTACHMENT_FRONT_RIGHT:
         natt = NATIVE_ATTACHMENT_FRONT_RIGHT;
         break;
      case ST_ATTACHMENT_BACK_RIGHT:
         natt = NATIVE_ATTACHMENT_BACK_RIGHT;
         break;
      default:
         natt = -1;
         break;
      }

      if (natt >= 0) {
         tex = textures[natt];

         if (statts[i] == stfbi->visual->render_buffer)
            pipe_resource_reference(&gsurf->render_texture, tex);

         if (attachment_mask & (1 << natt)) {
            /* transfer the ownership to the caller */
            out[i] = tex;
            attachment_mask &= ~(1 << natt);
         }
         else {
            /* the attachment is listed more than once */
            pipe_resource_reference(&out[i], tex);
         }
      }
   }

   return TRUE;
}

struct st_framebuffer_iface *
egl_g3d_create_st_framebuffer(_EGLSurface *surf)
{
   struct egl_g3d_surface *gsurf = egl_g3d_surface(surf);
   struct st_framebuffer_iface *stfbi;

   stfbi = CALLOC_STRUCT(st_framebuffer_iface);
   if (!stfbi)
      return NULL;

   stfbi->visual = &gsurf->stvis;
   if (gsurf->base.Type != EGL_PBUFFER_BIT) {
      stfbi->flush_front = egl_g3d_st_framebuffer_flush_front;
      stfbi->validate = egl_g3d_st_framebuffer_validate;
   }
   else {
      stfbi->flush_front = egl_g3d_st_framebuffer_flush_front_pbuffer;
      stfbi->validate = egl_g3d_st_framebuffer_validate_pbuffer;
   }
   stfbi->st_manager_private = (void *) &gsurf->base;

   return stfbi;
}

void
egl_g3d_destroy_st_framebuffer(struct st_framebuffer_iface *stfbi)
{
   FREE(stfbi);
}
