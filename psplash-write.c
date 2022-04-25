/*
 *  pslash - a lightweight framebuffer splashscreen for embedded devices.
 *
 *  Copyright (c) 2006 Matthew Allum <mallum@o-hand.com>
 *
 *  Parts of this file based on 'usplash' copyright Matthew Garret.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "psplash.h"

int main(int argc, char **argv)
{
  char   *rundir;
  int     pipe_fd;
  size_t  count;
  ssize_t written;

  rundir = getenv("PSPLASH_FIFO_DIR");

  if (!rundir)
    rundir = "/run";

  if (argc!=2)
    {
      fprintf(stderr, "Wrong number of arguments\n");
      exit(-1);
    }

  if (chdir(rundir)) {
    perror("chdir");
    exit(-1);
  }

  if ((pipe_fd = open (PSPLASH_FIFO,O_WRONLY|O_NONBLOCK)) == -1)
    {
      /* Silently error out instead of covering the boot process in
         errors when psplash has exitted due to a VC switch */
      /* perror("Error unable to open fifo"); */
      exit (-1);
    }

  count = strlen(argv[1]) + 1;
  written = write(pipe_fd, argv[1], count);
  if (written == -1) {
    perror("write");
    exit(-1);
  } else if ((size_t)written < count) {
    fprintf(stderr, "Wrote %zd bytes, less then expected %zu bytes\n",
      written, count);
    exit(-1);
  }

  return 0;
}
