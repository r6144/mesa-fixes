/*
 * Copyright © 2008-2009 Maciej Cencora <m.cencora@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Maciej Cencora <m.cencora@gmail.com>
 *
 */
#include "radeon_common.h"
#include "radeon_queryobj.h"

#include "main/imports.h"
#include "main/simple_list.h"

#define DDEBUG 0

#define PAGE_SIZE 4096

static void radeonQueryGetResult(GLcontext *ctx, struct gl_query_object *q)
{
	struct radeon_query_object *query = (struct radeon_query_object *)q;
	uint32_t *result;
	int i;

	if (DDEBUG) fprintf(stderr, "%s: query id %d, result %d\n", __FUNCTION__, query->Base.Id, (int) query->Base.Result);

	radeon_bo_map(query->bo, GL_FALSE);

	result = query->bo->ptr;

	query->Base.Result = 0;
	for (i = 0; i < query->curr_offset/sizeof(uint32_t); ++i) {
		query->Base.Result += result[i];
		if (DDEBUG) fprintf(stderr, "result[%d] = %d\n", i, result[i]);
	}

	radeon_bo_unmap(query->bo);
}

static struct gl_query_object * radeonNewQueryObject(GLcontext *ctx, GLuint id)
{
	struct radeon_query_object *query;

	query = _mesa_calloc(sizeof(struct radeon_query_object));

	query->Base.Id = id;
	query->Base.Result = 0;
	query->Base.Active = GL_FALSE;
	query->Base.Ready = GL_TRUE;

	if (DDEBUG) fprintf(stderr, "%s: query id %d\n", __FUNCTION__, query->Base.Id);

	return &query->Base;
}

static void radeonDeleteQuery(GLcontext *ctx, struct gl_query_object *q)
{
	struct radeon_query_object *query = (struct radeon_query_object *)q;

	if (DDEBUG) fprintf(stderr, "%s: query id %d\n", __FUNCTION__, q->Id);

	if (query->bo) {
		radeon_bo_unref(query->bo);
	}

	_mesa_free(query);
}

static void radeonWaitQuery(GLcontext *ctx, struct gl_query_object *q)
{
	radeonContextPtr radeon = RADEON_CONTEXT(ctx);
	struct radeon_query_object *tmp, *query = (struct radeon_query_object *)q;

	/* If the cmdbuf with packets for this query hasn't been flushed yet, do it now */
	{
		GLboolean found = GL_FALSE;
		foreach(tmp, &radeon->query.not_flushed_head) {
			if (tmp == query) {
				found = GL_TRUE;
				break;
			}
		}

		if (found)
			ctx->Driver.Flush(ctx);
	}

	if (DDEBUG) fprintf(stderr, "%s: query id %d, bo %p, offset %d\n", __FUNCTION__, q->Id, query->bo, query->curr_offset);

	radeonQueryGetResult(ctx, q);

	query->Base.Ready = GL_TRUE;
}


static void radeonBeginQuery(GLcontext *ctx, struct gl_query_object *q)
{
	radeonContextPtr radeon = RADEON_CONTEXT(ctx);
	struct radeon_query_object *query = (struct radeon_query_object *)q;

	if (DDEBUG) fprintf(stderr, "%s: query id %d\n", __FUNCTION__, q->Id);

	assert(radeon->query.current == NULL);

	if (radeon->dma.flush)
		radeon->dma.flush(radeon->glCtx);

	if (!query->bo) {
		query->bo = radeon_bo_open(radeon->radeonScreen->bom, 0, PAGE_SIZE, PAGE_SIZE, RADEON_GEM_DOMAIN_GTT, 0);
	}
	query->curr_offset = 0;

	radeon->query.current = query;

	radeon->query.queryobj.dirty = GL_TRUE;
	insert_at_tail(&radeon->query.not_flushed_head, query);

}

void radeonEmitQueryEnd(GLcontext *ctx)
{
	radeonContextPtr radeon = RADEON_CONTEXT(ctx);
	struct radeon_query_object *query = radeon->query.current;

	if (!query)
		return;

	if (query->emitted_begin == GL_FALSE)
		return;

	if (DDEBUG) fprintf(stderr, "%s: query id %d, bo %p, offset %d\n", __FUNCTION__, query->Base.Id, query->bo, query->curr_offset);

	radeon_cs_space_check_with_bo(radeon->cmdbuf.cs,
				      query->bo,
				      0, RADEON_GEM_DOMAIN_GTT);

	radeon->vtbl.emit_query_finish(radeon);
}

static void radeonEndQuery(GLcontext *ctx, struct gl_query_object *q)
{
	radeonContextPtr radeon = RADEON_CONTEXT(ctx);

	if (DDEBUG) fprintf(stderr, "%s: query id %d\n", __FUNCTION__, q->Id);

	if (radeon->dma.flush)
		radeon->dma.flush(radeon->glCtx);
	radeonEmitQueryEnd(ctx);

	radeon->query.current = NULL;
}

/**
 * TODO:
 * should check if bo is idle, bo there's no interface to do it
 * just wait for result now
 */
static void radeonCheckQuery(GLcontext *ctx, struct gl_query_object *q)
{
	if (DDEBUG) fprintf(stderr, "%s: query id %d\n", __FUNCTION__, q->Id);

	radeonWaitQuery(ctx, q);
}

void radeonInitQueryObjFunctions(struct dd_function_table *functions)
{
	functions->NewQueryObject = radeonNewQueryObject;
	functions->DeleteQuery = radeonDeleteQuery;
	functions->BeginQuery = radeonBeginQuery;
	functions->EndQuery = radeonEndQuery;
	functions->CheckQuery = radeonCheckQuery;
	functions->WaitQuery = radeonWaitQuery;
}

int radeon_check_query_active(GLcontext *ctx, struct radeon_state_atom *atom)
{
	radeonContextPtr radeon = RADEON_CONTEXT(ctx);
	struct radeon_query_object *query = radeon->query.current;

	if (!query || query->emitted_begin)
		return 0;
	return atom->cmd_size;
}

void radeon_emit_queryobj(GLcontext *ctx, struct radeon_state_atom *atom)
{
	radeonContextPtr radeon = RADEON_CONTEXT(ctx);
	BATCH_LOCALS(radeon);
	int dwords;

	dwords = (*atom->check) (ctx, atom);

	BEGIN_BATCH_NO_AUTOSTATE(dwords);
	OUT_BATCH_TABLE(atom->cmd, dwords);
	END_BATCH();

	radeon->query.current->emitted_begin = GL_TRUE;
}