/*
 * modeset - DRM Modesetting Example
 *
 * Written 2012 by David Rheinsberg <david.rheinsberg@gmail.com>
 * Dedicated to the Public Domain.
 */

/*
 * DRM Modesetting Howto
 * This document describes the DRM modesetting API. Before we can use the DRM
 * API, we have to include xf86drm.h and xf86drmMode.h. Both are provided by
 * libdrm which every major distribution ships by default. It has no other
 * dependencies and is pretty small.
 *
 * Please ignore all forward-declarations of functions which are used later. I
 * reordered the functions so you can read this document from top to bottom. If
 * you reimplement it, you would probably reorder the functions to avoid all the
 * nasty forward declarations.
 *
 * For easier reading, we ignore all memory-allocation errors of malloc() and
 * friends here. However, we try to correctly handle all other kinds of errors
 * that may occur.
 *
 * All functions and global variables are prefixed with "modeset_*" in this
 * file. So it should be clear whether a function is a local helper or if it is
 * provided by some external library.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "psplash-drm.h"
#ifdef ENABLE_DRM_LEASE
#include "psplash-drm-lease.h"
#endif

#define MIN(a,b) ((a) < (b) ? (a) : (b))

struct modeset_buf;
struct modeset_dev;
static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_create_fb(int fd, struct modeset_buf *buf);
static void modeset_destroy_fb(int fd, struct modeset_buf *buf);
static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev);
static int modeset_open(int *out, const char *node);
static int modeset_prepare(int fd);

#ifdef ENABLE_DRM_LEASE
char *drm_lease_name;
#endif

/*
 * When the linux kernel detects a graphics-card on your machine, it loads the
 * correct device driver (located in kernel-tree at ./drivers/gpu/drm/<xy>) and
 * provides two character-devices to control it. Udev (or whatever hotplugging
 * application you use) will create them as:
 *     /dev/dri/card0
 *     /dev/dri/controlID64
 * We only need the first one. You can hard-code this path into your application
 * like we do here, but it is recommended to use libudev with real hotplugging
 * and multi-seat support. However, this is beyond the scope of this document.
 * Also note that if you have multiple graphics-cards, there may also be
 * /dev/dri/card1, /dev/dri/card2, ...
 *
 * We simply use /dev/dri/card0 here but the user can specify another path on
 * the command line.
 *
 * modeset_open(out, node): This small helper function opens the DRM device
 * which is given as @node. The new fd is stored in @out on success. On failure,
 * a negative error code is returned.
 * After opening the file, we also check for the DRM_CAP_DUMB_BUFFER capability.
 * If the driver supports this capability, we can create simple memory-mapped
 * buffers without any driver-dependent code. As we want to avoid any radeon,
 * nvidia, intel, etc. specific code, we depend on DUMB_BUFFERs here.
 */

static int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t has_dumb;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	    !has_dumb) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

/*
 * As a next step we need to find our available display devices. libdrm provides
 * a drmModeRes structure that contains all the needed information. We can
 * retrieve it via drmModeGetResources(fd) and free it via
 * drmModeFreeResources(res) again.
 *
 * A physical connector on your graphics card is called a "connector". You can
 * plug a monitor into it and control what is displayed. We are definitely
 * interested in what connectors are currently used, so we simply iterate
 * through the list of connectors and try to display a test-picture on each
 * available monitor.
 * However, this isn't as easy as it sounds. First, we need to check whether the
 * connector is actually used (a monitor is plugged in and turned on). Then we
 * need to find a CRTC that can control this connector. CRTCs are described
 * later on. After that we create a framebuffer object. If we have all this, we
 * can mmap() the framebuffer and draw a test-picture into it. Then we can tell
 * the DRM device to show the framebuffer on the given CRTC with the selected
 * connector.
 *
 * As we want to draw moving pictures on the framebuffer, we actually have to
 * remember all these settings. Therefore, we create one "struct modeset_dev"
 * object for each connector+crtc+framebuffer pair that we successfully
 * initialized and push it into the global device-list.
 *
 * Each field of this structure is described when it is first used. But as a
 * summary:
 * "struct modeset_dev" contains: {
 *  - @next: points to the next device in the single-linked list
 *
 *  - @width: width of our buffer object
 *  - @height: height of our buffer object
 *  - @stride: stride value of our buffer object
 *  - @size: size of the memory mapped buffer
 *  - @handle: a DRM handle to the buffer object that we can draw into
 *  - @map: pointer to the memory mapped buffer
 *
 *  - @mode: the display mode that we want to use
 *  - @fb: a framebuffer handle with our buffer object as scanout buffer
 *  - @conn: the connector ID that we want to use with this buffer
 *  - @crtc: the crtc ID that we want to use with this connector
 *  - @saved_crtc: the configuration of the crtc before we changed it. We use it
 *                 so we can restore the same mode when we exit.
 * }
 */

/*
 * Previously, we used the modeset_dev objects to hold buffer informations, too.
 * Technically, we could have split them but avoided this to make the
 * example simpler.
 * However, in this example we need 2 buffers. One back buffer and one front
 * buffer. So we introduce a new structure modeset_buf which contains everything
 * related to a single buffer. Each device now gets an array of two of these
 * buffers.
 * Each buffer consists of width, height, stride, size, handle, map and fb-id.
 * They have the same meaning as before.
 *
 * Each device also gets a new integer field: front_buf. This field contains the
 * index of the buffer that is currently used as front buffer / scanout buffer.
 * In our example it can be 0 or 1. We flip it by using XOR:
 *   dev->front_buf ^= dev->front_buf
 *
 * Everything else stays the same.
 */

struct modeset_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	void *map;
	uint32_t fb;
};

struct modeset_dev {
	struct modeset_dev *next;

	uint32_t width;
	uint32_t height;

	unsigned int front_buf;
	struct modeset_buf bufs[2];

	drmModeModeInfo mode;
	uint32_t conn;
	uint32_t crtc;
	drmModeCrtc *saved_crtc;
};

static struct modeset_dev *modeset_list = NULL;

/*
 * So as next step we need to actually prepare all connectors that we find. We
 * do this in this little helper function:
 *
 * modeset_prepare(fd): This helper function takes the DRM fd as argument and
 * then simply retrieves the resource-info from the device. It then iterates
 * through all connectors and calls other helper functions to initialize this
 * connector (described later on).
 * If the initialization was successful, we simply add this object as new device
 * into the global modeset device list.
 *
 * The resource-structure contains a list of all connector-IDs. We use the
 * helper function drmModeGetConnector() to retrieve more information on each
 * connector. After we are done with it, we free it again with
 * drmModeFreeConnector().
 * Our helper modeset_setup_dev() returns -ENOENT if the connector is currently
 * unused and no monitor is plugged in. So we can ignore this connector.
 */

static int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	int i;
	struct modeset_dev *dev, *last_dev = NULL;
	int ret;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* ~iterate all connectors~ - Use first connector if present. It is
	   optimization related workaround since psplash supports drawing splash
	   screen on one scanout anyway. */
	for (i = 0; i < MIN(res->count_connectors, 1); ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;

		/* call helper function to prepare this connector */
		ret = modeset_setup_dev(fd, res, conn, dev);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n",
					i, res->connectors[i], errno);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		if (last_dev == NULL) {
			modeset_list = dev;
			last_dev = dev;
		} else {
			last_dev->next = dev;
			last_dev = dev;
		}
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

/*
 * Now we dig deeper into setting up a single connector. As described earlier,
 * we need to check several things first:
 *   * If the connector is currently unused, that is, no monitor is plugged in,
 *     then we can ignore it.
 *   * We have to find a suitable resolution and refresh-rate. All this is
 *     available in drmModeModeInfo structures saved for each crtc. We simply
 *     use the first mode that is available. This is always the mode with the
 *     highest resolution.
 *     A more sophisticated mode-selection should be done in real applications,
 *     though.
 *   * Then we need to find an CRTC that can drive this connector. A CRTC is an
 *     internal resource of each graphics-card. The number of CRTCs controls how
 *     many connectors can be controlled indepedently. That is, a graphics-cards
 *     may have more connectors than CRTCs, which means, not all monitors can be
 *     controlled independently.
 *     There is actually the possibility to control multiple connectors via a
 *     single CRTC if the monitors should display the same content. However, we
 *     do not make use of this here.
 *     So think of connectors as pipelines to the connected monitors and the
 *     CRTCs are the controllers that manage which data goes to which pipeline.
 *     If there are more pipelines than CRTCs, then we cannot control all of
 *     them at the same time.
 *   * We need to create a framebuffer for this connector. A framebuffer is a
 *     memory buffer that we can write XRGB32 data into. So we use this to
 *     render our graphics and then the CRTC can scan-out this data from the
 *     framebuffer onto the monitor.
 */

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	int ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		return -EFAULT;
	}

	/* copy the mode information into our device structure and into both
	 * buffers */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	dev->width = conn->modes[0].hdisplay;
	dev->height = conn->modes[0].vdisplay;
	dev->bufs[0].width = dev->width;
	dev->bufs[0].height = dev->height;
	dev->bufs[1].width = dev->width;
	dev->bufs[1].height = dev->height;
	fprintf(stderr, "mode for connector %u is %ux%u\n",
		conn->connector_id, dev->width, dev->height);

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, dev);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return ret;
	}

	/* create framebuffer #1 for this CRTC */
	ret = modeset_create_fb(fd, &dev->bufs[0]);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		return ret;
	}

	/* create framebuffer #2 for this CRTC */
	ret = modeset_create_fb(fd, &dev->bufs[1]);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		modeset_destroy_fb(fd, &dev->bufs[0]);
		return ret;
	}

	if (dev->bufs[0].size != dev->bufs[1].size) {
		fprintf(stderr, "front buffer size %" PRIu32 " does not match "
				"back buffer size %" PRIu32 "\n",
				dev->bufs[0].size, dev->bufs[1].size);
		return -1;
	}

	return 0;
}

/*
 * modeset_find_crtc(fd, res, conn, dev): This small helper tries to find a
 * suitable CRTC for the given connector. We have actually have to introduce one
 * more DRM object to make this more clear: Encoders.
 * Encoders help the CRTC to convert data from a framebuffer into the right
 * format that can be used for the chosen connector. We do not have to
 * understand any more of these conversions to make use of it. However, you must
 * know that each connector has a limited list of encoders that it can use. And
 * each encoder can only work with a limited list of CRTCs. So what we do is
 * trying each encoder that is available and looking for a CRTC that this
 * encoder can work with. If we find the first working combination, we are happy
 * and write it into the @dev structure.
 * But before iterating all available encoders, we first try the currently
 * active encoder+crtc on a connector to avoid a full modeset.
 *
 * However, before we can use a CRTC we must make sure that no other device,
 * that we setup previously, is already using this CRTC. Remember, we can only
 * drive one connector per CRTC! So we simply iterate through the "modeset_list"
 * of previously setup devices and check that this CRTC wasn't used before.
 * Otherwise, we continue with the next CRTC/Encoder combination.
 */

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	int i, j, crtc;
	struct modeset_dev *iter;

	/* first try the currently connected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == (uint32_t)crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == (uint32_t)crtc) {
					crtc = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

/*
 * modeset_create_fb(fd, dev): After we have found a crtc+connector+mode
 * combination, we need to actually create a suitable framebuffer that we can
 * use with it. There are actually two ways to do that:
 *   * We can create a so called "dumb buffer". This is a buffer that we can
 *     memory-map via mmap() and every driver supports this. We can use it for
 *     unaccelerated software rendering on the CPU.
 *   * We can use libgbm to create buffers available for hardware-acceleration.
 *     libgbm is an abstraction layer that creates these buffers for each
 *     available DRM driver. As there is no generic API for this, each driver
 *     provides its own way to create these buffers.
 *     We can then use such buffers to create OpenGL contexts with the mesa3D
 *     library.
 * We use the first solution here as it is much simpler and doesn't require any
 * external libraries. However, if you want to use hardware-acceleration via
 * OpenGL, it is actually pretty easy to create such buffers with libgbm and
 * libEGL. But this is beyond the scope of this document.
 *
 * So what we do is requesting a new dumb-buffer from the driver. We specify the
 * same size as the current mode that we selected for the connector.
 * Then we request the driver to prepare this buffer for memory mapping. After
 * that we perform the actual mmap() call. So we can now access the framebuffer
 * memory directly via the dev->map memory map.
 */

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB(fd, buf->width, buf->height, 24, 32, buf->stride,
			   buf->handle, &buf->fb);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

/*
 * modeset_destroy_fb() is a new function. It does exactly the reverse of
 * modeset_create_fb() and destroys a single framebuffer. The modeset.c example
 * used to do this directly in modeset_cleanup().
 * We simply unmap the buffer, remove the drm-FB and destroy the memory buffer.
 */

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	/* unmap buffer */
	munmap(buf->map, buf->size);

	/* delete framebuffer */
	drmModeRmFB(fd, buf->fb);

	/* delete dumb buffer */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static void psplash_drm_flip(PSplashCanvas *canvas, int sync)
{
	PSplashDRM *drm = canvas->priv;
	struct modeset_buf *buf;
	int ret;

	(void)sync;

	/* pick a back buffer */
	buf = &modeset_list->bufs[modeset_list->front_buf ^ 1];

	/* set back buffer as a front buffer */
	ret = drmModeSetCrtc(drm->fd, modeset_list->crtc, buf->fb, 0, 0,
			&modeset_list->conn, 1, &modeset_list->mode);
	if (ret) {
		fprintf(stderr, "cannot flip CRTC for connector %u (%d): %m\n",
			modeset_list->conn, errno);
		return;
	}

	/* update front buffer index */
	modeset_list->front_buf ^= 1;

	/* update back buffer pointer */
	drm->canvas.data = modeset_list->bufs[modeset_list->front_buf ^ 1].map;

	/* Sync new front to new back when requested */
	if (sync)
		memcpy(modeset_list->bufs[modeset_list->front_buf ^ 1].map,
			modeset_list->bufs[modeset_list->front_buf].map,
			modeset_list->bufs[0].size);
}

/*
 * Finally! We have a connector with a suitable CRTC. We know which mode we want
 * to use and we have a framebuffer of the correct size that we can write to.
 * There is nothing special left to do. We only have to program the CRTC to
 * connect each new framebuffer to each selected connector for each combination
 * that we saved in the global modeset_list.
 * This is done with a call to drmModeSetCrtc().
 *
 * So we are ready for our main() function. First we check whether the user
 * specified a DRM device on the command line, otherwise we use the default
 * /dev/dri/card0. Then we open the device via modeset_open(). modeset_prepare()
 * prepares all connectors and we can loop over "modeset_list" and call
 * drmModeSetCrtc() on every CRTC/connector combination.
 *
 * But printing empty black pages is boring so we have another helper function
 * modeset_draw() that draws some colors into the framebuffer for 5 seconds and
 * then returns. And then we have all the cleanup functions which correctly free
 * all devices again after we used them. All these functions are described below
 * the main() function.
 *
 * As a side note: drmModeSetCrtc() actually takes a list of connectors that we
 * want to control with this CRTC. We pass only one connector, though. As
 * explained earlier, if we used multiple connectors, then all connectors would
 * have the same controlling framebuffer so the output would be cloned. This is
 * most often not what you want so we avoid explaining this feature here.
 * Furthermore, all connectors will have to run with the same mode, which is
 * also often not guaranteed. So instead, we only use one connector per CRTC.
 *
 * Before calling drmModeSetCrtc() we also save the current CRTC configuration.
 * This is used in psplash_drm_destroy() to restore the CRTC to the same mode as
 * was before we changed it.
 * If we don't do this, the screen will stay blank after we exit until another
 * application performs modesetting itself.
 */

PSplashDRM* psplash_drm_new(int angle, int dev_id)
{
	PSplashDRM *drm = NULL;
	int ret;
	char card[] = "/dev/dri/card0";
	struct modeset_dev *iter;
	struct modeset_buf *buf;

	if ((drm = malloc(sizeof(*drm))) == NULL) {
		perror("malloc");
		goto error;
	}
	drm->fd = 0;
	drm->canvas.priv = drm;
	drm->canvas.flip = psplash_drm_flip;

	if (dev_id > 0 && dev_id < 10) {
		// Conversion from integer to ascii.
		card[13] = dev_id + 48;
	}

#ifdef ENABLE_DRM_LEASE
	if (drm_lease_name) {
		fprintf(stderr, "using drm lease '%s'\n", drm_lease_name);
		struct dlm_lease *lease = dlm_get_lease(drm_lease_name);
		if (lease) {
			drm->fd = dlm_lease_fd(lease);
			if (!drm->fd)
				dlm_release_lease(lease);
		}
		if (!drm->fd) {
			fprintf(stderr, "Could not get DRM lease %s\n", drm_lease_name);
			goto error;
		}
	}
#endif

	if (!drm->fd) {
		fprintf(stderr, "using card '%s'\n", card);

		/* open the DRM device */
		ret = modeset_open(&drm->fd, card);
		if (ret)
			goto error;
	}

	/* prepare all connectors and CRTCs */
	ret = modeset_prepare(drm->fd);
	if (ret)
		goto error;

	/* perform actual modesetting on each found connector+CRTC */
	for (iter = modeset_list; iter; iter = iter->next) {
		iter->saved_crtc = drmModeGetCrtc(drm->fd, iter->crtc);
		buf = &iter->bufs[iter->front_buf];
		ret = drmModeSetCrtc(drm->fd, iter->crtc, buf->fb, 0, 0,
				     &iter->conn, 1, &iter->mode);
		if (ret)
			fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
				iter->conn, errno);
	}

	drm->canvas.data = modeset_list->bufs[modeset_list->front_buf ^ 1].map;
	drm->canvas.width = modeset_list->width;
	drm->canvas.height = modeset_list->height;
	drm->canvas.bpp = 32;

	if (modeset_list->bufs[0].stride != modeset_list->bufs[1].stride) {
		fprintf(stderr, "front buffer stride %" PRIu32 " does not match"
				" back buffer stride %" PRIu32 "\n",
				modeset_list->bufs[0].stride,
				modeset_list->bufs[1].stride);
		goto error;
	}
	drm->canvas.stride = modeset_list->bufs[0].stride;

	drm->canvas.angle = angle;
	drm->canvas.rgbmode = RGB888;

	/*
	 * There seems some difference about handling portrait angle between
	 * pure drm vs drm-lease. We'd use a method as same with psplash-fb
	 * for drm-lease devices.
	 */
	if (drm_lease_name) {
		switch (angle) {
			case 270:
			case 90:
				drm->canvas.width  = modeset_list->height;
				drm->canvas.height = modeset_list->width;
				break;
			default:
				break;
		}
	}

	return drm;
error:
	psplash_drm_destroy(drm);
	return NULL;
}

/*
 * psplash_drm_destroy(drm): This cleans up all the devices we created during
 * modeset_prepare(). It resets the CRTCs to their saved states and deallocates
 * all memory.
 * It should be pretty obvious how all of this works.
 */

void psplash_drm_destroy(PSplashDRM *drm)
{
	struct modeset_dev *iter;

	if (!drm)
		return;

	while (modeset_list) {
		/* remove from global list */
		iter = modeset_list;
		modeset_list = iter->next;

		/* restore saved CRTC configuration */
		drmModeSetCrtc(drm->fd,
			       iter->saved_crtc->crtc_id,
			       iter->saved_crtc->buffer_id,
			       iter->saved_crtc->x,
			       iter->saved_crtc->y,
			       &iter->conn,
			       1,
			       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		/* destroy framebuffers */
		modeset_destroy_fb(drm->fd, &iter->bufs[1]);
		modeset_destroy_fb(drm->fd, &iter->bufs[0]);

		/* free allocated memory */
		free(iter);
	}

	close(drm->fd);
	free(drm);
}

#ifdef ENABLE_DRM_LEASE
void drm_set_lease_name (char *name) {
	if (!name)
		return;
	drm_lease_name = strdup(name);
}
#endif

/*
 * I hope this was a short but easy overview of the DRM modesetting API. The DRM
 * API offers much more capabilities including:
 *  - double-buffering or tripple-buffering (or whatever you want)
 *  - vsync'ed page-flips
 *  - hardware-accelerated rendering (for example via OpenGL)
 *  - output cloning
 *  - graphics-clients plus authentication
 *  - DRM planes/overlays/sprites
 *  - ...
 * If you are interested in these topics, I can currently only redirect you to
 * existing implementations, including:
 *  - plymouth (which uses dumb-buffers like this example; very easy to understand)
 *  - kmscon (which uses libuterm to do this)
 *  - wayland (very sophisticated DRM renderer; hard to understand fully as it
 *             uses more complicated techniques like DRM planes)
 *  - xserver (very hard to understand as it is split across many files/projects)
 *
 * But understanding how modesetting (as described in this document) works, is
 * essential to understand all further DRM topics.
 *
 * Any feedback is welcome. Feel free to use this code freely for your own
 * documentation or projects.
 *
 *  - Hosted on http://github.com/dvdhrm/docs
 *  - Written by David Rheinsberg <david.rheinsberg@gmail.com>
 */
