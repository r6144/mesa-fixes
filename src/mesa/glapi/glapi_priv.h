/*
 * Mesa 3-D graphics library
 * Version:  7.1
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef _GLAPI_PRIV_H
#define _GLAPI_PRIV_H

#include "glthread.h"

extern void
_glapi_check_table_not_null(const struct _glapi_table *table);


extern void
_glapi_check_table(const struct _glapi_table *table);


extern void
init_glapi_relocs_once(void);


extern _glapi_proc
generate_entrypoint(GLuint functionOffset);


extern void
fill_in_entrypoint_offset(_glapi_proc entrypoint, GLuint offset);


extern _glapi_proc
get_entrypoint_address(GLuint functionOffset);


#if defined(USE_X64_64_ASM) && defined(GLX_USE_TLS)
# define DISPATCH_FUNCTION_SIZE  16
#elif defined(USE_X86_ASM)
# if defined(THREADS) && !defined(GLX_USE_TLS)
#  define DISPATCH_FUNCTION_SIZE  32
# else
#  define DISPATCH_FUNCTION_SIZE  16
# endif
#endif


#endif
