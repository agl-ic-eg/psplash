/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  Parts of this file ( fifo handling ) based on 'usplash' copyright
 *  Matthew Garret.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "psplash.h"
#include "psplash-fb.h"
#ifdef ENABLE_DRM
#include "psplash-drm.h"
#endif
#ifdef ENABLE_DRM_LEASE
#include "psplash-drm-lease.h"
#endif
#include "psplash-config.h"
#include "psplash-colors.h"
#include "psplash-poky-img.h"
#include "psplash-bar-img.h"
#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif
#include FONT_HEADER

#define SPLIT_LINE_POS(canvas)                                  \
	(  (canvas)->height                                     \
	 - ((  PSPLASH_IMG_SPLIT_DENOMINATOR                    \
	     - PSPLASH_IMG_SPLIT_NUMERATOR)                     \
	    * (canvas)->height / PSPLASH_IMG_SPLIT_DENOMINATOR) \
	)

void
psplash_exit (int UNUSED(signum))
{
  DBG("mark");

  psplash_console_reset ();
}

void
psplash_draw_msg(PSplashCanvas *canvas, const char *msg)
{
  int w, h;

  psplash_text_size(&w, &h, &FONT_DEF, msg);

  DBG("displaying '%s' %ix%i\n", msg, w, h);

  /* Clear */

  psplash_draw_rect(canvas,
			0,
			SPLIT_LINE_POS(canvas) - h,
			canvas->width,
			h,
			PSPLASH_BACKGROUND_COLOR);

  psplash_draw_text(canvas,
			(canvas->width-w)/2,
			SPLIT_LINE_POS(canvas) - h,
			PSPLASH_TEXT_COLOR,
			&FONT_DEF,
			msg);
}

#ifdef PSPLASH_SHOW_PROGRESS_BAR
void
psplash_draw_progress(PSplashCanvas *canvas, int value)
{
  int x, y, width, height, barwidth;

  /* 4 pix border */
  x      = ((canvas->width  - BAR_IMG_WIDTH)/2) + 4 ;
  y      = SPLIT_LINE_POS(canvas) + 4;
  width  = BAR_IMG_WIDTH - 8;
  height = BAR_IMG_HEIGHT - 8;

  if (value > 0)
    {
      barwidth = (CLAMP(value,0,100) * width) / 100;
      psplash_draw_rect(canvas, x + barwidth, y,
    			width - barwidth, height,
			PSPLASH_BAR_BACKGROUND_COLOR);
      psplash_draw_rect(canvas, x, y, barwidth,
			    height, PSPLASH_BAR_COLOR);
    }
  else
    {
      barwidth = (CLAMP(-value,0,100) * width) / 100;
      psplash_draw_rect(canvas, x, y,
    			width - barwidth, height,
			PSPLASH_BAR_BACKGROUND_COLOR);
      psplash_draw_rect(canvas, x + width - barwidth,
			    y, barwidth, height,
			    PSPLASH_BAR_COLOR);
    }

  DBG("value: %i, width: %i, barwidth :%i\n", value,
		width, barwidth);
}
#endif /* PSPLASH_SHOW_PROGRESS_BAR */

static int
parse_command(PSplashCanvas *canvas, char *string)
{
  char *command;

  DBG("got cmd %s", string);

  if (strcmp(string,"QUIT") == 0)
    return 1;

  command = strtok(string," ");

  if (!strcmp(command,"MSG"))
    {
      char *arg = strtok(NULL, "\0");

      if (arg)
        psplash_draw_msg(canvas, arg);
    }
 #ifdef PSPLASH_SHOW_PROGRESS_BAR
  else  if (!strcmp(command,"PROGRESS"))
    {
      char *arg = strtok(NULL, "\0");

      if (arg)
        psplash_draw_progress(canvas, atoi(arg));
    }
#endif
  else if (!strcmp(command,"QUIT"))
    {
      return 1;
    }

  canvas->flip(canvas, 0);
  return 0;
}

void
psplash_main(PSplashCanvas *canvas, int pipe_fd, int timeout)
{
  int            err;
  ssize_t        length = 0;
  fd_set         descriptors;
  struct timeval tv;
  char          *end;
  char          *cmd;
  char           command[2048];

  tv.tv_sec = timeout;
  tv.tv_usec = 0;

  FD_ZERO(&descriptors);
  FD_SET(pipe_fd, &descriptors);

  end = command;

  while (1)
    {
      if (timeout != 0)
	err = select(pipe_fd+1, &descriptors, NULL, NULL, &tv);
      else
	err = select(pipe_fd+1, &descriptors, NULL, NULL, NULL);

      if (err <= 0)
	{
	  /*
	  if (errno == EINTR)
	    continue;
	  */
	  return;
	}

      length += read (pipe_fd, end, sizeof(command) - (end - command));

      if (length == 0)
	{
	  /* Reopen to see if there's anything more for us */
	  close(pipe_fd);
	  pipe_fd = open(PSPLASH_FIFO,O_RDONLY|O_NONBLOCK);
	  goto out;
	}

      cmd = command;
      do {
	int cmdlen;
        char *cmdend = memchr(cmd, '\n', length);

        /* Replace newlines with string termination */
        if (cmdend)
            *cmdend = '\0';

        cmdlen = strnlen(cmd, length);

        /* Skip string terminations */
	if (!cmdlen && length)
          {
            length--;
            cmd++;
	    continue;
          }

	if (parse_command(canvas, cmd))
	  return;

	length -= cmdlen;
	cmd += cmdlen;
      } while (length);

    out:
      end = &command[length];

      tv.tv_sec = timeout;
      tv.tv_usec = 0;

      FD_ZERO(&descriptors);
      FD_SET(pipe_fd,&descriptors);
    }

  return;
}

int
main (int argc, char** argv)
{
  char      *rundir;
  int        pipe_fd, i = 0, angle = 0, dev_id = 0, use_drm = 0, ret = 0;
  PSplashFB *fb = NULL;
#ifdef ENABLE_DRM
  PSplashDRM *drm = NULL;
#endif
  PSplashCanvas *canvas;
  bool       disable_console_switch = FALSE;

  signal(SIGHUP, psplash_exit);
  signal(SIGINT, psplash_exit);
  signal(SIGQUIT, psplash_exit);

  while (++i < argc) {
    if (!strcmp(argv[i],"-n") || !strcmp(argv[i],"--no-console-switch"))
      {
        disable_console_switch = TRUE;
        continue;
      }

    if (!strcmp(argv[i],"-a") || !strcmp(argv[i],"--angle"))
      {
        if (++i >= argc) goto fail;
        angle = atoi(argv[i]);
        continue;
      }

    if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fbdev") ||
        !strcmp(argv[i], "-d") || !strcmp(argv[i], "--dev"))
      {
        if (++i >= argc) goto fail;
        dev_id = atoi(argv[i]);
        continue;
      }

#ifdef ENABLE_DRM
    if (!strcmp(argv[i], "--drm")) {
        use_drm = 1;
        continue;
    }
#endif
#ifdef ENABLE_DRM_LEASE
    if (!strcmp(argv[i],"--drm-lease"))
      {
        if (++i >= argc) goto fail;
        drm_set_lease_name(argv[i]);
        use_drm = 1;
        continue;
      }
#endif

    fail:
      fprintf(stderr,
              "Usage: %s [-n|--no-console-switch][-a|--angle <0|90|180|270>][-f|--fbdev|-d|--dev <0..9>][--drm]\n",
              argv[0]);
      exit(-1);
  }

  rundir = getenv("PSPLASH_FIFO_DIR");

  if (!rundir)
    rundir = "/run";

  if (chdir(rundir)) {
    perror("chdir");
    exit(-1);
  }

  if (mkfifo(PSPLASH_FIFO, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP))
    {
      if (errno!=EEXIST)
	    {
	      perror("mkfifo");
	      exit(-1);
	    }
    }

  pipe_fd = open (PSPLASH_FIFO,O_RDONLY|O_NONBLOCK);

  if (pipe_fd==-1)
    {
      perror("pipe open");
      exit(-2);
    }

  if (!disable_console_switch)
    psplash_console_switch ();

  if (use_drm) {
#ifdef ENABLE_DRM
    if ((drm = psplash_drm_new(angle, dev_id)) == NULL) {
      ret = -1;
      goto error;
    }
    canvas = &drm->canvas;
#endif
  } else {
    if ((fb = psplash_fb_new(angle, dev_id)) == NULL) {
      ret = -1;
      goto error;
    }
    canvas = &fb->canvas;
  }

#ifdef HAVE_SYSTEMD
  sd_notify(0, "READY=1");
#endif

  /* Clear the background with #ecece1 */
  psplash_draw_rect(canvas, 0, 0, canvas->width, canvas->height,
                        PSPLASH_BACKGROUND_COLOR);

  /* Draw the Poky logo  */
  psplash_draw_image(canvas,
			 (canvas->width  - POKY_IMG_WIDTH)/2,
#if PSPLASH_IMG_FULLSCREEN
			 (canvas->height - POKY_IMG_HEIGHT)/2,
#else
			 (canvas->height * PSPLASH_IMG_SPLIT_NUMERATOR
			  / PSPLASH_IMG_SPLIT_DENOMINATOR - POKY_IMG_HEIGHT)/2,
#endif
			 POKY_IMG_WIDTH,
			 POKY_IMG_HEIGHT,
			 POKY_IMG_BYTES_PER_PIXEL,
			 POKY_IMG_ROWSTRIDE,
			 POKY_IMG_RLE_PIXEL_DATA);

#ifdef PSPLASH_SHOW_PROGRESS_BAR
  /* Draw progress bar border */
  psplash_draw_image(canvas,
			 (canvas->width  - BAR_IMG_WIDTH)/2,
			 SPLIT_LINE_POS(canvas),
			 BAR_IMG_WIDTH,
			 BAR_IMG_HEIGHT,
			 BAR_IMG_BYTES_PER_PIXEL,
			 BAR_IMG_ROWSTRIDE,
			 BAR_IMG_RLE_PIXEL_DATA);

  psplash_draw_progress(canvas, 0);
#endif

#ifdef PSPLASH_STARTUP_MSG
  psplash_draw_msg(canvas, PSPLASH_STARTUP_MSG);
#endif

  /* Scene set so let's flip the buffers. */
  /* The first time we also synchronize the buffers so we can build on an
   * existing scene. After the first scene is set in both buffers, only the
   * text and progress bar change which overwrite the specific areas with every
   * update.
   */
  canvas->flip(canvas, 1);

  psplash_main(canvas, pipe_fd, 0);

  if (fb)
    psplash_fb_destroy(fb);
#ifdef ENABLE_DRM
  if (drm)
    psplash_drm_destroy(drm);
#endif

 error:
  unlink(PSPLASH_FIFO);

  if (!disable_console_switch)
    psplash_console_reset ();

  return ret;
}
