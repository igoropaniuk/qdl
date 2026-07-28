/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __FLASHMAP_H__
#define __FLASHMAP_H__

#include <stdbool.h>
#include "list.h"

struct sahara_image;

int flashmap_load(struct list_head *ops, const char *filename, char *specifier,
		  struct sahara_image *images, const char *incdir);

/*
 * Implemented in zipper.c, which is only built with zip-container
 * support. The stub keeps callers linkable; they are expected to check
 * qdl_zip_supported() and report the missing feature themselves.
 */
#ifdef HAVE_ZIP_CONTAINER
int zipper_write(const char *filename, struct list_head *ops, struct sahara_image *images);
#else
static inline int zipper_write(const char *filename, struct list_head *ops,
			       struct sahara_image *images)
{
	(void)filename;
	(void)ops;
	(void)images;

	return -1;
}
#endif

#endif
