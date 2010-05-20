/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
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
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/* Author:
 *    Keith Whitwell <keith@tungstengraphics.com>
 */


#include "pipe/p_defines.h"
#include "util/u_string.h"
#include "draw/draw_context.h"
#include "lp_flush.h"
#include "lp_context.h"
#include "lp_setup.h"


/**
 * \param flags  bitmask of PIPE_FLUSH_x flags
 * \param fence  if non-null, returns pointer to a fench which can be waited on
 */
void
llvmpipe_flush( struct pipe_context *pipe,
		unsigned flags,
                struct pipe_fence_handle **fence )
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   draw_flush(llvmpipe->draw);

   if (fence) {
      if ((flags & (PIPE_FLUSH_SWAPBUFFERS |
                    PIPE_FLUSH_RENDER_CACHE))) {
         /* if we're going to flush the setup/rasterization modules, emit
          * a fence.
          * XXX this (and the code below) may need fine tuning...
          */
         *fence = lp_setup_fence( llvmpipe->setup );
      }
      else {
         *fence = NULL;
      }
   }

   /* ask the setup module to flush */
   if (flags & (PIPE_FLUSH_SWAPBUFFERS | PIPE_FLUSH_RENDER_CACHE |
                PIPE_FLUSH_TEXTURE_CACHE)) {
      lp_setup_flush(llvmpipe->setup, flags);
   }

   /* Enable to dump BMPs of the color/depth buffers each frame */
   if (0) {
      if (flags & PIPE_FLUSH_FRAME) {
         static unsigned frame_no = 1;
         char filename[256];
         unsigned i;

         for (i = 0; i < llvmpipe->framebuffer.nr_cbufs; i++) {
            util_snprintf(filename, sizeof(filename), "cbuf%u_%u", i, frame_no);
            debug_dump_surface_bmp(&llvmpipe->pipe, filename, llvmpipe->framebuffer.cbufs[0]);
         }

         if (0) {
            util_snprintf(filename, sizeof(filename), "zsbuf_%u", frame_no);
            debug_dump_surface_bmp(&llvmpipe->pipe, filename, llvmpipe->framebuffer.zsbuf);
         }

         ++frame_no;
      }
   }
}


/**
 * Flush context if necessary.
 *
 * Returns FALSE if it would have block, but do_not_block was set, TRUE
 * otherwise.
 *
 * TODO: move this logic to an auxiliary library?
 */
boolean
llvmpipe_flush_resource(struct pipe_context *pipe,
                        struct pipe_resource *resource,
                        unsigned face,
                        unsigned level,
                        unsigned flush_flags,
                        boolean read_only,
                        boolean cpu_access,
                        boolean do_not_block)
{
   unsigned referenced;

   referenced = pipe->is_resource_referenced(pipe, resource, face, level);

   if ((referenced & PIPE_REFERENCED_FOR_WRITE) ||
       ((referenced & PIPE_REFERENCED_FOR_READ) && !read_only)) {

      if (resource->target != PIPE_BUFFER) {
         /*
          * TODO: The semantics of these flush flags are too obtuse. They should
          * disappear and the pipe driver should just ensure that all visible
          * side-effects happen when they need to happen.
          */
         if (referenced & PIPE_REFERENCED_FOR_WRITE)
            flush_flags |= PIPE_FLUSH_RENDER_CACHE;

         if (referenced & PIPE_REFERENCED_FOR_READ)
            flush_flags |= PIPE_FLUSH_TEXTURE_CACHE;
      }

      if (cpu_access) {
         /*
          * Flush and wait.
          */

         struct pipe_fence_handle *fence = NULL;

         if (do_not_block)
            return FALSE;

         pipe->flush(pipe, flush_flags, &fence);

         if (fence) {
            pipe->screen->fence_finish(pipe->screen, fence, 0);
            pipe->screen->fence_reference(pipe->screen, &fence, NULL);
         }
      } else {
         /*
          * Just flush.
          */

         pipe->flush(pipe, flush_flags, NULL);
      }
   }

   return TRUE;
}
