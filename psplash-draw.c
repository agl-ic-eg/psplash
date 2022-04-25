/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "psplash-draw.h"

#define OFFSET(canvas, x, y) (((y) * (canvas)->stride) + ((x) * ((canvas)->bpp >> 3)))

/* TODO: change to 'static inline' as psplash_fb_plot_pixel was before */
void
psplash_plot_pixel(PSplashCanvas *canvas,
		   int            x,
		   int            y,
		   uint8          red,
		   uint8          green,
		   uint8          blue)
{
  /* Always write to back data (data) which points to the right data with or
   * without double buffering support */
  int off;

  if (x < 0 || x > canvas->width-1 || y < 0 || y > canvas->height-1)
    return;

  switch (canvas->angle)
    {
    case 270:
      off = OFFSET (canvas, canvas->height - y - 1, x);
      break;
    case 180:
      off = OFFSET (canvas, canvas->width - x - 1, canvas->height - y - 1);
      break;
    case 90:
      off = OFFSET (canvas, y, canvas->width - x - 1);
      break;
    case 0:
    default:
      off = OFFSET (canvas, x, y);
      break;
    }

  if (canvas->rgbmode == RGB565 || canvas->rgbmode == RGB888) {
    switch (canvas->bpp)
      {
      case 24:
#if __BYTE_ORDER == __BIG_ENDIAN
        *(canvas->data + off + 0) = red;
        *(canvas->data + off + 1) = green;
        *(canvas->data + off + 2) = blue;
#else
        *(canvas->data + off + 0) = blue;
        *(canvas->data + off + 1) = green;
        *(canvas->data + off + 2) = red;
#endif
        break;
      case 32:
        *(volatile uint32_t *) (canvas->data + off)
          = (red << 16) | (green << 8) | (blue);
        break;

      case 16:
        *(volatile uint16_t *) (canvas->data + off)
	  = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
        break;
      default:
        /* depth not supported yet */
        break;
      }
  } else if (canvas->rgbmode == BGR565 || canvas->rgbmode == BGR888) {
    switch (canvas->bpp)
      {
      case 24:
#if __BYTE_ORDER == __BIG_ENDIAN
        *(canvas->data + off + 0) = blue;
        *(canvas->data + off + 1) = green;
        *(canvas->data + off + 2) = red;
#else
        *(canvas->data + off + 0) = red;
        *(canvas->data + off + 1) = green;
        *(canvas->data + off + 2) = blue;
#endif
        break;
      case 32:
        *(volatile uint32_t *) (canvas->data + off)
          = (blue << 16) | (green << 8) | (red);
        break;
      case 16:
        *(volatile uint16_t *) (canvas->data + off)
	  = ((blue >> 3) << 11) | ((green >> 2) << 5) | (red >> 3);
        break;
      default:
        /* depth not supported yet */
        break;
      }
  } else {
    switch (canvas->bpp)
      {
      case 32:
        *(volatile uint32_t *) (canvas->data + off)
	  = ((red >> (8 - canvas->red_length)) << canvas->red_offset)
	      | ((green >> (8 - canvas->green_length)) << canvas->green_offset)
	      | ((blue >> (8 - canvas->blue_length)) << canvas->blue_offset);
        break;
      case 16:
        *(volatile uint16_t *) (canvas->data + off)
	  = ((red >> (8 - canvas->red_length)) << canvas->red_offset)
	      | ((green >> (8 - canvas->green_length)) << canvas->green_offset)
	      | ((blue >> (8 - canvas->blue_length)) << canvas->blue_offset);
        break;
      default:
        /* depth not supported yet */
        break;
      }
  }
}
