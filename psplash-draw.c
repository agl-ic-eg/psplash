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

static inline void
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

void
psplash_draw_rect(PSplashCanvas *canvas,
		  int            x,
		  int            y,
		  int            width,
		  int            height,
		  uint8          red,
		  uint8          green,
		  uint8          blue)
{
  int dx, dy;

  for (dy=0; dy < height; dy++)
    for (dx=0; dx < width; dx++)
	psplash_plot_pixel(canvas, x+dx, y+dy, red, green, blue);
}

void
psplash_draw_image(PSplashCanvas *canvas,
		   int            x,
		   int            y,
		   int            img_width,
		   int            img_height,
		   int            img_bytes_per_pixel,
		   int            img_rowstride,
		   uint8         *rle_data)
{
  uint8       *p = rle_data;
  int          dx = 0, dy = 0,  total_len;
  unsigned int len;

  total_len = img_rowstride * img_height;

  /* FIXME: Optimise, check for over runs ... */
  while ((p - rle_data) < total_len)
    {
      len = *(p++);

      if (len & 128)
	{
	  len -= 128;

	  if (len == 0) break;

	  do
	    {
	      if ((img_bytes_per_pixel < 4 || *(p+3)) && dx < img_width)
	        psplash_plot_pixel(canvas, x+dx, y+dy, *(p), *(p+1), *(p+2));
	      if (++dx * img_bytes_per_pixel >= img_rowstride) { dx=0; dy++; }
	    }
	  while (--len);

	  p += img_bytes_per_pixel;
	}
      else
	{
	  if (len == 0) break;

	  do
	    {
	      if ((img_bytes_per_pixel < 4 || *(p+3)) && dx < img_width)
	        psplash_plot_pixel(canvas, x+dx, y+dy, *(p), *(p+1), *(p+2));
	      if (++dx * img_bytes_per_pixel >= img_rowstride) { dx=0; dy++; }
	      p += img_bytes_per_pixel;
	    }
	  while (--len && (p - rle_data) < total_len);
	}
    }
}

/* Font rendering code based on BOGL by Ben Pfaff */

static int
psplash_font_glyph (const PSplashFont *font, wchar_t wc, u_int32_t **bitmap)
{
  int mask = font->index_mask;
  int i;

  for (;;)
    {
      for (i = font->offset[wc & mask]; font->index[i]; i += 2)
	{
	  if ((wchar_t)(font->index[i] & ~mask) == (wc & ~mask))
	    {
	      if (bitmap != NULL)
		*bitmap = &font->content[font->index[i+1]];
	      return font->index[i] & mask;
	    }
	}
    }
  return 0;
}

void
psplash_text_size(int                *width,
		  int                *height,
		  const PSplashFont  *font,
		  const char         *text)
{
  char   *c = (char*)text;
  wchar_t wc;
  int     k, n, w, h, mw;

  n = strlen (text);
  mw = h = w = 0;

  mbtowc (0, 0, 0);
  for (; (k = mbtowc (&wc, c, n)) > 0; c += k, n -= k)
    {
      if (*c == '\n')
	{
	  if (w > mw)
	    mw = w;
	  w = 0;
	  h += font->height;
	  continue;
	}

      w += psplash_font_glyph (font, wc, NULL);
    }

  *width  = (w > mw) ? w : mw;
  *height = (h == 0) ? font->height : h;
}

void
psplash_draw_text(PSplashCanvas     *canvas,
		  int                x,
		  int                y,
		  uint8              red,
		  uint8              green,
		  uint8              blue,
		  const PSplashFont *font,
		  const char        *text)
{
  int     h, w, k, n, cx, cy, dx, dy;
  char   *c = (char*)text;
  wchar_t wc;

  n = strlen (text);
  h = font->height;
  dx = dy = 0;

  mbtowc (0, 0, 0);
  for (; (k = mbtowc (&wc, c, n)) > 0; c += k, n -= k)
    {
      u_int32_t *glyph = NULL;

      if (*c == '\n')
	{
	  dy += h;
	  dx  = 0;
	  continue;
	}

      w = psplash_font_glyph (font, wc, &glyph);

      if (glyph == NULL)
	continue;

      for (cy = 0; cy < h; cy++)
	{
	  u_int32_t g = *glyph++;

	  for (cx = 0; cx < w; cx++)
	    {
	      if (g & 0x80000000)
		psplash_plot_pixel(canvas, x+dx+cx, y+dy+cy, red, green, blue);
	      g <<= 1;
	    }
	}

      dx += w;
    }
}
