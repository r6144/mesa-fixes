/*
 * Copyright (C) 2008-2009  Advanced Micro Devices, Inc.
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
 * THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Authors:
 *   Richard Li <RichardZ.Li@amd.com>, <richardradeon@gmail.com>
 *   CooperYuan <cooper.yuan@amd.com>, <cooperyuan@gmail.com>
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "main/glheader.h"

#include "r700_debug.h"
#include "r600_context.h"

void NormalizeLogErrorCode(int nError)
{
    //TODO
}

void r700_error(int nLocalError, char* fmt, ...)
{
    va_list args;

    NormalizeLogErrorCode(nLocalError);

	va_start(args, fmt);
    fprintf(stderr, fmt, args);
    va_end(args);
}

void DumpHwBinary(int type, void *addr, int size)
{
    int i;
    unsigned int *pHw = (unsigned int *)addr;
    switch (type)
    {
        case DUMP_PIXEL_SHADER:
            DEBUGF("Pixel Shader\n");
        break;
        case DUMP_VERTEX_SHADER:
            DEBUGF("Vertex Shader\n");
        break;
        case DUMP_FETCH_SHADER:
            DEBUGF("Fetch Shader\n");
        break;
    }

    for (i = 0; i < size; i++)
    {
        DEBUGP("0x%08x,\t", *pHw);
        if (i%4 == 3)
            DEBUGP("\n", *pHw);
        pHw++;

    }
}
