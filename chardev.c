// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2026, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Backend that talks to a Qualcomm EDL device through a character-device
 * interface exposed by an in-kernel driver, rather than via libusb. This
 * covers usb-serial bindings used by the qcom-usb-kernel-drivers project on
 * Linux (where the device shows up as /dev/ttyUSB*) as well as the official
 * Qualcomm QDLoader 9008 driver on Windows (where the device shows up as
 * \\.\COMx). The Sahara/Firehose framing is identical across both transports;
 * this file only deals with how user space reaches the underlying bulk pipes.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  /* cfmakeraw() */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#endif

#include "qdl.h"
#include "oscompat.h"

#define QCOM_USB_VID	"05c6"

struct qdl_device_chardev {
	struct qdl_device base;
	int fd;
	char path[PATH_MAX];
};

#ifdef __linux__

#define MAX_TTY_MATCHES	16

struct chardev_match {
	char serial[64];
	char path[PATH_MAX];
};

static bool sysfs_read_attr(const char *base, const char *attr,
			    char *out, size_t out_size)
{
	char path[PATH_MAX];
	char buf[256];
	ssize_t n;
	int fd;
	int ret;

	ret = snprintf(path, sizeof(path), "%s/%s", base, attr);
	if (ret < 0 || (size_t)ret >= sizeof(path))
		return false;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;

	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	if (strlen(buf) >= out_size)
		return false;

	strcpy(out, buf);
	return true;
}

/*
 * Walk up from /sys/class/tty/ttyUSB%d/device until we hit a directory that
 * contains an idVendor file: that's the parent USB device. Returns the
 * absolute sysfs path of that USB device in @out.
 */
static bool sysfs_resolve_usb_parent(const char *tty_name,
				     char *out, size_t out_size)
{
	char link[PATH_MAX];
	char real[PATH_MAX];
	char probe[PATH_MAX];
	char *slash;
	int ret;

	ret = snprintf(link, sizeof(link), "/sys/class/tty/%s/device", tty_name);
	if (ret < 0 || (size_t)ret >= sizeof(link))
		return false;

	if (!realpath(link, real))
		return false;

	while ((slash = strrchr(real, '/')) != NULL) {
		ret = snprintf(probe, sizeof(probe), "%s/idVendor", real);
		if (ret < 0 || (size_t)ret >= sizeof(probe))
			return false;
		if (access(probe, F_OK) == 0) {
			if (strlen(real) >= out_size)
				return false;
			strcpy(out, real);
			return true;
		}
		*slash = '\0';
		if (real[0] == '\0')
			return false;
	}

	return false;
}

static int chardev_enumerate_qcom(struct chardev_match *out, size_t out_max)
{
	char usb_path[PATH_MAX];
	char vid[16];
	char serial[64];
	struct dirent *de;
	size_t count = 0;
	DIR *d;

	d = opendir("/sys/class/tty");
	if (!d)
		return -errno;

	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "ttyUSB", 6) != 0)
			continue;

		if (!sysfs_resolve_usb_parent(de->d_name, usb_path, sizeof(usb_path)))
			continue;

		if (!sysfs_read_attr(usb_path, "idVendor", vid, sizeof(vid)))
			continue;
		if (strcmp(vid, QCOM_USB_VID) != 0)
			continue;

		if (!sysfs_read_attr(usb_path, "serial", serial, sizeof(serial)))
			serial[0] = '\0';

		if (count < out_max) {
			snprintf(out[count].serial, sizeof(out[count].serial),
				 "%s", serial);
			snprintf(out[count].path, sizeof(out[count].path),
				 "/dev/%s", de->d_name);
		}
		count++;
	}

	closedir(d);
	return (int)count;
}

static int chardev_configure_tty(int fd)
{
	struct termios tio;

	if (tcgetattr(fd, &tio) < 0)
		return -errno;

	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) < 0)
		return -errno;

	return 0;
}

static int chardev_open(struct qdl_device *qdl, const char *serial)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);
	struct chardev_match matches[MAX_TTY_MATCHES];
	const char *path = NULL;
	int found;
	int fd;
	int ret;
	int i;

	/*
	 * If --serial was given an absolute device path, use it verbatim. This
	 * lets users point qdl at a non-Qualcomm-VID chardev that surfaces the
	 * same protocol (test rigs, sims, etc.) without going through sysfs.
	 */
	if (serial && serial[0] == '/') {
		path = serial;
	} else {
		found = chardev_enumerate_qcom(matches, MAX_TTY_MATCHES);
		if (found < 0) {
			ux_err("chardev: failed to enumerate /sys/class/tty: %s\n",
			       strerror(-found));
			return -1;
		}
		if (found == 0) {
			ux_err("chardev: no Qualcomm tty devices found\n");
			return -1;
		}

		for (i = 0; i < found && i < MAX_TTY_MATCHES; i++) {
			if (!serial || serial[0] == '\0' ||
			    strcmp(matches[i].serial, serial) == 0) {
				path = matches[i].path;
				break;
			}
		}

		if (!path) {
			ux_err("chardev: no Qualcomm device matching serial \"%s\"\n",
			       serial);
			return -1;
		}
	}

	fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		ux_err("chardev: open(%s): %s\n", path, strerror(errno));
		return -1;
	}

	ret = chardev_configure_tty(fd);
	if (ret < 0) {
		ux_err("chardev: tcsetattr(%s): %s\n", path, strerror(-ret));
		close(fd);
		return -1;
	}

	/* Discard anything the kernel buffered before we attached. */
	tcflush(fd, TCIOFLUSH);

	qd->fd = fd;
	snprintf(qd->path, sizeof(qd->path), "%s", path);

	ux_debug("chardev: opened %s\n", path);
	return 0;
}

static int chardev_read(struct qdl_device *qdl, void *buf, size_t len,
			unsigned int timeout)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);
	struct pollfd pfd = { .fd = qd->fd, .events = POLLIN };
	ssize_t n;
	int ret;

	do {
		ret = poll(&pfd, 1, (int)timeout);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		return -errno;
	if (ret == 0)
		return -ETIMEDOUT;
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
		return -EIO;

	do {
		n = read(qd->fd, buf, len);
	} while (n < 0 && errno == EINTR);

	if (n < 0)
		return -errno;
	return (int)n;
}

static int chardev_write(struct qdl_device *qdl, const void *buf, size_t len,
			 unsigned int timeout)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);
	const unsigned char *p = buf;
	size_t total = 0;
	ssize_t n;

	/*
	 * write(2) on a usb-serial chardev has no per-call timeout: the kernel
	 * either submits the URB and completes, or fails when the device is
	 * gone. Keep the @timeout argument for API symmetry with usb_write().
	 */
	(void)timeout;

	while (total < len) {
		n = write(qd->fd, p + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		total += (size_t)n;
	}

	return (int)total;
}

static void chardev_close(struct qdl_device *qdl)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);

	if (qd->fd >= 0) {
		close(qd->fd);
		qd->fd = -1;
	}
}

static void chardev_set_out_chunk_size(struct qdl_device *qdl __unused,
				       long size __unused)
{
	/* The kernel handles bulk-transfer chunking transparently. */
}

struct qdl_device *chardev_init(void)
{
	struct qdl_device_chardev *qd = calloc(1, sizeof(*qd));

	if (!qd)
		return NULL;

	qd->fd = -1;
	qd->base.dev_type = QDL_DEVICE_CHARDEV;
	qd->base.open = chardev_open;
	qd->base.read = chardev_read;
	qd->base.write = chardev_write;
	qd->base.close = chardev_close;
	qd->base.set_out_chunk_size = chardev_set_out_chunk_size;
	qd->base.max_payload_size = 1048576;

	return &qd->base;
}

#else /* !__linux__ */

struct qdl_device *chardev_init(void)
{
	ux_err("chardev backend is not yet supported on this platform\n");
	return NULL;
}

#endif
