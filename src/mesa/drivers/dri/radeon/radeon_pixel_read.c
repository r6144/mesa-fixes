/*
 * Copyright (C) 2010 Maciej Cencora <m.cencora@gmail.com>
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "stdint.h"
#include "main/bufferobj.h"
#include "main/enums.h"
#include "main/image.h"
#include "main/state.h"
#include "swrast/swrast.h"

#include "radeon_buffer_objects.h"
#include "radeon_common_context.h"
#include "radeon_debug.h"
#include "radeon_mipmap_tree.h"

#include <unistd.h>

extern int r600_verbose_blit;

static gl_format gl_format_and_type_to_mesa_format(GLenum format, GLenum type)
{
    switch (format)
    {
        case GL_RGB:
            switch (type) {
                case GL_UNSIGNED_SHORT_5_6_5:
                    return MESA_FORMAT_RGB565;
                case GL_UNSIGNED_SHORT_5_6_5_REV:
                    return MESA_FORMAT_RGB565_REV;
            }
            break;
        case GL_RGBA:
            switch (type) {
                case GL_FLOAT:
                    return MESA_FORMAT_RGBA_FLOAT32;
                case GL_UNSIGNED_SHORT_5_5_5_1:
                    return MESA_FORMAT_RGBA5551;
                case GL_UNSIGNED_INT_8_8_8_8:
                    return MESA_FORMAT_RGBA8888;
                case GL_UNSIGNED_BYTE:
                case GL_UNSIGNED_INT_8_8_8_8_REV:
                    return MESA_FORMAT_RGBA8888_REV;
            }
            break;
        case GL_BGRA:
            switch (type) {
                case GL_UNSIGNED_SHORT_4_4_4_4:
                    return MESA_FORMAT_ARGB4444_REV;
                case GL_UNSIGNED_SHORT_4_4_4_4_REV:
                    return MESA_FORMAT_ARGB4444;
                case GL_UNSIGNED_SHORT_5_5_5_1:
                    return MESA_FORMAT_ARGB1555_REV;
                case GL_UNSIGNED_SHORT_1_5_5_5_REV:
                    return MESA_FORMAT_ARGB1555;
                case GL_UNSIGNED_INT_8_8_8_8:
                    return MESA_FORMAT_ARGB8888_REV;
                case GL_UNSIGNED_BYTE:
                case GL_UNSIGNED_INT_8_8_8_8_REV:
                    return MESA_FORMAT_ARGB8888;

            }
            break;
    }

    return MESA_FORMAT_NONE;
}

static GLboolean
do_blit_readpixels(struct gl_context * ctx,
                   GLint x, GLint y, GLsizei width, GLsizei height,
                   GLenum format, GLenum type,
                   const struct gl_pixelstore_attrib *pack, GLvoid * pixels)
{
    radeonContextPtr radeon = RADEON_CONTEXT(ctx);
    const struct radeon_renderbuffer *rrb = radeon_renderbuffer(ctx->ReadBuffer->_ColorReadBuffer);
    const gl_format dst_format = gl_format_and_type_to_mesa_format(format, type);
    unsigned dst_rowstride, dst_imagesize, aligned_rowstride, flip_y;
    struct radeon_bo *dst_buffer;
    GLint dst_x = 0, dst_y = 0;
    intptr_t dst_offset;
    int result;

#if 0
    return GL_FALSE; /* Works around the th125 problem including a crash in copy_rows() here; still present as of mesa-git in Dec 2010 */
#endif
    /* It's not worth if number of pixels to copy is really small */
    if (width * height < 100) {
        return GL_FALSE;
    }

    if (dst_format == MESA_FORMAT_NONE ||
        !radeon->vtbl.check_blit(dst_format) || !radeon->vtbl.blit) {
        return GL_FALSE;
    }

    if (ctx->_ImageTransferState || ctx->Color._LogicOpEnabled) {
        return GL_FALSE;
    }

    if (pack->SwapBytes || pack->LsbFirst) {
        return GL_FALSE;
    }

    if (pack->RowLength > 0) {
        dst_rowstride = pack->RowLength;
    } else {
        dst_rowstride = width;
    }

    if (!_mesa_clip_copytexsubimage(ctx, &dst_x, &dst_y, &x, &y, &width, &height)) {
        return GL_TRUE;
    }
    assert(x >= 0 && y >= 0);

    aligned_rowstride = get_texture_image_row_stride(radeon, dst_format, dst_rowstride, 0);
    dst_rowstride *= _mesa_get_format_bytes(dst_format);
    if (_mesa_is_bufferobj(pack->BufferObj) && aligned_rowstride != dst_rowstride)
        return GL_FALSE;
    dst_imagesize = get_texture_image_size(dst_format,
                                           aligned_rowstride,
                                           height, 1, 0);

    if (!_mesa_is_bufferobj(pack->BufferObj))
    {
        dst_buffer = radeon_bo_open(radeon->radeonScreen->bom, 0, dst_imagesize, 1024, RADEON_GEM_DOMAIN_GTT, 0);
        dst_offset = 0;
    }
    else
    {
        dst_buffer = get_radeon_buffer_object(pack->BufferObj)->bo;
        dst_offset = (intptr_t)pixels;
    }

    /* Disable source Y flipping for FBOs */
    flip_y = (ctx->ReadBuffer->Name == 0);
    if (pack->Invert) {
        y = rrb->base.Height - height - y;
        flip_y = !flip_y;
    }

#if 0 /* This doesn't help either... */
    radeonFlush(ctx);
    result = radeon_bo_wait(rrb->bo); assert(result == 0);
#endif
    r600_verbose_blit = 1;
    if (radeon->vtbl.blit(ctx,
                          rrb->bo,
                          rrb->draw_offset,
                          rrb->base.Format,
                          rrb->pitch / rrb->cpp,
                          rrb->base.Width,
                          rrb->base.Height,
                          x,
                          y,
                          dst_buffer,
                          dst_offset,
                          dst_format,
                          aligned_rowstride / _mesa_get_format_bytes(dst_format),
                          width,
                          height,
                          0, /* dst_x */
                          0, /* dst_y */
                          width,
                          height,
                          flip_y))
    {
	r600_verbose_blit = 0;
        if (!_mesa_is_bufferobj(pack->BufferObj))
        {
	    /* NOTE: r600_blit() ends with a radeonFlush(), so the command buffer has already been submitted via DRM_RADEON_CS,
	       and all the relevant buffer objects are now wait-able due to the radeon_bo_list_fence() call in radeon_cs_parser_fini().
	       Looking at the kernel DRM driver, the GEM set_domain operation doesn't usually do anything.
	       Since the buffer is in GTT, bo_wait() should be enough. */
	    /* NOTE: A bo_wait() should be unnecessary in GEM if the buffer object is not already mapped, since radeon_bo_map()
	       already waits in that case.  However, map_unmap_rb() does this, so we do this just to be safe. */
	    /* NOTE: Rebuild with --enable-debug to enable assertions; otherwise both ASSERT and assert will be disabled */
	    result = radeon_bo_wait(dst_buffer); assert(result == 0);
            result = radeon_bo_map(dst_buffer, 0);
	    if (result) {
		/* Since we have seen segfaults in copy_rows(), perhaps radeon_bo_map() could fail, but we can't reproduce this now. */
		fprintf(stderr, "(%s) error(%d) mapping buffer.\n",
			__FUNCTION__, result);
	    } else {
		/* This doesn't help either, even though the delay is noticeable.  th125's pictures remain garbled,
		   so the problem isn't insufficient waiting. */
		FILE *out_file;
		if (0) usleep(1000000);
		printf("read pixels: x=%d y=%d width=%u height=%u dst_rowstride=%u aligned_rowstride=%u\n",
		       x, y, width, height, dst_rowstride, aligned_rowstride);
		out_file = fopen("/tmp/readpix.out", "wb");
		result = fwrite(dst_buffer->ptr, aligned_rowstride * height, 1, out_file); assert(result == 1);
		fclose(out_file);
		copy_rows(pixels, dst_rowstride, dst_buffer->ptr,
			  aligned_rowstride, height, dst_rowstride);
	    }
	    /* Well, current libdrm's bo_map() still increases mapcount in case of failure, though this doesn't matter much now
	       due to the lazy munmap... */
	    radeon_bo_unmap(dst_buffer);
            radeon_bo_unref(dst_buffer);
        }

        return GL_TRUE;
    }
    r600_verbose_blit = 0;

    if (!_mesa_is_bufferobj(pack->BufferObj))
        radeon_bo_unref(dst_buffer);

    return GL_FALSE;
}

void
radeonReadPixels(struct gl_context * ctx,
                 GLint x, GLint y, GLsizei width, GLsizei height,
                 GLenum format, GLenum type,
                 const struct gl_pixelstore_attrib *pack, GLvoid * pixels)
{
    radeonContextPtr radeon = RADEON_CONTEXT(ctx);
    radeon_prepare_render(radeon);

    if (do_blit_readpixels(ctx, x, y, width, height, format, type, pack, pixels))
        return;

    /* Update Mesa state before calling down into _swrast_ReadPixels, as
     * the spans code requires the computed buffer states to be up to date,
     * but _swrast_ReadPixels only updates Mesa state after setting up
     * the spans code.
     */

    radeon_print(RADEON_FALLBACKS, RADEON_NORMAL,
                 "Falling back to sw for ReadPixels (format %s, type %s)\n",
                 _mesa_lookup_enum_by_nr(format), _mesa_lookup_enum_by_nr(type));

    if (ctx->NewState)
        _mesa_update_state(ctx);

    _swrast_ReadPixels(ctx, x, y, width, height, format, type, pack, pixels);
}
