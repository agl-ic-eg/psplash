/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include <endian.h>
#include "psplash-fb.h"

static void
psplash_wait_for_vsync(PSplashFB *fb)
{
  int err = ioctl(fb->fd, FBIO_WAITFORVSYNC, 0);
  if (err != 0)
    fprintf(stderr, "Error, FB vsync ioctl [%d]\n", err);
}

static void
psplash_fb_flip(PSplashCanvas *canvas, int sync)
{
  PSplashFB *fb = canvas->priv;
  char *tmp;

  if (fb->double_buffering) {

    /* Carry out the flip after a vsync */
    psplash_wait_for_vsync(fb);

    /* Switch the current activate area in fb */
    if (fb->fb_var.yoffset == 0 ) {
      fb->fb_var.yoffset = fb->real_height;
    } else {
      fb->fb_var.yoffset = 0;
    }
    if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb->fb_var) == -1 ) {
      fprintf(stderr, "psplash_fb_flip: FBIOPAN_DISPLAY failed\n");
    }

    /* Switch the front and back data pointers */
    tmp = fb->fdata;
    fb->fdata = fb->bdata;
    fb->bdata = tmp;
    fb->canvas.data = fb->bdata;

    /* Sync new front to new back when requested */
    if (sync) {
      memcpy(fb->bdata, fb->fdata, fb->canvas.stride * fb->real_height);
    }
  }
}

void
psplash_fb_destroy (PSplashFB *fb)
{
  if (fb->fd >= 0)
    close (fb->fd);

  free(fb);
}

static int
attempt_to_change_pixel_format (PSplashFB *fb,
                                struct fb_var_screeninfo *fb_var)
{
  /* By default the framebuffer driver may have set an oversized
   * yres_virtual to support VT scrolling via the panning interface.
   *
   * We don't try and maintain this since it's more likely that we
   * will fail to increase the bpp if the driver's pre allocated
   * framebuffer isn't large enough.
   */
  fb_var->yres_virtual = fb_var->yres;

  /* First try setting an 8,8,8,0 pixel format so we don't have to do
   * any conversions while drawing. */

  fb_var->bits_per_pixel = 32;

  fb_var->red.offset = 0;
  fb_var->red.length = 8;

  fb_var->green.offset = 8;
  fb_var->green.length = 8;

  fb_var->blue.offset = 16;
  fb_var->blue.length = 8;

  fb_var->transp.offset = 0;
  fb_var->transp.length = 0;

  if (ioctl (fb->fd, FBIOPUT_VSCREENINFO, fb_var) == 0)
    {
      fprintf(stdout, "Switched to a 32 bpp 8,8,8 frame buffer\n");
      return 1;
    }
  else
    {
      fprintf(stderr,
              "Error, failed to switch to a 32 bpp 8,8,8 frame buffer\n");
    }

  /* Otherwise try a 16bpp 5,6,5 format */

  fb_var->bits_per_pixel = 16;

  fb_var->red.offset = 11;
  fb_var->red.length = 5;

  fb_var->green.offset = 5;
  fb_var->green.length = 6;

  fb_var->blue.offset = 0;
  fb_var->blue.length = 5;

  fb_var->transp.offset = 0;
  fb_var->transp.length = 0;

  if (ioctl (fb->fd, FBIOPUT_VSCREENINFO, fb_var) == 0)
    {
      fprintf(stdout, "Switched to a 16 bpp 5,6,5 frame buffer\n");
      return 1;
    }
  else
    {
      fprintf(stderr,
              "Error, failed to switch to a 16 bpp 5,6,5 frame buffer\n");
    }

  return 0;
}

PSplashFB*
psplash_fb_new (int angle, int fbdev_id)
{
  struct fb_var_screeninfo fb_var;
  struct fb_fix_screeninfo fb_fix;
  int                      off;
  char                     fbdev[9] = "/dev/fb0";

  PSplashFB *fb = NULL;

  if (fbdev_id > 0 && fbdev_id < 10)
    {
        // Conversion from integer to ascii.
        fbdev[7] = fbdev_id + 48;
    }

  if ((fb = malloc (sizeof(PSplashFB))) == NULL)
    {
      perror ("Error no memory");
      goto fail;
    }

  memset (fb, 0, sizeof(PSplashFB));
  fb->canvas.priv = fb;
  fb->canvas.flip = psplash_fb_flip;
  fb->fd = -1;

  if ((fb->fd = open (fbdev, O_RDWR)) < 0)
    {
      fprintf(stderr,
              "Error opening %s\n",
              fbdev);
      goto fail;
    }

  if (ioctl (fb->fd, FBIOGET_VSCREENINFO, &fb_var) == -1)
    {
      perror ("Error getting variable framebuffer info");
      goto fail;
    }

  if (fb_var.bits_per_pixel < 16)
    {
      fprintf(stderr,
              "Error, no support currently for %i bpp frame buffers\n"
              "Trying to change pixel format...\n",
              fb_var.bits_per_pixel);
      if (!attempt_to_change_pixel_format (fb, &fb_var))
        goto fail;
    }

  if (ioctl (fb->fd, FBIOGET_VSCREENINFO, &fb_var) == -1)
    {
      perror ("Error getting variable framebuffer info (2)");
      goto fail;
    }

  /* NB: It looks like the fbdev concept of fixed vs variable screen info is
   * broken. The line_length is part of the fixed info but it can be changed
   * if you set a new pixel format. */
  if (ioctl (fb->fd, FBIOGET_FSCREENINFO, &fb_fix) == -1)
    {
      perror ("Error getting fixed framebuffer info");
      goto fail;
    }

  /* Setup double virtual resolution for double buffering */
  if (ioctl(fb->fd, FBIOPAN_DISPLAY, &fb_var) == -1) {
    fprintf(stderr, "warning: FBIOPAN_DISPLAY not supported, "
                    "double buffering disabled\n");
  } else {
    if (fb_var.yres_virtual == fb_var.yres * 2) {
      DBG("Virtual resolution already double");
      fb->double_buffering = 1;
    } else {
      fb_var.yres_virtual = fb_var.yres * 2;
      if (ioctl(fb->fd, FBIOPUT_VSCREENINFO, &fb_var) == -1) {
        fprintf(stderr, "warning: FBIOPUT_VSCREENINFO failed, "
                        "double buffering disabled\n");
      } else {
        if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb_fix) == -1) {
          perror(" Error getting the fixed framebuffer info");
          goto fail;
        } else {
          DBG("Virtual resolution set to double");
          fb->double_buffering = 1;
        }
      }
    }
  }

  fb->real_width  = fb->canvas.width  = fb_var.xres;
  fb->real_height = fb->canvas.height = fb_var.yres;
  fb->canvas.bpp    = fb_var.bits_per_pixel;
  fb->canvas.stride = fb_fix.line_length;
  fb->type   = fb_fix.type;
  fb->visual = fb_fix.visual;

  fb->canvas.red_offset = fb_var.red.offset;
  fb->canvas.red_length = fb_var.red.length;
  fb->canvas.green_offset = fb_var.green.offset;
  fb->canvas.green_length = fb_var.green.length;
  fb->canvas.blue_offset = fb_var.blue.offset;
  fb->canvas.blue_length = fb_var.blue.length;

  if (fb->canvas.red_offset == 11 && fb->canvas.red_length == 5 &&
      fb->canvas.green_offset == 5 && fb->canvas.green_length == 6 &&
      fb->canvas.blue_offset == 0 && fb->canvas.blue_length == 5) {
         fb->canvas.rgbmode = RGB565;
  } else if (fb->canvas.red_offset == 0 && fb->canvas.red_length == 5 &&
      fb->canvas.green_offset == 5 && fb->canvas.green_length == 6 &&
      fb->canvas.blue_offset == 11 && fb->canvas.blue_length == 5) {
         fb->canvas.rgbmode = BGR565;
  } else if (fb->canvas.red_offset == 16 && fb->canvas.red_length == 8 &&
      fb->canvas.green_offset == 8 && fb->canvas.green_length == 8 &&
      fb->canvas.blue_offset == 0 && fb->canvas.blue_length == 8) {
         fb->canvas.rgbmode = RGB888;
  } else if (fb->canvas.red_offset == 0 && fb->canvas.red_length == 8 &&
      fb->canvas.green_offset == 8 && fb->canvas.green_length == 8 &&
      fb->canvas.blue_offset == 16 && fb->canvas.blue_length == 8) {
         fb->canvas.rgbmode = BGR888;
  } else {
         fb->canvas.rgbmode = GENERIC;
  }

  DBG("width: %i, height: %i, bpp: %i, stride: %i",
      fb->canvas.width, fb->canvas.height, fb->canvas.bpp, fb->canvas.stride);

  fb->base = (char *) mmap ((caddr_t) NULL,
			    fb_fix.smem_len,
			    PROT_READ|PROT_WRITE,
			    MAP_SHARED,
			    fb->fd, 0);

  if (fb->base == (char *)-1)
    {
      perror("Error cannot mmap framebuffer ");
      goto fail;
    }

  off = (unsigned long) fb_fix.smem_start % (unsigned long) getpagesize();

  fb->data = fb->base + off;

  if (fb->double_buffering) {
    /* fb_var is needed when flipping the buffers */
    memcpy(&fb->fb_var, &fb_var, sizeof(struct fb_var_screeninfo));
    if (fb->fb_var.yoffset == 0) {
      printf("to back\n");
      fb->fdata = fb->data;
      fb->bdata = fb->data + fb->canvas.stride * fb->canvas.height;
    } else {
      printf("to front\n");
      fb->fdata = fb->data + fb->canvas.stride * fb->canvas.height;
      fb->bdata = fb->data;
    }
  } else {
    fb->fdata = fb->data;
    fb->bdata = fb->data;
  }
  fb->canvas.data = fb->bdata;

#if 0
  /* FIXME: No support for 8pp as yet  */
  if (visual == FB_VISUAL_PSEUDOCOLOR
      || visual == FB_VISUAL_STATIC_PSEUDOCOLOR)
  {
    static struct fb_cmap cmap;

    cmap.start = 0;
    cmap.len = 16;
    cmap.red = saved_red;
    cmap.green = saved_green;
    cmap.blue = saved_blue;
    cmap.transp = NULL;

    ioctl (fb, FBIOGETCMAP, &cmap);
  }

  if (!status)
    atexit (bogl_done);
  status = 2;
#endif

  fb->canvas.angle = angle;

  switch (angle)
    {
    case 270:
    case 90:
      fb->canvas.width  = fb->real_height;
      fb->canvas.height = fb->real_width;
      break;
    case 180:
    case 0:
    default:
      break;
    }

  return fb;

 fail:

  if (fb)
    psplash_fb_destroy (fb);

  return NULL;
}
