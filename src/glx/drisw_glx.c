/*
 * Copyright 2008 George Sapountzis
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if defined(GLX_DIRECT_RENDERING) && !defined(GLX_USE_APPLEGL)

#include <X11/Xlib.h>
#include "glxclient.h"
#include <dlfcn.h>
#include "dri_common.h"

typedef struct __GLXDRIdisplayPrivateRec __GLXDRIdisplayPrivate;
typedef struct __GLXDRIcontextPrivateRec __GLXDRIcontextPrivate;
typedef struct __GLXDRIdrawablePrivateRec __GLXDRIdrawablePrivate;

struct __GLXDRIdisplayPrivateRec
{
   __GLXDRIdisplay base;
};

struct __GLXDRIcontextPrivateRec
{
   __GLXDRIcontext base;
   __DRIcontext *driContext;
   __GLXscreenConfigs *psc;
};

struct __GLXDRIdrawablePrivateRec
{
   __GLXDRIdrawable base;

   GC gc;
   GC swapgc;

   XVisualInfo *visinfo;
   XImage *ximage;
};

static Bool
XCreateDrawable(__GLXDRIdrawablePrivate * pdp,
                Display * dpy, XID drawable, int visualid)
{
   XGCValues gcvalues;
   long visMask;
   XVisualInfo visTemp;
   int num_visuals;

   /* create GC's */
   pdp->gc = XCreateGC(dpy, drawable, 0, NULL);
   pdp->swapgc = XCreateGC(dpy, drawable, 0, NULL);

   gcvalues.function = GXcopy;
   gcvalues.graphics_exposures = False;
   XChangeGC(dpy, pdp->gc, GCFunction, &gcvalues);
   XChangeGC(dpy, pdp->swapgc, GCFunction, &gcvalues);
   XChangeGC(dpy, pdp->swapgc, GCGraphicsExposures, &gcvalues);

   /* visual */
   visTemp.screen = DefaultScreen(dpy);
   visTemp.visualid = visualid;
   visMask = (VisualScreenMask | VisualIDMask);
   pdp->visinfo = XGetVisualInfo(dpy, visMask, &visTemp, &num_visuals);

   /* create XImage */
   pdp->ximage = XCreateImage(dpy,
                              pdp->visinfo->visual,
                              pdp->visinfo->depth,
                              ZPixmap, 0,             /* format, offset */
                              NULL,                   /* data */
                              0, 0,                   /* width, height */
                              32,                     /* bitmap_pad */
                              0);                     /* bytes_per_line */

   return True;
}

static void
XDestroyDrawable(__GLXDRIdrawablePrivate * pdp, Display * dpy, XID drawable)
{
   XDestroyImage(pdp->ximage);
   XFree(pdp->visinfo);

   XFreeGC(dpy, pdp->gc);
   XFreeGC(dpy, pdp->swapgc);
}

/**
 * swrast loader functions
 */

static void
swrastGetDrawableInfo(__DRIdrawable * draw,
                      int *x, int *y, int *w, int *h,
                      void *loaderPrivate)
{
   __GLXDRIdrawablePrivate *pdp = loaderPrivate;
   __GLXDRIdrawable *pdraw = &(pdp->base);
   Display *dpy = pdraw->psc->dpy;
   Drawable drawable;

   Window root;
   Status stat;
   unsigned uw, uh, bw, depth;

   drawable = pdraw->xDrawable;

   stat = XGetGeometry(dpy, drawable, &root,
                       x, y, &uw, &uh, &bw, &depth);
   *w = uw;
   *h = uh;
}

/**
 * Align renderbuffer pitch.
 *
 * This should be chosen by the driver and the loader (libGL, xserver/glx)
 * should use the driver provided pitch.
 *
 * It seems that the xorg loader (that is the xserver loading swrast_dri for
 * indirect rendering, not client-side libGL) requires that the pitch is
 * exactly the image width padded to 32 bits. XXX
 *
 * The above restriction can probably be overcome by using ScratchPixmap and
 * CopyArea in the xserver, similar to ShmPutImage, and setting the width of
 * the scratch pixmap to 'pitch / cpp'.
 */
static inline int
bytes_per_line(unsigned pitch_bits, unsigned mul)
{
   unsigned mask = mul - 1;

   return ((pitch_bits + mask) & ~mask) / 8;
}

static void
swrastPutImage(__DRIdrawable * draw, int op,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   __GLXDRIdrawablePrivate *pdp = loaderPrivate;
   __GLXDRIdrawable *pdraw = &(pdp->base);
   Display *dpy = pdraw->psc->dpy;
   Drawable drawable;
   XImage *ximage;
   GC gc;

   switch (op) {
   case __DRI_SWRAST_IMAGE_OP_DRAW:
      gc = pdp->gc;
      break;
   case __DRI_SWRAST_IMAGE_OP_SWAP:
      gc = pdp->swapgc;
      break;
   default:
      return;
   }

   drawable = pdraw->xDrawable;

   ximage = pdp->ximage;
   ximage->data = data;
   ximage->width = w;
   ximage->height = h;
   ximage->bytes_per_line = bytes_per_line(w * ximage->bits_per_pixel, 32);

   XPutImage(dpy, drawable, gc, ximage, 0, 0, x, y, w, h);

   ximage->data = NULL;
}

static void
swrastGetImage(__DRIdrawable * read,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   __GLXDRIdrawablePrivate *prp = loaderPrivate;
   __GLXDRIdrawable *pread = &(prp->base);
   Display *dpy = pread->psc->dpy;
   Drawable readable;
   XImage *ximage;

   readable = pread->xDrawable;

   ximage = prp->ximage;
   ximage->data = data;
   ximage->width = w;
   ximage->height = h;
   ximage->bytes_per_line = bytes_per_line(w * ximage->bits_per_pixel, 32);

   XGetSubImage(dpy, readable, x, y, w, h, ~0L, ZPixmap, ximage, 0, 0);

   ximage->data = NULL;
}

static const __DRIswrastLoaderExtension swrastLoaderExtension = {
   {__DRI_SWRAST_LOADER, __DRI_SWRAST_LOADER_VERSION},
   swrastGetDrawableInfo,
   swrastPutImage,
   swrastGetImage
};

static const __DRIextension *loader_extensions[] = {
   &systemTimeExtension.base,
   &swrastLoaderExtension.base,
   NULL
};

/**
 * GLXDRI functions
 */

static void
driDestroyContext(__GLXDRIcontext * context,
                  __GLXscreenConfigs * psc, Display * dpy)
{
   __GLXDRIcontextPrivate *pcp = (__GLXDRIcontextPrivate *) context;
   const __DRIcoreExtension *core = pcp->psc->core;

   (*core->destroyContext) (pcp->driContext);

   Xfree(pcp);
}

static Bool
driBindContext(__GLXDRIcontext * context,
               __GLXDRIdrawable * draw, __GLXDRIdrawable * read)
{
   __GLXDRIcontextPrivate *pcp = (__GLXDRIcontextPrivate *) context;
   const __DRIcoreExtension *core = pcp->psc->core;

   return (*core->bindContext) (pcp->driContext,
                                draw->driDrawable, read->driDrawable);
}

static void
driUnbindContext(__GLXDRIcontext * context)
{
   __GLXDRIcontextPrivate *pcp = (__GLXDRIcontextPrivate *) context;
   const __DRIcoreExtension *core = pcp->psc->core;

   (*core->unbindContext) (pcp->driContext);
}

static __GLXDRIcontext *
driCreateContext(__GLXscreenConfigs * psc,
                 const __GLcontextModes * mode,
                 GLXContext gc, GLXContext shareList, int renderType)
{
   __GLXDRIcontextPrivate *pcp, *pcp_shared;
   __GLXDRIconfigPrivate *config = (__GLXDRIconfigPrivate *) mode;
   const __DRIcoreExtension *core;
   __DRIcontext *shared = NULL;

   if (!psc || !psc->driScreen)
      return NULL;

   core = psc->core;

   if (shareList) {
      pcp_shared = (__GLXDRIcontextPrivate *) shareList->driContext;
      shared = pcp_shared->driContext;
   }

   pcp = Xmalloc(sizeof *pcp);
   if (pcp == NULL)
      return NULL;

   pcp->psc = psc;
   pcp->driContext =
      (*core->createNewContext) (psc->__driScreen,
                                 config->driConfig, shared, pcp);
   if (pcp->driContext == NULL) {
      Xfree(pcp);
      return NULL;
   }

   pcp->base.destroyContext = driDestroyContext;
   pcp->base.bindContext = driBindContext;
   pcp->base.unbindContext = driUnbindContext;

   return &pcp->base;
}

static void
driDestroyDrawable(__GLXDRIdrawable * pdraw)
{
   __GLXDRIdrawablePrivate *pdp = (__GLXDRIdrawablePrivate *) pdraw;
   const __DRIcoreExtension *core = pdraw->psc->core;

   (*core->destroyDrawable) (pdraw->driDrawable);

   XDestroyDrawable(pdp, pdraw->psc->dpy, pdraw->drawable);
   Xfree(pdp);
}

static __GLXDRIdrawable *
driCreateDrawable(__GLXscreenConfigs * psc,
                  XID xDrawable,
                  GLXDrawable drawable, const __GLcontextModes * modes)
{
   __GLXDRIdrawable *pdraw;
   __GLXDRIdrawablePrivate *pdp;
   __GLXDRIconfigPrivate *config = (__GLXDRIconfigPrivate *) modes;
   const __DRIswrastExtension *swrast = psc->swrast;

   /* Old dri can't handle GLX 1.3+ drawable constructors. */
   if (xDrawable != drawable)
      return NULL;

   pdp = Xmalloc(sizeof(*pdp));
   if (!pdp)
      return NULL;

   pdraw = &(pdp->base);
   pdraw->xDrawable = xDrawable;
   pdraw->drawable = drawable;
   pdraw->psc = psc;

   XCreateDrawable(pdp, psc->dpy, xDrawable, modes->visualID);

   /* Create a new drawable */
   pdraw->driDrawable =
      (*swrast->createNewDrawable) (psc->__driScreen, config->driConfig, pdp);

   if (!pdraw->driDrawable) {
      XDestroyDrawable(pdp, psc->dpy, xDrawable);
      Xfree(pdp);
      return NULL;
   }

   pdraw->destroyDrawable = driDestroyDrawable;

   return pdraw;
}

static int64_t
driSwapBuffers(__GLXDRIdrawable * pdraw,
               int64_t target_msc, int64_t divisor, int64_t remainder)
{
   (void) target_msc;
   (void) divisor;
   (void) remainder;

   (*pdraw->psc->core->swapBuffers) (pdraw->driDrawable);

   return 0;
}

static void
driDestroyScreen(__GLXscreenConfigs * psc)
{
   /* Free the direct rendering per screen data */
   (*psc->core->destroyScreen) (psc->__driScreen);
   psc->__driScreen = NULL;
   if (psc->driver)
      dlclose(psc->driver);
}

static void *
driOpenSwrast(void)
{
   void *driver = NULL;

   if (driver == NULL)
      driver = driOpenDriver("swrast");

   if (driver == NULL)
      driver = driOpenDriver("swrastg");

   return driver;
}

static __GLXDRIscreen *
driCreateScreen(__GLXscreenConfigs * psc, int screen,
                __GLXdisplayPrivate * priv)
{
   __GLXDRIscreen *psp;
   const __DRIconfig **driver_configs;
   const __DRIextension **extensions;
   int i;

   psp = Xcalloc(1, sizeof *psp);
   if (psp == NULL)
      return NULL;

   psc->driver = driOpenSwrast();
   if (psc->driver == NULL)
      goto handle_error;

   extensions = dlsym(psc->driver, __DRI_DRIVER_EXTENSIONS);
   if (extensions == NULL) {
      ErrorMessageF("driver exports no extensions (%s)\n", dlerror());
      goto handle_error;
   }

   for (i = 0; extensions[i]; i++) {
      if (strcmp(extensions[i]->name, __DRI_CORE) == 0)
         psc->core = (__DRIcoreExtension *) extensions[i];
      if (strcmp(extensions[i]->name, __DRI_SWRAST) == 0)
         psc->swrast = (__DRIswrastExtension *) extensions[i];
   }

   if (psc->core == NULL || psc->swrast == NULL) {
      ErrorMessageF("core dri extension not found\n");
      goto handle_error;
   }

   psc->__driScreen =
      psc->swrast->createNewScreen(screen,
                                   loader_extensions, &driver_configs, psc);
   if (psc->__driScreen == NULL) {
      ErrorMessageF("failed to create dri screen\n");
      goto handle_error;
   }

   driBindExtensions(psc);
   driBindCommonExtensions(psc);

   psc->configs = driConvertConfigs(psc->core, psc->configs, driver_configs);
   psc->visuals = driConvertConfigs(psc->core, psc->visuals, driver_configs);

   psc->driver_configs = driver_configs;

   psp->destroyScreen = driDestroyScreen;
   psp->createContext = driCreateContext;
   psp->createDrawable = driCreateDrawable;
   psp->swapBuffers = driSwapBuffers;
   psp->waitX = NULL;
   psp->waitGL = NULL;

   return psp;

 handle_error:
   Xfree(psp);

   if (psc->driver)
      dlclose(psc->driver);

   ErrorMessageF("reverting to indirect rendering\n");

   return NULL;
}

/* Called from __glXFreeDisplayPrivate.
 */
static void
driDestroyDisplay(__GLXDRIdisplay * dpy)
{
   Xfree(dpy);
}

/*
 * Allocate, initialize and return a __DRIdisplayPrivate object.
 * This is called from __glXInitialize() when we are given a new
 * display pointer.
 */
_X_HIDDEN __GLXDRIdisplay *
driswCreateDisplay(Display * dpy)
{
   __GLXDRIdisplayPrivate *pdpyp;

   pdpyp = Xmalloc(sizeof *pdpyp);
   if (pdpyp == NULL)
      return NULL;

   pdpyp->base.destroyDisplay = driDestroyDisplay;
   pdpyp->base.createScreen = driCreateScreen;

   return &pdpyp->base;
}

#endif /* GLX_DIRECT_RENDERING */
