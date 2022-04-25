/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef _HAVE_PSPLASH_FB_H
#define _HAVE_PSPLASH_FB_H

#include <linux/fb.h>
#include "psplash-draw.h"

typedef struct PSplashFB
{
  PSplashCanvas  canvas;

  int            fd;
  struct fb_var_screeninfo fb_var;
  int            type;
  int            visual;
  char		*data;
  char		*base;

  /* Support for double buffering */
  int		double_buffering;
  char		*bdata;
  char		*fdata;

  int            fbdev_id;
  int            real_width, real_height;
}
PSplashFB;

void
psplash_fb_destroy (PSplashFB *fb);

PSplashFB*
psplash_fb_new (int angle, int fbdev_id);

void
psplash_fb_text_size (int                *width,
		      int                *height,
		      const PSplashFont  *font,
		      const char         *text);

void
psplash_fb_draw_text (PSplashFB         *fb,
		      int                x,
		      int                y,
		      uint8              red,
		      uint8              green,
		      uint8              blue,
		      const PSplashFont *font,
		      const char        *text);

void
psplash_fb_flip(PSplashFB *fb, int sync);

#endif
