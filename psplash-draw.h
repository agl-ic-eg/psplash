/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#ifndef _HAVE_PSPLASH_CANVAS_H
#define _HAVE_PSPLASH_CANVAS_H

#include "psplash.h"

enum RGBMode {
    RGB565,
    BGR565,
    RGB888,
    BGR888,
    GENERIC,
};

typedef struct PSplashCanvas
{
  int            width, height;
  int            bpp;
  int            stride;
  char		*data;

  int            angle;

  enum RGBMode   rgbmode;
  int            red_offset;
  int            red_length;
  int            green_offset;
  int            green_length;
  int            blue_offset;
  int            blue_length;
}
PSplashCanvas;

void
psplash_draw_rect(PSplashCanvas *canvas,
		  int            x,
		  int            y,
		  int            width,
		  int            height,
		  uint8          red,
		  uint8          green,
		  uint8          blue);

void
psplash_draw_image(PSplashCanvas *canvas,
		   int            x,
		   int            y,
		   int            img_width,
		   int            img_height,
		   int            img_bytes_per_pixel,
		   int            img_rowstride,
		   uint8         *rle_data);

void
psplash_text_size(int                *width,
		  int                *height,
		  const PSplashFont  *font,
		  const char         *text);

void
psplash_draw_text(PSplashCanvas     *canvas,
		  int                x,
		  int                y,
		  uint8              red,
		  uint8              green,
		  uint8              blue,
		  const PSplashFont *font,
		  const char        *text);

#endif
