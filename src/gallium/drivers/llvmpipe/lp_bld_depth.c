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
 * Depth/stencil testing to LLVM IR translation.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */

#include "pipe/p_state.h"
#include "util/u_format.h"

#include "lp_bld_type.h"
#include "lp_bld_const.h"
#include "lp_bld_logic.h"
#include "lp_bld_flow.h"
#include "lp_bld_debug.h"
#include "lp_bld_depth.h"


union lp_type
lp_depth_type(const struct util_format_description *format_desc,
              unsigned length)
{
   union lp_type type;
   unsigned swizzle;

   assert(format_desc->colorspace == UTIL_FORMAT_COLORSPACE_ZS);
   assert(format_desc->block.width == 1);
   assert(format_desc->block.height == 1);

   swizzle = format_desc->swizzle[0];
   assert(swizzle < 4);

   type.value = 0;
   type.width = format_desc->block.bits;

   if(format_desc->channel[swizzle].type == UTIL_FORMAT_TYPE_FLOAT) {
      type.floating = TRUE;
      assert(swizzle = 0);
      assert(format_desc->channel[swizzle].size == format_desc->block.bits);
   }
   else if(format_desc->channel[swizzle].type == UTIL_FORMAT_TYPE_UNSIGNED) {
      assert(format_desc->block.bits <= 32);
      if(format_desc->channel[swizzle].normalized)
         type.norm = TRUE;
   }
   else
      assert(0);

   assert(type.width <= length);
   type.length = length / type.width;

   return type;
}


void
lp_build_depth_test(LLVMBuilderRef builder,
                    const struct pipe_depth_state *state,
                    union lp_type type,
                    const struct util_format_description *format_desc,
                    struct lp_build_mask_context *mask,
                    LLVMValueRef src,
                    LLVMValueRef dst_ptr)
{
   struct lp_build_context bld;
   unsigned z_swizzle;
   LLVMValueRef dst;
   LLVMValueRef z_bitmask = NULL;
   LLVMValueRef test;

   if(!state->enabled)
      return;

   assert(format_desc->colorspace == UTIL_FORMAT_COLORSPACE_ZS);
   assert(format_desc->block.width == 1);
   assert(format_desc->block.height == 1);

   z_swizzle = format_desc->swizzle[0];
   if(z_swizzle == UTIL_FORMAT_SWIZZLE_NONE)
      return;

   /* Sanity checking */
   assert(z_swizzle < 4);
   assert(format_desc->block.bits == type.width);
   if(type.floating) {
      assert(z_swizzle == 0);
      assert(format_desc->channel[z_swizzle].type == UTIL_FORMAT_TYPE_FLOAT);
      assert(format_desc->channel[z_swizzle].size == format_desc->block.bits);
   }
   else {
      assert(format_desc->channel[z_swizzle].type == UTIL_FORMAT_TYPE_UNSIGNED);
      assert(format_desc->channel[z_swizzle].normalized);
      assert(!type.fixed);
      assert(!type.sign);
      assert(type.norm);
   }

   /* Setup build context */
   lp_build_context_init(&bld, builder, type);

   dst = LLVMBuildLoad(builder, dst_ptr, "");

   lp_build_name(dst, "zsbuf");

   /* Align the source depth bits with the destination's, and mask out any
    * stencil or padding bits from both */
   if(format_desc->channel[z_swizzle].size == format_desc->block.bits) {
      assert(z_swizzle == 0);
      /* nothing to do */
   }
   else {
      unsigned padding_left;
      unsigned padding_right;
      unsigned chan;

      assert(format_desc->layout == UTIL_FORMAT_LAYOUT_ARITH);
      assert(format_desc->channel[z_swizzle].type == UTIL_FORMAT_TYPE_UNSIGNED);
      assert(format_desc->channel[z_swizzle].size <= format_desc->block.bits);
      assert(format_desc->channel[z_swizzle].normalized);

      padding_right = 0;
      for(chan = 0; chan < z_swizzle; ++chan)
         padding_right += format_desc->channel[chan].size;
      padding_left = format_desc->block.bits - format_desc->channel[z_swizzle].size;

      if(padding_left || padding_right) {
         const long long mask_left = ((long long)1 << (format_desc->block.bits - padding_left)) - 1;
         const long long mask_right = ((long long)1 << (padding_right)) - 1;
         z_bitmask = lp_build_int_const_uni(type, mask_left & mask_right);
      }

      if(padding_left)
         src = LLVMBuildLShr(builder, src, lp_build_int_const_uni(type, padding_left), "");
      if(padding_right)
         src = LLVMBuildAnd(builder, src, z_bitmask, "");
      if(padding_left || padding_right)
         dst = LLVMBuildAnd(builder, dst, z_bitmask, "");
   }

   lp_build_name(dst, "zsbuf.z");

   test = lp_build_cmp(&bld, state->func, src, dst);
   lp_build_mask_update(mask, test);

   if(state->writemask) {
      if(z_bitmask)
         z_bitmask = LLVMBuildAnd(builder, mask->value, z_bitmask, "");
      else
         z_bitmask = mask->value;

      dst = lp_build_select(&bld, z_bitmask, src, dst);
      LLVMBuildStore(builder, dst, dst_ptr);
   }

   assert(!state->occlusion_count);
}