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


#include "pipe/p_compiler.h"
#include "util/u_cpu_detect.h"
#include "util/u_debug.h"
#include "lp_bld_debug.h"
#include "lp_bld_init.h"


#ifdef DEBUG
unsigned gallivm_debug = 0;

static const struct debug_named_value lp_bld_debug_flags[] = {
   { "tgsi",   GALLIVM_DEBUG_TGSI },
   { "ir",     GALLIVM_DEBUG_IR },
   { "asm",    GALLIVM_DEBUG_ASM },
   { "nopt",   GALLIVM_DEBUG_NO_OPT },
   {NULL, 0}
};
#endif


LLVMModuleRef lp_build_module = NULL;
LLVMExecutionEngineRef lp_build_engine = NULL;
LLVMModuleProviderRef lp_build_provider = NULL;
LLVMTargetDataRef lp_build_target = NULL;


void
lp_build_init(void)
{
#ifdef DEBUG
   gallivm_debug = debug_get_flags_option("GALLIVM_DEBUG", lp_bld_debug_flags, 0 );
#endif

   LLVMInitializeNativeTarget();

   LLVMLinkInJIT();

   if (!lp_build_module)
      lp_build_module = LLVMModuleCreateWithName("gallivm");

   if (!lp_build_provider)
      lp_build_provider = LLVMCreateModuleProviderForExistingModule(lp_build_module);

   if (!lp_build_engine) {
      char *error = NULL;

      if (LLVMCreateJITCompiler(&lp_build_engine, lp_build_provider, 1, &error)) {
         _debug_printf("%s\n", error);
         LLVMDisposeMessage(error);
         assert(0);
      }
   }

   if (!lp_build_target)
      lp_build_target = LLVMGetExecutionEngineTargetData(lp_build_engine);

   util_cpu_detect();

#if 0
   /* For simulating less capable machines */
   util_cpu_caps.has_sse3 = 0;
   util_cpu_caps.has_ssse3 = 0;
   util_cpu_caps.has_sse4_1 = 0;
#endif
}


/* 
 * Hack to allow the linking of release LLVM static libraries on a debug build.
 *
 * See also:
 * - http://social.msdn.microsoft.com/Forums/en-US/vclanguage/thread/7234ea2b-0042-42ed-b4e2-5d8644dfb57d
 */
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdefs.h>
_CRTIMP void __cdecl
_invalid_parameter_noinfo(void) {}
#endif
