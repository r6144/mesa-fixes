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


#include "lp_context.h"
#include "lp_state.h"
#include "pipe/p_shader_tokens.h"

static void
lp_push_quad_first( struct llvmpipe_context *lp,
                    struct quad_stage *quad )
{
   quad->next = lp->quad.first;
   lp->quad.first = quad;
}


void
lp_build_quad_pipeline(struct llvmpipe_context *lp)
{
   boolean early_depth_test =
      lp->depth_stencil->depth.enabled &&
      lp->framebuffer.zsbuf &&
      !lp->depth_stencil->alpha.enabled &&
      !lp->fs->info.uses_kill &&
      !lp->fs->info.writes_z;

   lp->quad.first = lp->quad.blend;

   if (early_depth_test) {
      lp_push_quad_first( lp, lp->quad.shade );
      lp_push_quad_first( lp, lp->quad.depth_test );
   }
   else {
      lp_push_quad_first( lp, lp->quad.depth_test );
      lp_push_quad_first( lp, lp->quad.shade );
   }
}
