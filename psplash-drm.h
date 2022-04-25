#ifndef _HAVE_PSPLASH_DRM_H
#define _HAVE_PSPLASH_DRM_H

#include "psplash-draw.h"

typedef struct PSplashDRM
{
  PSplashCanvas canvas;
  int fd;
}
PSplashDRM;

void psplash_drm_destroy(PSplashDRM *drm);

PSplashDRM* psplash_drm_new(int angle, int dev_id);

#endif
