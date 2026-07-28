/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __QDL_FILE_H__
#define __QDL_FILE_H__

#include <stdbool.h>
#include <sys/types.h>

struct zip_file;

enum qdl_file_type {
	QDL_FILE_TYPE_UNKNOWN,
	QDL_FILE_TYPE_POSIX,
	QDL_FILE_TYPE_ZIP,
};

struct qdl_file {
	enum qdl_file_type type;

	size_t size;

	int fd;
	struct zip_file *zip_file;
};

struct qdl_zip;

int qdl_file_open(struct qdl_zip *qzip, const char *filename, struct qdl_file *file);
void *qdl_file_load(struct qdl_file *file, size_t *len);
void qdl_file_close(struct qdl_file *file);
size_t qdl_file_getsize(struct qdl_file *file);
ssize_t qdl_file_read(struct qdl_file *file, void *buf, size_t len);
ssize_t qdl_file_read_exact(struct qdl_file *file, void *buf, size_t len);
off_t qdl_file_seek(struct qdl_file *file, off_t offset, int whence);

int qdl_zip_open(const char *filename, struct qdl_zip **__qdl_zip);
struct qdl_zip *qdl_zip_get(struct qdl_zip *qzip);
void qdl_zip_put(struct qdl_zip *qzip);

/*
 * Internal libzip-backed helpers, implemented in file_zip.c. Builds
 * without zip-container support get inert stubs; they are unreachable
 * there, as qdl_zip_open() then never produces a zip handle and no
 * qdl_file ever becomes QDL_FILE_TYPE_ZIP.
 */
#ifdef HAVE_ZIP_CONTAINER
static inline bool qdl_zip_supported(void)
{
	return true;
}

int qdl_zip_file_open(struct qdl_zip *qzip, const char *filename, struct qdl_file *file);
ssize_t qdl_zip_file_read(struct qdl_file *file, void *buf, size_t len);
void qdl_zip_file_close(struct qdl_file *file);
#else
static inline bool qdl_zip_supported(void)
{
	return false;
}

static inline int qdl_zip_file_open(struct qdl_zip *qzip, const char *filename,
				    struct qdl_file *file)
{
	(void)qzip;
	(void)filename;
	(void)file;

	return -1;
}

static inline ssize_t qdl_zip_file_read(struct qdl_file *file, void *buf, size_t len)
{
	(void)file;
	(void)buf;
	(void)len;

	return -1;
}

static inline void qdl_zip_file_close(struct qdl_file *file)
{
	(void)file;
}
#endif
#endif
