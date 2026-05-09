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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
/*
 * CancelIoEx() requires Vista+; MinGW headers gate it on _WIN32_WINNT >= 0x0600.
 * Windows 7 is the de-facto floor for this project so this is safe.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
/*
 * <initguid.h> must precede any header that declares the GUIDs we reference,
 * so that GUID_DEVCLASS_PORTS gets a real definition in this TU instead of an
 * extern reference (MinGW does not ship a uuid.lib that would resolve it).
 */
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <ctype.h>
#endif

#include "qdl.h"
#include "oscompat.h"

#define QCOM_USB_VID	"05c6"

struct qdl_device_chardev {
	struct qdl_device base;
#ifdef _WIN32
	void *handle;	/* HANDLE; void* avoids leaking <windows.h> to consumers */
#else
	int fd;
#endif
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

#endif /* __linux__ */

#ifdef _WIN32

#define QCOM_HWID_PREFIX	"USB\\VID_05C6&PID_"

struct chardev_match {
	char serial[64];
	char path[64];	/* "\\\\.\\COM999" fits */
};

/*
 * Pull the iSerial out of the SetupAPI device-instance ID. The instance ID for
 * a USB device looks like "USB\\VID_05C6&PID_9008\\<SERIAL>"; the trailing
 * component after the last backslash is the iSerial as Windows stored it.
 */
static void win_extract_serial(const char *instance_id, char *out, size_t out_size)
{
	const char *last;

	out[0] = '\0';
	last = strrchr(instance_id, '\\');
	if (!last)
		return;
	last++;
	snprintf(out, out_size, "%s", last);
}

/*
 * Read PortName ("COM5", "COM12", ...) from the device's "Device Parameters"
 * registry subkey.
 */
static bool win_read_port_name(HDEVINFO devinfo, SP_DEVINFO_DATA *info,
			       char *out, size_t out_size)
{
	DWORD type = 0;
	DWORD size = 0;
	HKEY key;
	LSTATUS s;

	key = SetupDiOpenDevRegKey(devinfo, info, DICS_FLAG_GLOBAL, 0,
				   DIREG_DEV, KEY_QUERY_VALUE);
	if (key == INVALID_HANDLE_VALUE)
		return false;

	size = (DWORD)out_size;
	s = RegQueryValueExA(key, "PortName", NULL, &type,
			     (LPBYTE)out, &size);
	RegCloseKey(key);

	if (s != ERROR_SUCCESS || type != REG_SZ)
		return false;

	if (size > 0 && out[size - 1] != '\0' && size < out_size)
		out[size] = '\0';
	return true;
}

static int win_enumerate_qcom(struct chardev_match *out, size_t out_max)
{
	HDEVINFO devinfo;
	SP_DEVINFO_DATA info = { .cbSize = sizeof(info) };
	char hwid[256];
	char instance_id[256];
	char port_name[32];
	size_t count = 0;
	DWORD i;

	devinfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL,
				       DIGCF_PRESENT);
	if (devinfo == INVALID_HANDLE_VALUE)
		return -1;

	for (i = 0; SetupDiEnumDeviceInfo(devinfo, i, &info); i++) {
		DWORD type = 0;
		DWORD size = 0;

		if (!SetupDiGetDeviceRegistryPropertyA(devinfo, &info,
						       SPDRP_HARDWAREID, &type,
						       (BYTE *)hwid, sizeof(hwid),
						       &size))
			continue;

		/* HARDWAREID is a REG_MULTI_SZ; first entry is what we want. */
		if (strncmp(hwid, QCOM_HWID_PREFIX,
			    sizeof(QCOM_HWID_PREFIX) - 1) != 0)
			continue;

		if (!SetupDiGetDeviceInstanceIdA(devinfo, &info, instance_id,
						 sizeof(instance_id), NULL))
			continue;

		if (!win_read_port_name(devinfo, &info, port_name,
					sizeof(port_name)))
			continue;

		if (count < out_max) {
			win_extract_serial(instance_id, out[count].serial,
					   sizeof(out[count].serial));
			snprintf(out[count].path, sizeof(out[count].path),
				 "\\\\.\\%s", port_name);
		}
		count++;
	}

	SetupDiDestroyDeviceInfoList(devinfo);
	return (int)count;
}

static int win_configure_handle(HANDLE h)
{
	COMMTIMEOUTS to = { 0 };
	DCB dcb = { .DCBlength = sizeof(dcb) };

	/*
	 * The QDLoader 9008 driver presents the device as a serial port, but
	 * line-discipline parameters (baud, parity, stop bits) are irrelevant
	 * over USB-CDC. Some driver builds nevertheless reject I/O until DCB
	 * has been initialised, so we set sane defaults and never touch them
	 * again.
	 */
	if (!GetCommState(h, &dcb))
		return -1;
	dcb.BaudRate = 115200;
	dcb.ByteSize = 8;
	dcb.Parity   = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary  = TRUE;
	dcb.fNull    = FALSE;
	dcb.fAbortOnError = FALSE;
	if (!SetCommState(h, &dcb))
		return -1;

	/*
	 * Make every read/write fully overlapped: Read/WriteFile must return
	 * ERROR_IO_PENDING immediately and we drive the timeout via
	 * WaitForSingleObject() ourselves. Setting MAXDWORD ReadIntervalTimeout
	 * with zeros for the rest is the documented "non-blocking" preset.
	 */
	to.ReadIntervalTimeout = MAXDWORD;
	to.ReadTotalTimeoutMultiplier = 0;
	to.ReadTotalTimeoutConstant = 0;
	to.WriteTotalTimeoutMultiplier = 0;
	to.WriteTotalTimeoutConstant = 0;
	if (!SetCommTimeouts(h, &to))
		return -1;

	SetupComm(h, 64 * 1024, 64 * 1024);
	PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
	return 0;
}

static int chardev_open(struct qdl_device *qdl, const char *serial)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);
	struct chardev_match matches[16];
	const char *path = NULL;
	HANDLE h;
	int found;
	int i;

	/* Allow an explicit \\.\COMx (or COMx) path via --serial. */
	if (serial && (serial[0] == '\\' ||
		       (strlen(serial) >= 3 &&
			(serial[0] == 'C' || serial[0] == 'c') &&
			(serial[1] == 'O' || serial[1] == 'o') &&
			(serial[2] == 'M' || serial[2] == 'm')))) {
		path = serial;
	} else {
		found = win_enumerate_qcom(matches,
					   sizeof(matches) / sizeof(matches[0]));
		if (found < 0) {
			ux_err("chardev: SetupDiGetClassDevs failed (error %lu)\n",
			       (unsigned long)GetLastError());
			return -1;
		}
		if (found == 0) {
			ux_err("chardev: no Qualcomm COM ports found\n");
			return -1;
		}

		for (i = 0; i < found && i < (int)(sizeof(matches) / sizeof(matches[0])); i++) {
			if (!serial || serial[0] == '\0' ||
			    _stricmp(matches[i].serial, serial) == 0) {
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

	h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		ux_err("chardev: CreateFile(%s) failed (error %lu)\n",
		       path, (unsigned long)GetLastError());
		return -1;
	}

	if (win_configure_handle(h) < 0) {
		ux_err("chardev: failed to configure %s (error %lu)\n",
		       path, (unsigned long)GetLastError());
		CloseHandle(h);
		return -1;
	}

	qd->handle = h;
	snprintf(qd->path, sizeof(qd->path), "%s", path);

	ux_debug("chardev: opened %s\n", path);
	return 0;
}

static int win_overlapped_io(HANDLE h, void *buf, size_t len,
			     unsigned int timeout_ms, bool is_write)
{
	OVERLAPPED ovl = { 0 };
	HANDLE ev;
	DWORD wait;
	DWORD done = 0;
	BOOL ok;

	ev = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ev)
		return -EIO;
	ovl.hEvent = ev;

	if (is_write)
		ok = WriteFile(h, buf, (DWORD)len, NULL, &ovl);
	else
		ok = ReadFile(h, buf, (DWORD)len, NULL, &ovl);

	if (!ok && GetLastError() != ERROR_IO_PENDING) {
		CloseHandle(ev);
		return -EIO;
	}

	wait = WaitForSingleObject(ev, timeout_ms);
	if (wait == WAIT_TIMEOUT) {
		CancelIoEx(h, &ovl);
		/* Drain the cancellation so the OVERLAPPED isn't reused live. */
		GetOverlappedResult(h, &ovl, &done, TRUE);
		CloseHandle(ev);
		return -ETIMEDOUT;
	}
	if (wait != WAIT_OBJECT_0) {
		CloseHandle(ev);
		return -EIO;
	}

	if (!GetOverlappedResult(h, &ovl, &done, FALSE)) {
		CloseHandle(ev);
		return -EIO;
	}

	CloseHandle(ev);
	return (int)done;
}

static int chardev_read(struct qdl_device *qdl, void *buf, size_t len,
			unsigned int timeout)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);

	return win_overlapped_io((HANDLE)qd->handle, buf, len, timeout, false);
}

static int chardev_write(struct qdl_device *qdl, const void *buf, size_t len,
			 unsigned int timeout)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);

	return win_overlapped_io((HANDLE)qd->handle, (void *)buf, len, timeout,
				 true);
}

static void chardev_close(struct qdl_device *qdl)
{
	struct qdl_device_chardev *qd = container_of(qdl,
						     struct qdl_device_chardev,
						     base);
	HANDLE h = (HANDLE)qd->handle;

	if (h && h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
		qd->handle = NULL;
	}
}

static void chardev_set_out_chunk_size(struct qdl_device *qdl __unused,
				       long size __unused)
{
	/* The kernel-mode driver handles bulk-transfer chunking. */
}

#endif /* _WIN32 */

#if defined(__linux__) || defined(_WIN32)

struct qdl_device *chardev_init(void)
{
	struct qdl_device_chardev *qd = calloc(1, sizeof(*qd));

	if (!qd)
		return NULL;

#ifdef _WIN32
	qd->handle = NULL;
#else
	qd->fd = -1;
#endif
	qd->base.dev_type = QDL_DEVICE_CHARDEV;
	qd->base.open = chardev_open;
	qd->base.read = chardev_read;
	qd->base.write = chardev_write;
	qd->base.close = chardev_close;
	qd->base.set_out_chunk_size = chardev_set_out_chunk_size;
	qd->base.max_payload_size = 1048576;

	return &qd->base;
}

#else /* unsupported platform */

struct qdl_device *chardev_init(void)
{
	ux_err("chardev backend is not supported on this platform\n");
	return NULL;
}

#endif
