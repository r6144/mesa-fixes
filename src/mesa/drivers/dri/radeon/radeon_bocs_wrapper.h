#ifndef RADEON_CS_WRAPPER_H
#define RADEON_CS_WRAPPER_H

#ifdef HAVE_LIBDRM_RADEON

#include "radeon_bo.h"
#include "radeon_bo_gem.h"
#include "radeon_cs.h"
#include "radeon_cs_gem.h"

#else
#define RADEON_GEM_DOMAIN_CPU 0x1   // Cached CPU domain
#define RADEON_GEM_DOMAIN_GTT 0x2   // GTT or cache flushed
#define RADEON_GEM_DOMAIN_VRAM 0x4  // VRAM domain

#ifndef DRM_RADEON_GEM_INFO
#define DRM_RADEON_GEM_INFO 0x1c
#endif

#ifndef RADEON_PARAM_DEVICE_ID
#define RADEON_PARAM_DEVICE_ID 17
#endif

/* to be used to build locally in mesa with no libdrm bits */
#include "../radeon/radeon_bo_drm.h"
#include "../radeon/radeon_cs_drm.h"


static inline void *radeon_bo_manager_gem_ctor(int fd)
{
  return NULL;
}

static inline void radeon_bo_manager_gem_dtor(void *dummy)
{
}

static inline void *radeon_cs_manager_gem_ctor(int fd)
{
  return NULL;
}

static inline void radeon_cs_manager_gem_dtor(void *dummy)
{
}

static inline void radeon_tracker_print(void *ptr, int io)
{
}
#endif

#include "radeon_bo_legacy.h"
#include "radeon_cs_legacy.h"

#endif