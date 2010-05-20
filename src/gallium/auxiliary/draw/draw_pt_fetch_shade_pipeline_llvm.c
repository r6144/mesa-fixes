/**************************************************************************
 *
 * Copyright 2010 VMWare, Inc.
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

#include "util/u_math.h"
#include "util/u_memory.h"
#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "draw/draw_pt.h"
#include "draw/draw_vs.h"
#include "draw/draw_llvm.h"


struct llvm_middle_end {
   struct draw_pt_middle_end base;
   struct draw_context *draw;

   struct pt_emit *emit;
   struct pt_fetch *fetch;
   struct pt_post_vs *post_vs;


   unsigned vertex_data_offset;
   unsigned vertex_size;
   unsigned prim;
   unsigned opt;

   struct draw_llvm *llvm;
   struct draw_llvm_variant *variants;
   struct draw_llvm_variant *current_variant;
   int nr_variants;
};


static void
llvm_middle_end_prepare( struct draw_pt_middle_end *middle,
                         unsigned prim,
                         unsigned opt,
                         unsigned *max_vertices )
{
   struct llvm_middle_end *fpme = (struct llvm_middle_end *)middle;
   struct draw_context *draw = fpme->draw;
   struct draw_vertex_shader *vs = draw->vs.vertex_shader;
   struct draw_llvm_variant_key key;
   struct draw_llvm_variant *variant = NULL;
   unsigned i;
   unsigned instance_id_index = ~0;

   /* Add one to num_outputs because the pipeline occasionally tags on
    * an additional texcoord, eg for AA lines.
    */
   unsigned nr = MAX2( vs->info.num_inputs,
		       vs->info.num_outputs + 1 );

   /* Scan for instanceID system value.
    */
   for (i = 0; i < vs->info.num_inputs; i++) {
      if (vs->info.input_semantic_name[i] == TGSI_SEMANTIC_INSTANCEID) {
         instance_id_index = i;
         break;
      }
   }

   fpme->prim = prim;
   fpme->opt = opt;

   /* Always leave room for the vertex header whether we need it or
    * not.  It's hard to get rid of it in particular because of the
    * viewport code in draw_pt_post_vs.c.
    */
   fpme->vertex_size = sizeof(struct vertex_header) + nr * 4 * sizeof(float);


   /* XXX: it's not really gl rasterization rules we care about here,
    * but gl vs dx9 clip spaces.
    */
   draw_pt_post_vs_prepare( fpme->post_vs,
			    (boolean)draw->bypass_clipping,
			    (boolean)(draw->identity_viewport),
			    (boolean)draw->rasterizer->gl_rasterization_rules,
			    (draw->vs.edgeflag_output ? true : false) );

   if (!(opt & PT_PIPELINE)) {
      draw_pt_emit_prepare( fpme->emit,
			    prim,
                            max_vertices );

      *max_vertices = MAX2( *max_vertices,
                            DRAW_PIPE_MAX_VERTICES );
   }
   else {
      *max_vertices = DRAW_PIPE_MAX_VERTICES;
   }

   /* return even number */
   *max_vertices = *max_vertices & ~1;

   draw_llvm_make_variant_key(fpme->llvm, &key);

   variant = fpme->variants;
   while(variant) {
      if(memcmp(&variant->key, &key, sizeof key) == 0)
         break;

      variant = variant->next;
   }

   if (!variant) {
      variant = draw_llvm_prepare(fpme->llvm, nr);
      variant->next = fpme->variants;
      fpme->variants = variant;
      ++fpme->nr_variants;
   }
   fpme->current_variant = variant;

   /*XXX we only support one constant buffer */
   fpme->llvm->jit_context.vs_constants =
      draw->pt.user.vs_constants[0];
   fpme->llvm->jit_context.gs_constants =
      draw->pt.user.gs_constants[0];
}



static void llvm_middle_end_run( struct draw_pt_middle_end *middle,
                                 const unsigned *fetch_elts,
                                 unsigned fetch_count,
                                 const ushort *draw_elts,
                                 unsigned draw_count )
{
   struct llvm_middle_end *fpme = (struct llvm_middle_end *)middle;
   struct draw_context *draw = fpme->draw;
   unsigned opt = fpme->opt;
   unsigned alloc_count = align( fetch_count, 4 );

   struct vertex_header *pipeline_verts =
      (struct vertex_header *)MALLOC(fpme->vertex_size * alloc_count);

   if (!pipeline_verts) {
      /* Not much we can do here - just skip the rendering.
       */
      assert(0);
      return;
   }

   fpme->current_variant->jit_func_elts( &fpme->llvm->jit_context,
                                         pipeline_verts,
                                         (const char **)draw->pt.user.vbuffer,
                                         fetch_elts,
                                         fetch_count,
                                         fpme->vertex_size,
                                         draw->pt.vertex_buffer );

   if (draw_pt_post_vs_run( fpme->post_vs,
			    pipeline_verts,
			    fetch_count,
			    fpme->vertex_size ))
   {
      opt |= PT_PIPELINE;
   }

   /* Do we need to run the pipeline?
    */
   if (opt & PT_PIPELINE) {
      draw_pipeline_run( fpme->draw,
                         fpme->prim,
                         pipeline_verts,
                         fetch_count,
                         fpme->vertex_size,
                         draw_elts,
                         draw_count );
   }
   else {
      draw_pt_emit( fpme->emit,
		    (const float (*)[4])pipeline_verts->data,
		    fetch_count,
		    fpme->vertex_size,
		    draw_elts,
		    draw_count );
   }


   FREE(pipeline_verts);
}


static void llvm_middle_end_linear_run( struct draw_pt_middle_end *middle,
                                       unsigned start,
                                       unsigned count)
{
   struct llvm_middle_end *fpme = (struct llvm_middle_end *)middle;
   struct draw_context *draw = fpme->draw;
   unsigned opt = fpme->opt;
   unsigned alloc_count = align( count, 4 );

   struct vertex_header *pipeline_verts =
      (struct vertex_header *)MALLOC(fpme->vertex_size * alloc_count);

   if (!pipeline_verts) {
      /* Not much we can do here - just skip the rendering.
       */
      assert(0);
      return;
   }

#if 0
   debug_printf("#### Pipeline = %p (data = %p)\n",
                pipeline_verts, pipeline_verts->data);
#endif
   fpme->current_variant->jit_func( &fpme->llvm->jit_context,
                                    pipeline_verts,
                                    (const char **)draw->pt.user.vbuffer,
                                    start,
                                    count,
                                    fpme->vertex_size,
                                    draw->pt.vertex_buffer );

   if (draw_pt_post_vs_run( fpme->post_vs,
			    pipeline_verts,
			    count,
			    fpme->vertex_size ))
   {
      opt |= PT_PIPELINE;
   }

   /* Do we need to run the pipeline?
    */
   if (opt & PT_PIPELINE) {
      draw_pipeline_run_linear( fpme->draw,
                                fpme->prim,
                                pipeline_verts,
                                count,
                                fpme->vertex_size);
   }
   else {
      draw_pt_emit_linear( fpme->emit,
                           (const float (*)[4])pipeline_verts->data,
                           fpme->vertex_size,
                           count );
   }

   FREE(pipeline_verts);
}



static boolean
llvm_middle_end_linear_run_elts( struct draw_pt_middle_end *middle,
                                 unsigned start,
                                 unsigned count,
                                 const ushort *draw_elts,
                                 unsigned draw_count )
{
   struct llvm_middle_end *fpme = (struct llvm_middle_end *)middle;
   struct draw_context *draw = fpme->draw;
   unsigned opt = fpme->opt;
   unsigned alloc_count = align( count, 4 );

   struct vertex_header *pipeline_verts =
      (struct vertex_header *)MALLOC(fpme->vertex_size * alloc_count);

   if (!pipeline_verts)
      return FALSE;

   fpme->current_variant->jit_func( &fpme->llvm->jit_context,
                                    pipeline_verts,
                                    (const char **)draw->pt.user.vbuffer,
                                    start,
                                    count,
                                    fpme->vertex_size,
                                    draw->pt.vertex_buffer );

   if (draw_pt_post_vs_run( fpme->post_vs,
			    pipeline_verts,
			    count,
			    fpme->vertex_size ))
   {
      opt |= PT_PIPELINE;
   }

   /* Do we need to run the pipeline?
    */
   if (opt & PT_PIPELINE) {
      draw_pipeline_run( fpme->draw,
                         fpme->prim,
                         pipeline_verts,
                         count,
                         fpme->vertex_size,
                         draw_elts,
                         draw_count );
   }
   else {
      draw_pt_emit( fpme->emit,
		    (const float (*)[4])pipeline_verts->data,
		    count,
		    fpme->vertex_size,
		    draw_elts,
		    draw_count );
   }

   FREE(pipeline_verts);
   return TRUE;
}



static void llvm_middle_end_finish( struct draw_pt_middle_end *middle )
{
   /* nothing to do */
}

static void llvm_middle_end_destroy( struct draw_pt_middle_end *middle )
{
   struct llvm_middle_end *fpme = (struct llvm_middle_end *)middle;
   struct draw_context *draw = fpme->draw;
   struct draw_llvm_variant *variant = NULL;

   variant = fpme->variants;
   while(variant) {
      struct draw_llvm_variant *next = variant->next;

      if (variant->function_elts) {
         if (variant->function_elts)
            LLVMFreeMachineCodeForFunction(draw->engine,
                                           variant->function_elts);
         LLVMDeleteFunction(variant->function_elts);
      }

      if (variant->function) {
         if (variant->function)
            LLVMFreeMachineCodeForFunction(draw->engine,
                                           variant->function);
         LLVMDeleteFunction(variant->function);
      }

      FREE(variant);

      variant = next;
   }
   if (fpme->fetch)
      draw_pt_fetch_destroy( fpme->fetch );

   if (fpme->emit)
      draw_pt_emit_destroy( fpme->emit );

   if (fpme->post_vs)
      draw_pt_post_vs_destroy( fpme->post_vs );

   if (fpme->llvm)
      draw_llvm_destroy( fpme->llvm );

   FREE(middle);
}


struct draw_pt_middle_end *draw_pt_fetch_pipeline_or_emit_llvm( struct draw_context *draw )
{
   struct llvm_middle_end *fpme = 0;

   if (!draw->engine)
      return NULL;

   fpme = CALLOC_STRUCT( llvm_middle_end );
   if (!fpme)
      goto fail;

   fpme->base.prepare         = llvm_middle_end_prepare;
   fpme->base.run             = llvm_middle_end_run;
   fpme->base.run_linear      = llvm_middle_end_linear_run;
   fpme->base.run_linear_elts = llvm_middle_end_linear_run_elts;
   fpme->base.finish          = llvm_middle_end_finish;
   fpme->base.destroy         = llvm_middle_end_destroy;

   fpme->draw = draw;

   fpme->fetch = draw_pt_fetch_create( draw );
   if (!fpme->fetch)
      goto fail;

   fpme->post_vs = draw_pt_post_vs_create( draw );
   if (!fpme->post_vs)
      goto fail;

   fpme->emit = draw_pt_emit_create( draw );
   if (!fpme->emit)
      goto fail;

   fpme->llvm = draw_llvm_create(draw);
   if (!fpme->llvm)
      goto fail;

   fpme->variants = NULL;
   fpme->current_variant = NULL;
   fpme->nr_variants = 0;

   return &fpme->base;

 fail:
   if (fpme)
      llvm_middle_end_destroy( &fpme->base );

   return NULL;
}
