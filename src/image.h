/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __IMAGE_H__
#define __IMAGE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "list.h"

struct qdl_image;

int image_parse_sector(const char *expr, bool *from_end, uint64_t *value);
bool image_same_file(const char *a, const char *b);
int image_plan(struct list_head *ops, const char *path,
	       unsigned int default_sector_size,
	       unsigned int *sector_size, uint64_t *min_size);

struct qdl_image *image_create(const char *path, uint64_t size);
int image_close(struct qdl_image *image, bool keep);
const char *image_path(struct qdl_image *image);
uint64_t image_size(struct qdl_image *image);

int image_sector_offset(struct qdl_image *image, const char *expr,
			unsigned int sector_size, uint64_t *offset);
int image_write(struct qdl_image *image, uint64_t offset, const void *buf, size_t len);
int image_read(struct qdl_image *image, uint64_t offset, void *buf, size_t len);
int image_zero(struct qdl_image *image, uint64_t offset, uint64_t len);
int image_patch(struct qdl_image *image, unsigned int sector_size,
		const char *start_sector, unsigned int byte_offset,
		unsigned int size_in_bytes, const char *value);

#endif
