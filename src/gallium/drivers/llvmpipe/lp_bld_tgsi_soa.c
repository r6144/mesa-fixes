/**************************************************************************
 * 
 * Copyright 2009 VMware, Inc.
 * Copyright 2007-2008 Tungsten Graphics, Inc., Cedar Park, Texas.
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

#include "pipe/p_config.h"
#include "pipe/p_shader_tokens.h"
#include "util/u_debug.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_util.h"
#include "tgsi/tgsi_exec.h"
#include "lp_bld_type.h"
#include "lp_bld_const.h"
#include "lp_bld_intr.h"
#include "lp_bld_arit.h"
#include "lp_bld_swizzle.h"
#include "lp_bld_tgsi.h"


#define LP_MAX_TEMPS 256
#define LP_MAX_IMMEDIATES 256


#define FOR_EACH_CHANNEL( CHAN )\
   for (CHAN = 0; CHAN < NUM_CHANNELS; CHAN++)

#define IS_DST0_CHANNEL_ENABLED( INST, CHAN )\
   ((INST).FullDstRegisters[0].DstRegister.WriteMask & (1 << (CHAN)))

#define IF_IS_DST0_CHANNEL_ENABLED( INST, CHAN )\
   if (IS_DST0_CHANNEL_ENABLED( INST, CHAN ))

#define FOR_EACH_DST0_ENABLED_CHANNEL( INST, CHAN )\
   FOR_EACH_CHANNEL( CHAN )\
      IF_IS_DST0_CHANNEL_ENABLED( INST, CHAN )

#define CHAN_X 0
#define CHAN_Y 1
#define CHAN_Z 2
#define CHAN_W 3


struct lp_build_tgsi_soa_context
{
   struct lp_build_context base;

   LLVMValueRef x, y, w;
   LLVMValueRef a0_ptr;
   LLVMValueRef dadx_ptr;
   LLVMValueRef dady_ptr;

   LLVMValueRef consts_ptr;
   LLVMValueRef (*outputs)[NUM_CHANNELS];
   LLVMValueRef samplers_ptr;

   LLVMValueRef oow;

   LLVMValueRef inputs[PIPE_MAX_SHADER_INPUTS][NUM_CHANNELS];

   LLVMValueRef immediates[LP_MAX_IMMEDIATES][NUM_CHANNELS];
   LLVMValueRef temps[LP_MAX_TEMPS][NUM_CHANNELS];

   /** Coords/texels store */
   LLVMValueRef store_ptr;
};


/**
 * Register fetch.
 */

static LLVMValueRef
emit_fetch(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_src_register *reg,
   const unsigned chan_index )
{
   unsigned swizzle = tgsi_util_get_full_src_register_extswizzle( reg, chan_index );
   LLVMValueRef res;

   switch (swizzle) {
   case TGSI_EXTSWIZZLE_X:
   case TGSI_EXTSWIZZLE_Y:
   case TGSI_EXTSWIZZLE_Z:
   case TGSI_EXTSWIZZLE_W:

      switch (reg->SrcRegister.File) {
      case TGSI_FILE_CONSTANT: {
         LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), reg->SrcRegister.Index*4 + swizzle, 0);
         LLVMValueRef scalar_ptr = LLVMBuildGEP(bld->base.builder, bld->consts_ptr, &index, 1, "");
         LLVMValueRef scalar = LLVMBuildLoad(bld->base.builder, scalar_ptr, "");
         res = lp_build_broadcast_scalar(&bld->base, scalar);
         break;
      }

      case TGSI_FILE_IMMEDIATE:
         res = bld->immediates[reg->SrcRegister.Index][swizzle];
         assert(res);
         break;

      case TGSI_FILE_INPUT:
         res = bld->inputs[reg->SrcRegister.Index][swizzle];
         assert(res);
         break;

      case TGSI_FILE_TEMPORARY:
         res = bld->temps[reg->SrcRegister.Index][swizzle];
         if(!res)
            return bld->base.undef;
         break;

      default:
         assert( 0 );
      }
      break;

   case TGSI_EXTSWIZZLE_ZERO:
      res = bld->base.zero;
      break;

   case TGSI_EXTSWIZZLE_ONE:
      res = bld->base.one;
      break;

   default:
      assert( 0 );
   }

   switch( tgsi_util_get_full_src_register_sign_mode( reg, chan_index ) ) {
   case TGSI_UTIL_SIGN_CLEAR:
      res = lp_build_abs( &bld->base, res );
      break;

   case TGSI_UTIL_SIGN_SET:
      res = lp_build_abs( &bld->base, res );
      res = LLVMBuildNeg( bld->base.builder, res, "" );
      break;

   case TGSI_UTIL_SIGN_TOGGLE:
      res = LLVMBuildNeg( bld->base.builder, res, "" );
      break;

   case TGSI_UTIL_SIGN_KEEP:
      break;
   }

   return res;
}

#define FETCH( FUNC, INST, INDEX, CHAN )\
   emit_fetch( FUNC, &(INST).FullSrcRegisters[INDEX], CHAN )

/**
 * Register store.
 */

static void
emit_store(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_dst_register *reg,
   const struct tgsi_full_instruction *inst,
   unsigned chan_index,
   LLVMValueRef value)
{
   switch( inst->Instruction.Saturate ) {
   case TGSI_SAT_NONE:
      break;

   case TGSI_SAT_ZERO_ONE:
      /* assert( 0 ); */
      break;

   case TGSI_SAT_MINUS_PLUS_ONE:
      assert( 0 );
      break;
   }

   switch( reg->DstRegister.File ) {
   case TGSI_FILE_OUTPUT:
      bld->outputs[reg->DstRegister.Index][chan_index] = value;
      break;

   case TGSI_FILE_TEMPORARY:
      bld->temps[reg->DstRegister.Index][chan_index] = value;
      break;

   case TGSI_FILE_ADDRESS:
      /* FIXME */
      assert(0);
      break;

   default:
      assert( 0 );
   }
}

#define STORE( FUNC, INST, INDEX, CHAN, VAL )\
   emit_store( FUNC, &(INST).FullDstRegisters[INDEX], &(INST), CHAN, VAL )


void PIPE_CDECL
lp_build_tgsi_fetch_texel_soa( struct tgsi_sampler **samplers,
                               uint32_t unit,
                               float *store )
{
   struct tgsi_sampler *sampler = samplers[unit];

#if 0
   uint j;

   debug_printf("%s sampler: %p (%p) store: %p\n", 
                __FUNCTION__,
                sampler, *sampler,
                store );

   debug_printf("lodbias %f\n", store[12]);

   for (j = 0; j < 4; j++)
      debug_printf("sample %d texcoord %f %f\n", 
                   j, 
                   store[0+j],
                   store[4+j]);
#endif

   {
      float rgba[NUM_CHANNELS][QUAD_SIZE];
      sampler->get_samples(sampler,
                           &store[0],
                           &store[4],
                           &store[8],
                           0.0f, /*store[12],  lodbias */
                           rgba);
      memcpy(store, rgba, sizeof rgba);
   }

#if 0
   for (j = 0; j < 4; j++)
      debug_printf("sample %d result %f %f %f %f\n", 
                   j, 
                   store[0+j],
                   store[4+j],
                   store[8+j],
                   store[12+j]);
#endif
}

/**
 * High-level instruction translators.
 */

static void
emit_tex( struct lp_build_tgsi_soa_context *bld,
          const struct tgsi_full_instruction *inst,
          boolean apply_lodbias,
          boolean projected)
{
   LLVMTypeRef vec_type = lp_build_vec_type(bld->base.type);
   const uint unit = inst->FullSrcRegisters[1].SrcRegister.Index;
   LLVMValueRef lodbias;
   LLVMValueRef oow;
   LLVMValueRef args[3];
   unsigned count;
   unsigned i;

   switch (inst->InstructionExtTexture.Texture) {
   case TGSI_TEXTURE_1D:
   case TGSI_TEXTURE_SHADOW1D:
      count = 1;
      break;
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_RECT:
   case TGSI_TEXTURE_SHADOW2D:
   case TGSI_TEXTURE_SHADOWRECT:
      count = 2;
      break;
   case TGSI_TEXTURE_3D:
   case TGSI_TEXTURE_CUBE:
      count = 3;
      break;
   default:
      assert(0);
      return;
   }

   if(apply_lodbias)
      lodbias = FETCH( bld, *inst, 0, 3 );
   else
      lodbias = bld->base.zero;

   if(!bld->store_ptr)
      bld->store_ptr = LLVMBuildArrayAlloca(bld->base.builder,
                                            vec_type,
                                            LLVMConstInt(LLVMInt32Type(), 4, 0),
                                            "store");

   if (projected) {
      oow = FETCH( bld, *inst, 0, 3 );
      oow = lp_build_rcp(&bld->base, oow);
   }

   for (i = 0; i < count; i++) {
      LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), i, 0);
      LLVMValueRef coord_ptr = LLVMBuildGEP(bld->base.builder, bld->store_ptr, &index, 1, "");
      LLVMValueRef coord;

      coord = FETCH( bld, *inst, 0, i );

      if (projected)
         coord = lp_build_mul(&bld->base, coord, oow);

      LLVMBuildStore(bld->base.builder, coord, coord_ptr);
   }

   args[0] = bld->samplers_ptr;
   args[1] = LLVMConstInt(LLVMInt32Type(), unit, 0);
   args[2] = bld->store_ptr;

   lp_build_intrinsic(bld->base.builder, "fetch_texel", LLVMVoidType(), args, 3);

   FOR_EACH_DST0_ENABLED_CHANNEL( *inst, i ) {
      LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), i, 0);
      LLVMValueRef res_ptr = LLVMBuildGEP(bld->base.builder, bld->store_ptr, &index, 1, "");
      LLVMValueRef res = LLVMBuildLoad(bld->base.builder, res_ptr, "");
      STORE( bld, *inst, 0, i, res );
   }
}


static void
emit_kil(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_src_register *reg )
{
#if 0
   unsigned uniquemask;
   unsigned unique_count = 0;
   unsigned chan_index;
   unsigned i;

   /* This mask stores component bits that were already tested. Note that
    * we test if the value is less than zero, so 1.0 and 0.0 need not to be
    * tested. */
   uniquemask = (1 << TGSI_EXTSWIZZLE_ZERO) | (1 << TGSI_EXTSWIZZLE_ONE);

   FOR_EACH_CHANNEL( chan_index ) {
      unsigned swizzle;

      /* unswizzle channel */
      swizzle = tgsi_util_get_full_src_register_extswizzle(
         reg,
         chan_index );

      /* check if the component has not been already tested */
      if( !(uniquemask & (1 << swizzle)) ) {
         uniquemask |= 1 << swizzle;

         /* allocate register */
         emit_fetch(
            bld,
            unique_count++,
            reg,
            chan_index );
      }
   }

   x86_push(
      bld,
      x86_make_reg( file_REG32, reg_AX ) );
   x86_push(
      bld,
      x86_make_reg( file_REG32, reg_DX ) );

   for (i = 0 ; i < unique_count; i++ ) {
      LLVMValueRef dataXMM = make_xmm(i);

      sse_cmpps(
         bld,
         dataXMM,
         get_temp(
            TGSI_EXEC_TEMP_00000000_I,
            TGSI_EXEC_TEMP_00000000_C ),
         cc_LessThan );
      
      if( i == 0 ) {
         sse_movmskps(
            bld,
            x86_make_reg( file_REG32, reg_AX ),
            dataXMM );
      }
      else {
         sse_movmskps(
            bld,
            x86_make_reg( file_REG32, reg_DX ),
            dataXMM );
         x86_or(
            bld,
            x86_make_reg( file_REG32, reg_AX ),
            x86_make_reg( file_REG32, reg_DX ) );
      }
   }

   x86_or(
      bld,
      get_temp(
         TGSI_EXEC_TEMP_KILMASK_I,
         TGSI_EXEC_TEMP_KILMASK_C ),
      x86_make_reg( file_REG32, reg_AX ) );

   x86_pop(
      bld,
      x86_make_reg( file_REG32, reg_DX ) );
   x86_pop(
      bld,
      x86_make_reg( file_REG32, reg_AX ) );
#endif
}


static void
emit_kilp(
   struct lp_build_tgsi_soa_context *bld )
{
   /* XXX todo / fix me */
}


/**
 * Check if inst src/dest regs use indirect addressing into temporary
 * register file.
 */
static boolean
indirect_temp_reference(const struct tgsi_full_instruction *inst)
{
   uint i;
   for (i = 0; i < inst->Instruction.NumSrcRegs; i++) {
      const struct tgsi_full_src_register *reg = &inst->FullSrcRegisters[i];
      if (reg->SrcRegister.File == TGSI_FILE_TEMPORARY &&
          reg->SrcRegister.Indirect)
         return TRUE;
   }
   for (i = 0; i < inst->Instruction.NumDstRegs; i++) {
      const struct tgsi_full_dst_register *reg = &inst->FullDstRegisters[i];
      if (reg->DstRegister.File == TGSI_FILE_TEMPORARY &&
          reg->DstRegister.Indirect)
         return TRUE;
   }
   return FALSE;
}


static int
emit_instruction(
   struct lp_build_tgsi_soa_context *bld,
   struct tgsi_full_instruction *inst )
{
   unsigned chan_index;
   LLVMValueRef src0, src1, src2;
   LLVMValueRef tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
   LLVMValueRef dst0;

   /* we can't handle indirect addressing into temp register file yet */
   if (indirect_temp_reference(inst))
      return FALSE;

   switch (inst->Instruction.Opcode) {
#if 0
   case TGSI_OPCODE_ARL:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_flr(bld, 0, 0);
         emit_f2it( bld, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_MOV:
   case TGSI_OPCODE_SWZ:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_LIT:
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) ) {
         STORE( bld, *inst, 0, CHAN_X, bld->base.one);
      }
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_W ) ) {
         STORE( bld, *inst, 0, CHAN_W, bld->base.one);
      }
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) ) {
         tmp0 = FETCH( bld, *inst, 0, CHAN_X );
         tmp0 = lp_build_max( &bld->base, tmp0, bld->base.one);
         STORE( bld, *inst, 0, CHAN_Y, tmp0);
      }
#if 0
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ) ) {
         /* XMM[1] = SrcReg[0].yyyy */
         tmp1 = FETCH( bld, *inst, 0, CHAN_Y );
         /* XMM[1] = max(XMM[1], 0) */
         tmp1 = lp_build_max( &bld->base, tmp1, bld->base.zero);
         /* XMM[2] = SrcReg[0].wwww */
         tmp2 = FETCH( bld, *inst, 0, CHAN_W );
         tmp1 = lp_build_pow( &bld->base, tmp1, tmp2);
         tmp0 = FETCH( bld, *inst, 0, CHAN_X );
         tmp2 = bld->base.zero;
         sse_cmpps(
            bld,
            make_xmm( 2 ),
            make_xmm( 0 ),
            cc_LessThan );
         sse_andps(
            bld,
            make_xmm( 2 ),
            make_xmm( 1 ) );
         STORE( bld, *inst, 0, CHAN_Z, tmp2);
      }
      break;
#else
      return 0;
#endif

   case TGSI_OPCODE_RCP:
   /* TGSI_OPCODE_RECIP */
      src0 = FETCH( bld, *inst, 0, CHAN_X );
      dst0 = lp_build_rcp(&bld->base, src0);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_RSQ:
   /* TGSI_OPCODE_RECIPSQRT */
      src0 = FETCH( bld, *inst, 0, CHAN_X );
      src0 = lp_build_abs(&bld->base, src0);
      dst0 = lp_build_rsqrt(&bld->base, src0);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_EXP:
      if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z )) {
         LLVMValueRef *p_exp2_int_part = NULL;
         LLVMValueRef *p_frac_part = NULL;
         LLVMValueRef *p_exp2 = NULL;

         src0 = FETCH( bld, *inst, 0, CHAN_X );

         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ))
            p_exp2_int_part = &tmp0;
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ))
            p_frac_part = &tmp1;
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ))
            p_exp2 = &tmp2;

         lp_build_exp2_approx(&bld->base, src0, p_exp2_int_part, p_frac_part, p_exp2);

         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ))
            STORE( bld, *inst, 0, CHAN_X, tmp0);
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ))
            STORE( bld, *inst, 0, CHAN_Y, tmp1);
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ))
            STORE( bld, *inst, 0, CHAN_Z, tmp2);
      }
      /* dst.w = 1.0 */
      if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_W )) {
         tmp0 = bld->base.one;
         STORE( bld, *inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_LOG:
      if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z )) {
         LLVMValueRef *p_floor_log2;
         LLVMValueRef *p_exp;
         LLVMValueRef *p_log2;

         src0 = FETCH( bld, *inst, 0, CHAN_X );
         src0 = lp_build_abs( &bld->base, src0 );

         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ))
            p_floor_log2 = &tmp0;
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ))
            p_exp = &tmp1;
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ))
            p_log2 = &tmp2;

         lp_build_log2_approx(&bld->base, src0, p_exp, p_floor_log2, p_log2);

         /* dst.x = floor(lg2(abs(src.x))) */
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ))
            STORE( bld, *inst, 0, CHAN_X, tmp0);
         /* dst.y = abs(src)/ex2(floor(lg2(abs(src.x)))) */
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y )) {
            tmp1 = lp_build_div( &bld->base, src0, tmp1);
            STORE( bld, *inst, 0, CHAN_Y, tmp1);
         }
         /* dst.z = lg2(abs(src.x)) */
         if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ))
            STORE( bld, *inst, 0, CHAN_Z, tmp2);
      }
      /* dst.w = 1.0 */
      if (IS_DST0_CHANNEL_ENABLED( *inst, CHAN_W )) {
         tmp0 = bld->base.one;
         STORE( bld, *inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_MUL:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         src0 = FETCH( bld, *inst, 0, chan_index );
         src1 = FETCH( bld, *inst, 1, chan_index );
         dst0 = lp_build_mul(&bld->base, src0, src1);
         STORE( bld, *inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_ADD:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         src0 = FETCH( bld, *inst, 0, chan_index );
         src1 = FETCH( bld, *inst, 1, chan_index );
         dst0 = lp_build_add(&bld->base, src0, src1);
         STORE( bld, *inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_DP3:
   /* TGSI_OPCODE_DOT3 */
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp1 = FETCH( bld, *inst, 1, CHAN_X );
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_Y );
      tmp2 = FETCH( bld, *inst, 1, CHAN_Y );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_Z );
      tmp2 = FETCH( bld, *inst, 1, CHAN_Z );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_DP4:
   /* TGSI_OPCODE_DOT4 */
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp1 = FETCH( bld, *inst, 1, CHAN_X );
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_Y );
      tmp2 = FETCH( bld, *inst, 1, CHAN_Y );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_Z );
      tmp2 = FETCH( bld, *inst, 1, CHAN_Z );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_W );
      tmp2 = FETCH( bld, *inst, 1, CHAN_W );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_DST:
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) {
         tmp0 = bld->base.one;
         STORE( bld, *inst, 0, CHAN_X, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) {
         tmp0 = FETCH( bld, *inst, 0, CHAN_Y );
         tmp1 = FETCH( bld, *inst, 1, CHAN_Y );
         tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
         STORE( bld, *inst, 0, CHAN_Y, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ) {
         tmp0 = FETCH( bld, *inst, 0, CHAN_Z );
         STORE( bld, *inst, 0, CHAN_Z, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_W ) {
         tmp0 = FETCH( bld, *inst, 1, CHAN_W );
         STORE( bld, *inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_MIN:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         src0 = FETCH( bld, *inst, 0, chan_index );
         src1 = FETCH( bld, *inst, 1, chan_index );
         dst0 = lp_build_min( &bld->base, src0, src1 );
         STORE( bld, *inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_MAX:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         src0 = FETCH( bld, *inst, 0, chan_index );
         src1 = FETCH( bld, *inst, 1, chan_index );
         dst0 = lp_build_max( &bld->base, src0, src1 );
         STORE( bld, *inst, 0, chan_index, dst0);
      }
      break;

#if 0
   case TGSI_OPCODE_SLT:
   /* TGSI_OPCODE_SETLT */
      emit_setcc( bld, inst, cc_LessThan );
      break;

   case TGSI_OPCODE_SGE:
   /* TGSI_OPCODE_SETGE */
      emit_setcc( bld, inst, cc_NotLessThan );
      break;
#endif

   case TGSI_OPCODE_MAD:
   /* TGSI_OPCODE_MADD */
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         tmp1 = FETCH( bld, *inst, 1, chan_index );
         tmp2 = FETCH( bld, *inst, 2, chan_index );
         tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
         tmp0 = lp_build_add( &bld->base, tmp0, tmp2);
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_SUB:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         tmp1 = FETCH( bld, *inst, 1, chan_index );
         tmp0 = lp_build_sub( &bld->base, tmp0, tmp1);
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_LRP:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         src0 = FETCH( bld, *inst, 0, chan_index );
         src1 = FETCH( bld, *inst, 1, chan_index );
         src2 = FETCH( bld, *inst, 2, chan_index );
         tmp0 = lp_build_sub( &bld->base, src1, src2 );
         tmp0 = lp_build_mul( &bld->base, src0, tmp0 );
         dst0 = lp_build_add( &bld->base, tmp0, src2 );
         STORE( bld, *inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_CND:
      return 0;
      break;

   case TGSI_OPCODE_CND0:
      return 0;
      break;

   case TGSI_OPCODE_DP2A:
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );  /* xmm0 = src[0].x */
      tmp1 = FETCH( bld, *inst, 1, CHAN_X );  /* xmm1 = src[1].x */
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 * xmm1 */
      tmp1 = FETCH( bld, *inst, 0, CHAN_Y );  /* xmm1 = src[0].y */
      tmp2 = FETCH( bld, *inst, 1, CHAN_Y );  /* xmm2 = src[1].y */
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);              /* xmm1 = xmm1 * xmm2 */
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 + xmm1 */
      tmp1 = FETCH( bld, *inst, 2, CHAN_X );  /* xmm1 = src[2].x */
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 + xmm1 */
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);  /* dest[ch] = xmm0 */
      }
      break;

#if 0
   case TGSI_OPCODE_FRC:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_frc( bld, 0, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_CLAMP:
      return 0;
      break;

   case TGSI_OPCODE_FLR:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_flr( bld, 0, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_ROUND:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_rnd( bld, 0, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_EX2: {
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp0 = lp_build_exp2( &bld->base, tmp0);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;
   }

   case TGSI_OPCODE_LG2:
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp0 = lp_build_log2( &bld->base, tmp0);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_POW:
      src0 = FETCH( bld, *inst, 0, CHAN_X );
      src1 = FETCH( bld, *inst, 1, CHAN_X );
      dst0 = lp_build_pow( &bld->base, src0, src1 );
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_XPD:
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) ) {
         tmp1 = FETCH( bld, *inst, 1, CHAN_Z );
         tmp3 = FETCH( bld, *inst, 0, CHAN_Z );
      }
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ) ) {
         tmp0 = FETCH( bld, *inst, 0, CHAN_Y );
         tmp4 = FETCH( bld, *inst, 1, CHAN_Y );
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) {
         tmp2 = tmp0;
         tmp2 = lp_build_mul( &bld->base, tmp2, tmp1);
         tmp5 = tmp3;
         tmp5 = lp_build_mul( &bld->base, tmp5, tmp4);
         tmp2 = lp_build_sub( &bld->base, tmp2, tmp5);
         STORE( bld, *inst, 0, CHAN_X, tmp2);
      }
      if( IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) ||
          IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ) ) {
         tmp2 = FETCH( bld, *inst, 1, CHAN_X );
         tmp5 = FETCH( bld, *inst, 0, CHAN_X );
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) {
         tmp3 = lp_build_mul( &bld->base, tmp3, tmp2);
         tmp1 = lp_build_mul( &bld->base, tmp1, tmp5);
         tmp3 = lp_build_sub( &bld->base, tmp3, tmp1);
         STORE( bld, *inst, 0, CHAN_Y, tmp3);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ) {
         tmp5 = lp_build_mul( &bld->base, tmp5, tmp4);
         tmp0 = lp_build_mul( &bld->base, tmp0, tmp2);
         tmp5 = lp_build_sub( &bld->base, tmp5, tmp0);
         STORE( bld, *inst, 0, CHAN_Z, tmp5);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_W ) {
         tmp0 = bld->base.one;
         STORE( bld, *inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_ABS:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         tmp0 = lp_build_abs( &bld->base, tmp0 ) ;
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_RCC:
      return 0;
      break;

   case TGSI_OPCODE_DPH:
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp1 = FETCH( bld, *inst, 1, CHAN_X );
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_Y );
      tmp2 = FETCH( bld, *inst, 1, CHAN_Y );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 0, CHAN_Z );
      tmp2 = FETCH( bld, *inst, 1, CHAN_Z );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = FETCH( bld, *inst, 1, CHAN_W );
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_COS:
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp0 = lp_build_cos( &bld->base, tmp0 );
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_DDX:
      return 0;
      break;

   case TGSI_OPCODE_DDY:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_KILP:
      /* predicated kill */
      emit_kilp( bld );
      return 0; /* XXX fix me */
      break;

   case TGSI_OPCODE_KIL:
      /* conditional kill */
      emit_kil( bld, &inst->FullSrcRegisters[0] );
      break;
#endif

   case TGSI_OPCODE_PK2H:
      return 0;
      break;

   case TGSI_OPCODE_PK2US:
      return 0;
      break;

   case TGSI_OPCODE_PK4B:
      return 0;
      break;

   case TGSI_OPCODE_PK4UB:
      return 0;
      break;

   case TGSI_OPCODE_RFL:
      return 0;
      break;

   case TGSI_OPCODE_SEQ:
      return 0;
      break;

   case TGSI_OPCODE_SFL:
      return 0;
      break;

   case TGSI_OPCODE_SGT:
      return 0;
      break;

   case TGSI_OPCODE_SIN:
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );
      tmp0 = lp_build_sin( &bld->base, tmp0 );
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_SLE:
      return 0;
      break;

   case TGSI_OPCODE_SNE:
      return 0;
      break;

   case TGSI_OPCODE_STR:
      return 0;
      break;

   case TGSI_OPCODE_TEX:
      emit_tex( bld, inst, FALSE, FALSE );
      break;

   case TGSI_OPCODE_TXD:
      return 0;
      break;

   case TGSI_OPCODE_UP2H:
      return 0;
      break;

   case TGSI_OPCODE_UP2US:
      return 0;
      break;

   case TGSI_OPCODE_UP4B:
      return 0;
      break;

   case TGSI_OPCODE_UP4UB:
      return 0;
      break;

   case TGSI_OPCODE_X2D:
      return 0;
      break;

   case TGSI_OPCODE_ARA:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_ARR:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_rnd( bld, 0, 0 );
         emit_f2it( bld, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_BRA:
      return 0;
      break;

   case TGSI_OPCODE_CAL:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_RET:
      emit_ret( bld );
      break;
#endif

   case TGSI_OPCODE_END:
      break;

#if 0
   case TGSI_OPCODE_SSG:
   /* TGSI_OPCODE_SGN */
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_sgn( bld, 0, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_CMP:
      emit_cmp (bld, inst);
      break;
#endif

   case TGSI_OPCODE_SCS:
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_X ) {
         tmp0 = FETCH( bld, *inst, 0, CHAN_X );
         tmp0 = lp_build_cos( &bld->base, tmp0 );
         STORE( bld, *inst, 0, CHAN_X, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Y ) {
         tmp0 = FETCH( bld, *inst, 0, CHAN_X );
         tmp0 = lp_build_sin( &bld->base, tmp0 );
         STORE( bld, *inst, 0, CHAN_Y, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_Z ) {
         tmp0 = bld->base.zero;
         STORE( bld, *inst, 0, CHAN_Z, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( *inst, CHAN_W ) {
         tmp0 = bld->base.one;
         STORE( bld, *inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_TXB:
      emit_tex( bld, inst, TRUE, FALSE );
      break;

   case TGSI_OPCODE_NRM:
      /* fall-through */
   case TGSI_OPCODE_NRM4:
      /* 3 or 4-component normalization */
      {
         uint dims = (inst->Instruction.Opcode == TGSI_OPCODE_NRM) ? 3 : 4;

         if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_X) ||
             IS_DST0_CHANNEL_ENABLED(*inst, CHAN_Y) ||
             IS_DST0_CHANNEL_ENABLED(*inst, CHAN_Z) ||
             (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_W) && dims == 4)) {

            /* NOTE: Cannot use xmm regs 2/3 here (see emit_rsqrt() above). */

            /* xmm4 = src.x */
            /* xmm0 = src.x * src.x */
            tmp0 = FETCH(bld, *inst, 0, CHAN_X);
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_X)) {
               tmp4 = tmp0;
            }
            tmp0 = lp_build_mul( &bld->base, tmp0, tmp0);

            /* xmm5 = src.y */
            /* xmm0 = xmm0 + src.y * src.y */
            tmp1 = FETCH(bld, *inst, 0, CHAN_Y);
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_Y)) {
               tmp5 = tmp1;
            }
            tmp1 = lp_build_mul( &bld->base, tmp1, tmp1);
            tmp0 = lp_build_add( &bld->base, tmp0, tmp1);

            /* xmm6 = src.z */
            /* xmm0 = xmm0 + src.z * src.z */
            tmp1 = FETCH(bld, *inst, 0, CHAN_Z);
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_Z)) {
               tmp6 = tmp1;
            }
            tmp1 = lp_build_mul( &bld->base, tmp1, tmp1);
            tmp0 = lp_build_add( &bld->base, tmp0, tmp1);

            if (dims == 4) {
               /* xmm7 = src.w */
               /* xmm0 = xmm0 + src.w * src.w */
               tmp1 = FETCH(bld, *inst, 0, CHAN_W);
               if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_W)) {
                  tmp7 = tmp1;
               }
               tmp1 = lp_build_mul( &bld->base, tmp1, tmp1);
               tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
            }

            /* xmm1 = 1 / sqrt(xmm0) */
            tmp1 = lp_build_rsqrt( &bld->base, tmp0);

            /* dst.x = xmm1 * src.x */
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_X)) {
               tmp4 = lp_build_mul( &bld->base, tmp4, tmp1);
               STORE(bld, *inst, 0, CHAN_X, tmp4);
            }

            /* dst.y = xmm1 * src.y */
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_Y)) {
               tmp5 = lp_build_mul( &bld->base, tmp5, tmp1);
               STORE(bld, *inst, 0, CHAN_Y, tmp5);
            }

            /* dst.z = xmm1 * src.z */
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_Z)) {
               tmp6 = lp_build_mul( &bld->base, tmp6, tmp1);
               STORE(bld, *inst, 0, CHAN_Z, tmp6);
            }

            /* dst.w = xmm1 * src.w */
            if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_X) && dims == 4) {
               tmp7 = lp_build_mul( &bld->base, tmp7, tmp1);
               STORE(bld, *inst, 0, CHAN_W, tmp7);
            }
         }

         /* dst0.w = 1.0 */
         if (IS_DST0_CHANNEL_ENABLED(*inst, CHAN_W) && dims == 3) {
            tmp0 = bld->base.one;
            STORE(bld, *inst, 0, CHAN_W, tmp0);
         }
      }
      break;

   case TGSI_OPCODE_DIV:
      return 0;
      break;

   case TGSI_OPCODE_DP2:
      tmp0 = FETCH( bld, *inst, 0, CHAN_X );  /* xmm0 = src[0].x */
      tmp1 = FETCH( bld, *inst, 1, CHAN_X );  /* xmm1 = src[1].x */
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 * xmm1 */
      tmp1 = FETCH( bld, *inst, 0, CHAN_Y );  /* xmm1 = src[0].y */
      tmp2 = FETCH( bld, *inst, 1, CHAN_Y );  /* xmm2 = src[1].y */
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);              /* xmm1 = xmm1 * xmm2 */
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 + xmm1 */
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         STORE( bld, *inst, 0, chan_index, tmp0);  /* dest[ch] = xmm0 */
      }
      break;

   case TGSI_OPCODE_TXL:
      emit_tex( bld, inst, TRUE, FALSE );
      break;

   case TGSI_OPCODE_TXP:
      emit_tex( bld, inst, FALSE, TRUE );
      break;
      
   case TGSI_OPCODE_BRK:
      return 0;
      break;

   case TGSI_OPCODE_IF:
      return 0;
      break;

   case TGSI_OPCODE_LOOP:
      return 0;
      break;

   case TGSI_OPCODE_REP:
      return 0;
      break;

   case TGSI_OPCODE_ELSE:
      return 0;
      break;

   case TGSI_OPCODE_ENDIF:
      return 0;
      break;

   case TGSI_OPCODE_ENDLOOP:
      return 0;
      break;

   case TGSI_OPCODE_ENDREP:
      return 0;
      break;

   case TGSI_OPCODE_PUSHA:
      return 0;
      break;

   case TGSI_OPCODE_POPA:
      return 0;
      break;

   case TGSI_OPCODE_CEIL:
      return 0;
      break;

   case TGSI_OPCODE_I2F:
      return 0;
      break;

   case TGSI_OPCODE_NOT:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_TRUNC:
      FOR_EACH_DST0_ENABLED_CHANNEL( *inst, chan_index ) {
         tmp0 = FETCH( bld, *inst, 0, chan_index );
         emit_f2it( bld, 0 );
         emit_i2f( bld, 0 );
         STORE( bld, *inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_SHL:
      return 0;
      break;

   case TGSI_OPCODE_SHR:
      return 0;
      break;

   case TGSI_OPCODE_AND:
      return 0;
      break;

   case TGSI_OPCODE_OR:
      return 0;
      break;

   case TGSI_OPCODE_MOD:
      return 0;
      break;

   case TGSI_OPCODE_XOR:
      return 0;
      break;

   case TGSI_OPCODE_SAD:
      return 0;
      break;

   case TGSI_OPCODE_TXF:
      return 0;
      break;

   case TGSI_OPCODE_TXQ:
      return 0;
      break;

   case TGSI_OPCODE_CONT:
      return 0;
      break;

   case TGSI_OPCODE_EMIT:
      return 0;
      break;

   case TGSI_OPCODE_ENDPRIM:
      return 0;
      break;

   default:
      return 0;
   }
   
   return 1;
}

static void
emit_declaration(
   struct lp_build_tgsi_soa_context *bld,
   struct tgsi_full_declaration *decl )
{
   if( decl->Declaration.File == TGSI_FILE_INPUT ) {
      LLVMBuilderRef builder = bld->base.builder;
      unsigned first, last, mask;
      unsigned attrib, chan;

      first = decl->DeclarationRange.First;
      last = decl->DeclarationRange.Last;
      mask = decl->Declaration.UsageMask;

      for( attrib = first; attrib <= last; attrib++ ) {
         for( chan = 0; chan < NUM_CHANNELS; chan++ ) {
            LLVMValueRef input = bld->base.undef;

            if( mask & (1 << chan) ) {
               LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), attrib*NUM_CHANNELS + chan, 0);
               LLVMValueRef a0;
               LLVMValueRef dadx;
               LLVMValueRef dady;
               char name[32];

               switch( decl->Declaration.Interpolate ) {
               case TGSI_INTERPOLATE_PERSPECTIVE:
               case TGSI_INTERPOLATE_LINEAR: {
                  LLVMValueRef dadx_ptr = LLVMBuildGEP(builder, bld->dadx_ptr, &index, 1, "");
                  LLVMValueRef dady_ptr = LLVMBuildGEP(builder, bld->dady_ptr, &index, 1, "");
                  dadx = LLVMBuildLoad(builder, dadx_ptr, "");
                  dady = LLVMBuildLoad(builder, dady_ptr, "");
                  dadx = lp_build_broadcast_scalar(&bld->base, dadx);
                  dady = lp_build_broadcast_scalar(&bld->base, dady);
                  util_snprintf(name, sizeof name, "dadx_%u.%c", attrib, "xyzw"[chan]);
                  LLVMSetValueName(dadx, name);
                  util_snprintf(name, sizeof name, "dady_%u.%c", attrib, "xyzw"[chan]);
                  LLVMSetValueName(dady, name);
               }

               case TGSI_INTERPOLATE_CONSTANT: {
                  LLVMValueRef a0_ptr = LLVMBuildGEP(builder, bld->a0_ptr, &index, 1, "");
                  a0 = LLVMBuildLoad(builder, a0_ptr, "");
                  a0 = lp_build_broadcast_scalar(&bld->base, a0);
                  util_snprintf(name, sizeof name, "a0_%u.%c", attrib, "xyzw"[chan]);
                  LLVMSetValueName(a0, name);
                  break;
               }

               default:
                  assert(0);
                  break;
               }

               input = a0;

               if (decl->Declaration.Interpolate != TGSI_INTERPOLATE_CONSTANT) {
                  input = lp_build_add(&bld->base, input, lp_build_mul(&bld->base, bld->x, dadx));
                  input = lp_build_add(&bld->base, input, lp_build_mul(&bld->base, bld->y, dady));
               }

               if (decl->Declaration.Interpolate == TGSI_INTERPOLATE_PERSPECTIVE) {
                  if(!bld->oow)
                     bld->oow = lp_build_rcp(&bld->base, bld->w);
                  input = lp_build_mul(&bld->base, input, bld->oow);
               }

               util_snprintf(name, sizeof name, "input%u.%c", attrib, "xyzw"[chan]);
               LLVMSetValueName(input, name);
            }

            bld->inputs[attrib][chan] = input;
         }
      }
   }
}

/**
 * Translate a TGSI vertex/fragment shader to SSE2 code.
 * Slightly different things are done for vertex vs. fragment shaders.
 *
 * \param tokens  the TGSI input shader
 * \param bld  the output SSE code/function
 * \param immediates  buffer to place immediates, later passed to SSE bld
 * \param return  1 for success, 0 if translation failed
 */
void
lp_build_tgsi_soa(LLVMBuilderRef builder,
                  const struct tgsi_token *tokens,
                  union lp_type type,
                  LLVMValueRef *pos,
                  LLVMValueRef a0_ptr,
                  LLVMValueRef dadx_ptr,
                  LLVMValueRef dady_ptr,
                  LLVMValueRef consts_ptr,
                  LLVMValueRef (*outputs)[4],
                  LLVMValueRef samplers_ptr)
{
   struct lp_build_tgsi_soa_context bld;
   struct tgsi_parse_context parse;
   uint num_immediates = 0;
   unsigned i;

   /* Setup build context */
   memset(&bld, 0, sizeof bld);
   lp_build_context_init(&bld.base, builder, type);
   bld.x = pos[0];
   bld.y = pos[1];
   bld.w = pos[3];
   bld.a0_ptr = a0_ptr;
   bld.dadx_ptr = dadx_ptr;
   bld.dady_ptr = dady_ptr;
   bld.outputs = outputs;
   bld.consts_ptr = consts_ptr;
   bld.samplers_ptr = samplers_ptr;

   tgsi_parse_init( &parse, tokens );

   while( !tgsi_parse_end_of_tokens( &parse ) ) {
      tgsi_parse_token( &parse );

      switch( parse.FullToken.Token.Type ) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         if (parse.FullHeader.Processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            emit_declaration( &bld, &parse.FullToken.FullDeclaration );
         }
         break;

      case TGSI_TOKEN_TYPE_INSTRUCTION:
         if (!emit_instruction( &bld, &parse.FullToken.FullInstruction )) {
	    debug_printf("failed to translate tgsi opcode %d to SSE (%s)\n", 
			 parse.FullToken.FullInstruction.Instruction.Opcode,
                         parse.FullHeader.Processor.Processor == TGSI_PROCESSOR_VERTEX ?
                         "vertex shader" : "fragment shader");
	 }
         break;

      case TGSI_TOKEN_TYPE_IMMEDIATE:
         /* simply copy the immediate values into the next immediates[] slot */
         {
            const uint size = parse.FullToken.FullImmediate.Immediate.NrTokens - 1;
            assert(size <= 4);
            assert(num_immediates < LP_MAX_IMMEDIATES);
            for( i = 0; i < size; ++i )
               bld.immediates[num_immediates][i] =
                  lp_build_const_uni(type, parse.FullToken.FullImmediate.u[i].Float);
            for( i = size; i < 4; ++i )
               bld.immediates[num_immediates][i] = bld.base.undef;
            num_immediates++;
         }
         break;

      default:
         assert( 0 );
      }
   }

   tgsi_parse_free( &parse );
}
