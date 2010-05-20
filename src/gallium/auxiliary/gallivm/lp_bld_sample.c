/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
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

/**
 * @file
 * Texture sampling -- common code.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */

#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_format.h"
#include "util/u_math.h"
#include "lp_bld_debug.h"
#include "lp_bld_const.h"
#include "lp_bld_arit.h"
#include "lp_bld_type.h"
#include "lp_bld_format.h"
#include "lp_bld_sample.h"


/**
 * Initialize lp_sampler_static_state object with the gallium sampler
 * and texture state.
 * The former is considered to be static and the later dynamic.
 */
void
lp_sampler_static_state(struct lp_sampler_static_state *state,
                        const struct pipe_sampler_view *view,
                        const struct pipe_sampler_state *sampler)
{
   const struct pipe_resource *texture = view->texture;

   memset(state, 0, sizeof *state);

   if(!texture)
      return;

   if(!sampler)
      return;

   /*
    * We don't copy sampler state over unless it is actually enabled, to avoid
    * spurious recompiles, as the sampler static state is part of the shader
    * key.
    *
    * Ideally the state tracker or cso_cache module would make all state
    * canonical, but until that happens it's better to be safe than sorry here.
    *
    * XXX: Actually there's much more than can be done here, especially
    * regarding 1D/2D/3D/CUBE textures, wrap modes, etc.
    */

   state->format            = view->format;
   state->swizzle_r         = view->swizzle_r;
   state->swizzle_g         = view->swizzle_g;
   state->swizzle_b         = view->swizzle_b;
   state->swizzle_a         = view->swizzle_a;

   state->target            = texture->target;
   state->pot_width         = util_is_pot(texture->width0);
   state->pot_height        = util_is_pot(texture->height0);
   state->pot_depth         = util_is_pot(texture->depth0);

   state->wrap_s            = sampler->wrap_s;
   state->wrap_t            = sampler->wrap_t;
   state->wrap_r            = sampler->wrap_r;
   state->min_img_filter    = sampler->min_img_filter;
   state->mag_img_filter    = sampler->mag_img_filter;
   if (texture->last_level) {
      state->min_mip_filter = sampler->min_mip_filter;
   } else {
      state->min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
   }

   state->compare_mode      = sampler->compare_mode;
   if (sampler->compare_mode != PIPE_TEX_COMPARE_NONE) {
      state->compare_func   = sampler->compare_func;
   }

   state->normalized_coords = sampler->normalized_coords;
   state->lod_bias          = sampler->lod_bias;
   state->min_lod           = sampler->min_lod;
   state->max_lod           = sampler->max_lod;
   state->border_color[0]   = sampler->border_color[0];
   state->border_color[1]   = sampler->border_color[1];
   state->border_color[2]   = sampler->border_color[2];
   state->border_color[3]   = sampler->border_color[3];

   /*
    * FIXME: Handle the remainder of pipe_sampler_view.
    */
}


/**
 * Gather elements from scatter positions in memory into a single vector.
 * Use for fetching texels from a texture.
 * For SSE, typical values are length=4, src_width=32, dst_width=32.
 *
 * @param length length of the offsets
 * @param src_width src element width in bits
 * @param dst_width result element width in bits (src will be expanded to fit)
 * @param base_ptr base pointer, should be a i8 pointer type.
 * @param offsets vector with offsets
 */
LLVMValueRef
lp_build_gather(LLVMBuilderRef builder,
                unsigned length,
                unsigned src_width,
                unsigned dst_width,
                LLVMValueRef base_ptr,
                LLVMValueRef offsets)
{
   LLVMTypeRef src_type = LLVMIntType(src_width);
   LLVMTypeRef src_ptr_type = LLVMPointerType(src_type, 0);
   LLVMTypeRef dst_elem_type = LLVMIntType(dst_width);
   LLVMTypeRef dst_vec_type = LLVMVectorType(dst_elem_type, length);
   LLVMValueRef res;
   unsigned i;

   res = LLVMGetUndef(dst_vec_type);
   for(i = 0; i < length; ++i) {
      LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), i, 0);
      LLVMValueRef elem_offset;
      LLVMValueRef elem_ptr;
      LLVMValueRef elem;

      elem_offset = LLVMBuildExtractElement(builder, offsets, index, "");
      elem_ptr = LLVMBuildGEP(builder, base_ptr, &elem_offset, 1, "");
      elem_ptr = LLVMBuildBitCast(builder, elem_ptr, src_ptr_type, "");
      elem = LLVMBuildLoad(builder, elem_ptr, "");

      assert(src_width <= dst_width);
      if(src_width > dst_width)
         elem = LLVMBuildTrunc(builder, elem, dst_elem_type, "");
      if(src_width < dst_width)
         elem = LLVMBuildZExt(builder, elem, dst_elem_type, "");

      res = LLVMBuildInsertElement(builder, res, elem, index, "");
   }

   return res;
}


/**
 * Compute the offset of a pixel block.
 *
 * x, y, z, y_stride, z_stride are vectors, and they refer to pixel blocks, as
 * per format description, and not individual pixels.
 */
LLVMValueRef
lp_build_sample_offset(struct lp_build_context *bld,
                       const struct util_format_description *format_desc,
                       LLVMValueRef x,
                       LLVMValueRef y,
                       LLVMValueRef z,
                       LLVMValueRef y_stride,
                       LLVMValueRef z_stride)
{
   LLVMValueRef x_stride;
   LLVMValueRef offset;

   x_stride = lp_build_const_vec(bld->type, format_desc->block.bits/8);
   offset = lp_build_mul(bld, x, x_stride);

   if (y && y_stride) {
      LLVMValueRef y_offset = lp_build_mul(bld, y, y_stride);
      offset = lp_build_add(bld, offset, y_offset);
   }

   if (z && z_stride) {
      LLVMValueRef z_offset = lp_build_mul(bld, z, z_stride);
      offset = lp_build_add(bld, offset, z_offset);
   }

   return offset;
}
