/**************************************************************************
 *
 * Copyright 2009, VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Author: Keith Whitwell <keithw@vmware.com>
 * Author: Jakob Bornecrantz <wallbraker@gmail.com>
 */

/* XXX DRI1 is untested after the switch to st_api.h */

#include "util/u_memory.h"
#include "util/u_rect.h"
#include "util/u_surface.h"
#include "util/u_inlines.h"
#include "pipe/p_context.h"
#include "state_tracker/dri1_api.h"

#include "dri_screen.h"
#include "dri_context.h"
#include "dri_drawable.h"
#include "dri1_helper.h"
#include "dri1.h"

static INLINE void
dri1_lock(struct dri_context *ctx)
{
   drm_context_t hw_context = ctx->cPriv->hHWContext;
   char ret = 0;

   DRM_CAS(ctx->lock, hw_context, DRM_LOCK_HELD | hw_context, ret);
   if (ret) {
      drmGetLock(ctx->sPriv->fd, hw_context, 0);
      ctx->stLostLock = TRUE;
      ctx->wsLostLock = TRUE;
   }
   ctx->isLocked = TRUE;
}

static INLINE void
dri1_unlock(struct dri_context *ctx)
{
   ctx->isLocked = FALSE;
   DRM_UNLOCK(ctx->sPriv->fd, ctx->lock, ctx->cPriv->hHWContext);
}

static void
dri1_update_drawables_locked(struct dri_context *ctx,
			     __DRIdrawable * driDrawPriv,
			     __DRIdrawable * driReadPriv)
{
   if (ctx->stLostLock) {
      ctx->stLostLock = FALSE;
      if (driDrawPriv == driReadPriv)
	 DRI_VALIDATE_DRAWABLE_INFO(ctx->sPriv, driDrawPriv);
      else
	 DRI_VALIDATE_TWO_DRAWABLES_INFO(ctx->sPriv, driDrawPriv,
					 driReadPriv);
   }
}

/**
 * This ensures all contexts which bind to a drawable pick up the
 * drawable change and signal new buffer state.
 */
static void
dri1_propagate_drawable_change(struct dri_context *ctx)
{
   __DRIdrawable *dPriv = ctx->dPriv;
   __DRIdrawable *rPriv = ctx->rPriv;
   struct dri_drawable *draw;
   struct dri_drawable *read;
   boolean flushed = FALSE;

   if (dPriv) {
      draw = dri_drawable(dPriv);
   }

   if (rPriv) {
      read = dri_drawable(rPriv);
   }

   if (dPriv && draw->texture_stamp != dPriv->lastStamp) {
      ctx->st->flush(ctx->st, PIPE_FLUSH_RENDER_CACHE, NULL);
      flushed = TRUE;
      ctx->st->notify_invalid_framebuffer(ctx->st, &draw->base);
   }

   if (rPriv && dPriv != rPriv && read->texture_stamp != rPriv->lastStamp) {
      if (!flushed)
	 ctx->st->flush(ctx->st, PIPE_FLUSH_RENDER_CACHE, NULL);
      ctx->st->notify_invalid_framebuffer(ctx->st, &read->base);
   }
}

static INLINE boolean
dri1_intersect_src_bbox(struct drm_clip_rect *dst,
			int dst_x,
			int dst_y,
			const struct drm_clip_rect *src,
			const struct drm_clip_rect *bbox)
{
   int xy1;
   int xy2;

   xy1 = ((int)src->x1 > (int)bbox->x1 + dst_x) ? src->x1 :
      (int)bbox->x1 + dst_x;
   xy2 = ((int)src->x2 < (int)bbox->x2 + dst_x) ? src->x2 :
      (int)bbox->x2 + dst_x;
   if (xy1 >= xy2 || xy1 < 0)
      return FALSE;

   dst->x1 = xy1;
   dst->x2 = xy2;

   xy1 = ((int)src->y1 > (int)bbox->y1 + dst_y) ? src->y1 :
      (int)bbox->y1 + dst_y;
   xy2 = ((int)src->y2 < (int)bbox->y2 + dst_y) ? src->y2 :
      (int)bbox->y2 + dst_y;
   if (xy1 >= xy2 || xy1 < 0)
      return FALSE;

   dst->y1 = xy1;
   dst->y2 = xy2;
   return TRUE;
}

static void
dri1_swap_copy(struct pipe_context *pipe,
	       struct pipe_surface *dst,
	       struct pipe_surface *src,
	       __DRIdrawable * dPriv, const struct drm_clip_rect *bbox)
{
   struct drm_clip_rect clip;
   struct drm_clip_rect *cur;
   int i;

   cur = dPriv->pClipRects;

   for (i = 0; i < dPriv->numClipRects; ++i) {
      if (dri1_intersect_src_bbox(&clip, dPriv->x, dPriv->y, cur++, bbox)) {
         if (pipe->surface_copy) {
            pipe->surface_copy(pipe, dst, clip.x1, clip.y1,
                               src,
                               (int)clip.x1 - dPriv->x,
                               (int)clip.y1 - dPriv->y,
                               clip.x2 - clip.x1, clip.y2 - clip.y1);
         } else {
            util_surface_copy(pipe, FALSE, dst, clip.x1, clip.y1,
                              src,
                              (int)clip.x1 - dPriv->x,
                              (int)clip.y1 - dPriv->y,
                              clip.x2 - clip.x1, clip.y2 - clip.y1);
         }
      }
   }
}

static void
dri1_present_texture_locked(__DRIdrawable * dPriv,
                            struct pipe_resource *ptex,
                            const struct drm_clip_rect *sub_box,
                            struct pipe_fence_handle **fence)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct dri_screen *screen = dri_screen(drawable->sPriv);
   struct pipe_context *pipe;
   struct pipe_surface *psurf;
   struct drm_clip_rect bbox;
   boolean visible = TRUE;

   *fence = NULL;

   bbox.x1 = 0;
   bbox.x2 = ptex->width0;
   bbox.y1 = 0;
   bbox.y2 = ptex->height0;

   if (sub_box)
      visible = dri1_intersect_src_bbox(&bbox, 0, 0, &bbox, sub_box);
   if (!visible)
      return;

   pipe = dri1_get_pipe_context(screen);
   psurf = dri1_get_pipe_surface(drawable, ptex);
   if (!pipe || !psurf)
      return;

   if (__dri1_api_hooks->present_locked) {
      __dri1_api_hooks->present_locked(pipe, psurf,
                                       dPriv->pClipRects, dPriv->numClipRects,
                                       dPriv->x, dPriv->y, &bbox, fence);
   } else if (__dri1_api_hooks->front_srf_locked) {
      struct pipe_surface *front = __dri1_api_hooks->front_srf_locked(pipe);

      if (front)
         dri1_swap_copy(pipe, front, psurf, dPriv, &bbox);

      pipe->flush(pipe, PIPE_FLUSH_RENDER_CACHE, fence);
   }
}

static void
dri1_copy_to_front(struct dri_context *ctx,
		   struct pipe_resource *ptex,
		   __DRIdrawable * dPriv,
		   const struct drm_clip_rect *sub_box,
		   struct pipe_fence_handle **fence)
{
   boolean save_lost_lock;

   dri1_lock(ctx);
   save_lost_lock = ctx->stLostLock;
   dri1_update_drawables_locked(ctx, dPriv, dPriv);

   dri1_present_texture_locked(dPriv, ptex, sub_box, fence);

   ctx->stLostLock = save_lost_lock;

   /**
    * FIXME: Revisit this: Update drawables on copy_sub_buffer ?
    */

   if (!sub_box)
      dri1_update_drawables_locked(ctx, ctx->dPriv, ctx->rPriv);

   dri1_unlock(ctx);
   dri1_propagate_drawable_change(ctx);
}

/*
 * Backend functions for st_framebuffer interface and swap_buffers.
 */

static void
dri1_flush_frontbuffer(struct dri_drawable *draw,
                       enum st_attachment_type statt)
{
   struct dri_context *ctx = dri_get_current(draw->sPriv);
   struct dri_screen *screen = dri_screen(draw->sPriv);
   struct pipe_screen *pipe_screen = screen->base.screen;
   struct pipe_fence_handle *dummy_fence;
   struct pipe_resource *ptex;

   if (!ctx)
      return;			       /* For now */

   ptex = draw->textures[statt];
   if (ptex) {
      dri1_copy_to_front(ctx, ptex, ctx->dPriv, NULL, &dummy_fence);
      pipe_screen->fence_reference(pipe_screen, &dummy_fence, NULL);
   }

   /**
    * FIXME: Do we need swap throttling here?
    */
}

void
dri1_swap_buffers(__DRIdrawable * dPriv)
{
   struct dri_drawable *draw = dri_drawable(dPriv);
   struct dri_context *ctx = dri_get_current(draw->sPriv);
   struct dri_screen *screen = dri_screen(draw->sPriv);
   struct pipe_screen *pipe_screen = screen->base.screen;
   struct pipe_fence_handle *fence;
   struct pipe_resource *ptex;

   assert(__dri1_api_hooks != NULL);

   if (!ctx)
      return;			       /* For now */

   ptex = draw->textures[ST_ATTACHMENT_BACK_LEFT];
   if (ptex) {
      ctx->st->flush(ctx->st, PIPE_FLUSH_RENDER_CACHE, NULL);
      fence = dri1_swap_fences_pop_front(draw);
      if (fence) {
	 (void)pipe_screen->fence_finish(pipe_screen, fence, 0);
	 pipe_screen->fence_reference(pipe_screen, &fence, NULL);
      }
      dri1_copy_to_front(ctx, ptex, dPriv, NULL, &fence);
      dri1_swap_fences_push_back(draw, fence);
      pipe_screen->fence_reference(pipe_screen, &fence, NULL);
   }
}

void
dri1_copy_sub_buffer(__DRIdrawable * dPriv, int x, int y, int w, int h)
{
   struct dri_context *ctx = dri_get_current(dPriv->driScreenPriv);
   struct dri_screen *screen = dri_screen(dPriv->driScreenPriv);
   struct pipe_screen *pipe_screen = screen->base.screen;
   struct drm_clip_rect sub_bbox;
   struct dri_drawable *draw = dri_drawable(dPriv);
   struct pipe_fence_handle *dummy_fence;
   struct pipe_resource *ptex;

   assert(__dri1_api_hooks != NULL);

   if (!ctx)
      return;

   sub_bbox.x1 = x;
   sub_bbox.x2 = x + w;
   sub_bbox.y1 = y;
   sub_bbox.y2 = y + h;

   ptex = draw->textures[ST_ATTACHMENT_BACK_LEFT];
   if (ptex) {
      ctx->st->flush(ctx->st, PIPE_FLUSH_RENDER_CACHE, NULL);
      dri1_copy_to_front(ctx, ptex, dPriv, &sub_bbox, &dummy_fence);
      pipe_screen->fence_reference(pipe_screen, &dummy_fence, NULL);
   }
}

/**
 * Allocate framebuffer attachments.
 *
 * During fixed-size operation, the function keeps allocating new attachments
 * as they are requested. Unused attachments are not removed, not until the
 * framebuffer is resized or destroyed.
 */
static void
dri1_allocate_textures(struct dri_drawable *drawable,
                       const enum st_attachment_type *statts,
                       unsigned count)
{
   struct dri_screen *screen = dri_screen(drawable->sPriv);
   struct pipe_resource templ;
   unsigned width, height;
   boolean resized;
   int i;

   width  = drawable->dPriv->w;
   height = drawable->dPriv->h;

   resized = (drawable->old_w != width ||
              drawable->old_h != height);

   /* remove outdated textures */
   if (resized) {
      for (i = 0; i < ST_ATTACHMENT_COUNT; i++)
         pipe_resource_reference(&drawable->textures[i], NULL);
   }

   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.last_level = 0;

   for (i = 0; i < count; i++) {
      enum pipe_format format;
      unsigned bind;

      /* the texture already exists */
      if (drawable->textures[statts[i]])
         continue;

      dri_drawable_get_format(drawable, statts[i], &format, &bind);

      if (format == PIPE_FORMAT_NONE)
         continue;

      templ.format = format;
      templ.bind = bind;

      drawable->textures[statts[i]] =
         screen->base.screen->resource_create(screen->base.screen, &templ);
   }

   drawable->old_w = width;
   drawable->old_h = height;
}

/*
 * Backend function for init_screen.
 */

static const __DRIextension *dri1_screen_extensions[] = {
   &driReadDrawableExtension,
   &driCopySubBufferExtension.base,
   &driSwapControlExtension.base,
   &driFrameTrackingExtension.base,
   &driMediaStreamCounterExtension.base,
   NULL
};

static void
st_dri_lock(struct pipe_context *pipe)
{
   dri1_lock((struct dri_context *)pipe->priv);
}

static void
st_dri_unlock(struct pipe_context *pipe)
{
   dri1_unlock((struct dri_context *)pipe->priv);
}

static boolean
st_dri_is_locked(struct pipe_context *pipe)
{
   return ((struct dri_context *)pipe->priv)->isLocked;
}

static boolean
st_dri_lost_lock(struct pipe_context *pipe)
{
   return ((struct dri_context *)pipe->priv)->wsLostLock;
}

static void
st_dri_clear_lost_lock(struct pipe_context *pipe)
{
   ((struct dri_context *)pipe->priv)->wsLostLock = FALSE;
}

static struct dri1_api_lock_funcs dri1_lf = {
   .lock = st_dri_lock,
   .unlock = st_dri_unlock,
   .is_locked = st_dri_is_locked,
   .is_lock_lost = st_dri_lost_lock,
   .clear_lost_lock = st_dri_clear_lost_lock
};

static INLINE void
dri1_copy_version(struct dri1_api_version *dst,
		  const struct __DRIversionRec *src)
{
   dst->major = src->major;
   dst->minor = src->minor;
   dst->patch_level = src->patch;
}

struct dri1_api *__dri1_api_hooks = NULL;

const __DRIconfig **
dri1_init_screen(__DRIscreen * sPriv)
{
   const __DRIconfig **configs;
   struct pipe_screen *pscreen;
   struct dri_screen *screen;
   struct dri1_create_screen_arg arg;

   screen = CALLOC_STRUCT(dri_screen);
   if (!screen)
      return NULL;

   screen->api = drm_api_create();
   screen->sPriv = sPriv;
   screen->fd = sPriv->fd;
   screen->drmLock = (drmLock *) & sPriv->pSAREA->lock;
   screen->allocate_textures = dri1_allocate_textures;
   screen->flush_frontbuffer = dri1_flush_frontbuffer;

   sPriv->private = (void *)screen;
   sPriv->extensions = dri1_screen_extensions;

   arg.base.mode = DRM_CREATE_DRI1;
   arg.lf = &dri1_lf;
   arg.ddx_info = sPriv->pDevPriv;
   arg.ddx_info_size = sPriv->devPrivSize;
   arg.sarea = sPriv->pSAREA;
   dri1_copy_version(&arg.ddx_version, &sPriv->ddx_version);
   dri1_copy_version(&arg.dri_version, &sPriv->dri_version);
   dri1_copy_version(&arg.drm_version, &sPriv->drm_version);
   arg.api = NULL;

   /**
    * FIXME: If the driver supports format conversion swapbuffer blits, we might
    * want to support other color bit depths than the server is currently
    * using.
    */

   pscreen = screen->api->create_screen(screen->api, screen->fd, &arg.base);
   /* dri_init_screen_helper checks pscreen for us */

   configs = dri_init_screen_helper(screen, pscreen, sPriv->fbBPP);
   if (!configs)
      goto fail;

   if (!arg.api) {
      debug_printf("%s: failed to create dri1 screen\n", __FUNCTION__);
      goto fail;
   }

   __dri1_api_hooks = arg.api;

   return configs;
fail:
   if (configs)
      FREE(configs);
   dri_destroy_screen_helper(screen);
   FREE(screen);
   return NULL;
}
