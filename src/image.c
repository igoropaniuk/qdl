// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
/*
 * Disk image backing for the dry-run simulator.
 *
 * With --disk-image the simulated device stores what it is told to program
 * in a file, producing an image of the flashed storage that can be written
 * to an SD card or eMMC as is. The image models one flat block device, so
 * only physical partition 0 can be addressed.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qdl.h"
#include "file.h"
#include "firehose.h"
#include "image.h"
#include "oscompat.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

#define IMAGE_NUM_DISK_SECTORS	"NUM_DISK_SECTORS"
#define IMAGE_CRC32_PREFIX	"CRC32("
#define IMAGE_IO_CHUNK		(1024 * 1024)
#define IMAGE_TMP_SUFFIX	".XXXXXX"

struct qdl_image {
	char *path;
	char *tmp_path;
	uint64_t size;
	int fd;
};

static int image_parse_number(const char *str, const char **end, uint64_t *value)
{
	char *tmp;

	if (!isdigit((unsigned char)str[0]))
		return -1;

	errno = 0;
	*value = strtoull(str, &tmp, 10);
	if (errno)
		return -1;

	*end = tmp;

	return 0;
}

/*
 * Parse the leading sector expression of @expr, leaving @end at the first
 * character past it. The trailing dot is how the XML files mark decimal
 * numbers for Trace32, it carries no meaning here.
 */
static int image_parse_sector_prefix(const char *expr, const char **end,
				     bool *from_end, uint64_t *value)
{
	size_t len = strlen(IMAGE_NUM_DISK_SECTORS);
	const char *ptr = expr;

	*from_end = false;
	*value = 0;

	if (!strncmp(ptr, IMAGE_NUM_DISK_SECTORS, len)) {
		*from_end = true;
		ptr += len;

		if (ptr[0] == '-' && image_parse_number(ptr + 1, &ptr, value) < 0)
			return -1;
	} else if (image_parse_number(ptr, &ptr, value) < 0) {
		return -1;
	}

	if (ptr[0] == '.')
		ptr++;

	*end = ptr;

	return 0;
}

/**
 * image_parse_sector() - parse a sector expression
 * @expr: "<N>", "NUM_DISK_SECTORS" or "NUM_DISK_SECTORS-<N>", each
 *	  optionally followed by a dot
 * @from_end: set when the sector is counted back from the end of the disk
 * @value: the parsed <N>, zero when absent
 *
 * Returns: 0 on success, -1 if @expr is malformed.
 */
int image_parse_sector(const char *expr, bool *from_end, uint64_t *value)
{
	const char *end;

	if (image_parse_sector_prefix(expr, &end, from_end, value) < 0)
		return -1;

	return end[0] ? -1 : 0;
}

/*
 * Turn a parsed sector expression into a number. Only an expression counted
 * from the end of the disk depends on, and is limited by, its size: a plain
 * number is not necessarily a sector, <patch> stores those as they are.
 */
static int image_resolve_number(struct qdl_image *image, bool from_end, uint64_t value,
				unsigned int sector_size, uint64_t *number)
{
	uint64_t num_sectors = image->size / sector_size;

	if (!from_end) {
		*number = value;
		return 0;
	}

	if (value > num_sectors)
		return -1;

	*number = num_sectors - value;

	return 0;
}

/* Resolve a parsed sector expression to a byte offset within the image */
static int image_resolve_offset(struct qdl_image *image, bool from_end, uint64_t value,
				unsigned int sector_size, uint64_t *offset)
{
	uint64_t sector;

	if (image_resolve_number(image, from_end, value, sector_size, &sector) < 0)
		return -1;

	if (sector > image->size / sector_size)
		return -1;

	*offset = sector * sector_size;

	return 0;
}

/**
 * image_sector_offset() - resolve a sector expression to a byte offset
 * @image: disk image
 * @expr: sector expression, see image_parse_sector()
 * @sector_size: sector size the expression is counted in
 * @offset: resulting byte offset into the image
 *
 * Returns: 0 on success, -1 with the error reported on failure.
 */
int image_sector_offset(struct qdl_image *image, const char *expr,
			unsigned int sector_size, uint64_t *offset)
{
	uint64_t value;
	bool from_end;

	if (!sector_size) {
		ux_err("disk image accessed without a sector size\n");
		return -1;
	}

	if (image_parse_sector(expr, &from_end, &value) < 0) {
		ux_err("unsupported sector expression \"%s\"\n", expr);
		return -1;
	}

	if (image_resolve_offset(image, from_end, value, sector_size, offset) < 0) {
		ux_err("sector \"%s\" is beyond the end of the disk image\n", expr);
		return -1;
	}

	return 0;
}

static int image_seek(struct qdl_image *image, uint64_t offset, uint64_t len)
{
	if (offset > image->size || len > image->size - offset) {
		ux_err("access beyond the end of the disk image (offset %" PRIu64 ", %" PRIu64 " bytes)\n",
		       offset, len);
		return -1;
	}

	if (lseek(image->fd, (off_t)offset, SEEK_SET) < 0) {
		ux_err("failed to seek in %s: %s\n", image->tmp_path, strerror(errno));
		return -1;
	}

	return 0;
}

int image_write(struct qdl_image *image, uint64_t offset, const void *buf, size_t len)
{
	const char *ptr = buf;
	ssize_t n;

	if (image_seek(image, offset, len) < 0)
		return -1;

	while (len > 0) {
		n = write(image->fd, ptr, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ux_err("failed to write %s: %s\n", image->tmp_path, strerror(errno));
			return -1;
		}

		ptr += n;
		len -= n;
	}

	return 0;
}

int image_read(struct qdl_image *image, uint64_t offset, void *buf, size_t len)
{
	char *ptr = buf;
	ssize_t n;

	if (image_seek(image, offset, len) < 0)
		return -1;

	while (len > 0) {
		n = read(image->fd, ptr, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			ux_err("failed to read %s: %s\n", image->tmp_path, strerror(errno));
			return -1;
		}
		if (n == 0) {
			ux_err("unexpected end of %s\n", image->tmp_path);
			return -1;
		}

		ptr += n;
		len -= n;
	}

	return 0;
}

static int image_resize(struct qdl_image *image, uint64_t size)
{
	if (ftruncate(image->fd, (off_t)size) < 0) {
		ux_err("failed to resize %s to %" PRIu64 " bytes: %s\n",
		       image->tmp_path, size, strerror(errno));
		return -1;
	}

	return 0;
}

/**
 * image_zero() - zero a byte range of the image
 * @image: disk image
 * @offset: start of the range
 * @len: length of the range
 *
 * Only chunks holding data are rewritten, so that erasing a region that was
 * never programmed does not fill the holes of a sparse image file.
 *
 * Returns: 0 on success, -1 with the error reported on failure.
 */
int image_zero(struct qdl_image *image, uint64_t offset, uint64_t len)
{
	size_t chunk;
	char *zeros;
	char *buf;
	int ret = -1;

	if (image_seek(image, offset, len) < 0)
		return -1;

	/* Dropping all the data is cheaper than looking for it */
	if (offset == 0 && len == image->size) {
		if (image_resize(image, 0) < 0)
			return -1;
		return image_resize(image, image->size);
	}

	buf = calloc(1, IMAGE_IO_CHUNK);
	zeros = calloc(1, IMAGE_IO_CHUNK);
	if (!buf || !zeros) {
		ux_err("failed to allocate disk image buffer\n");
		goto out;
	}

	while (len > 0) {
		chunk = len < IMAGE_IO_CHUNK ? (size_t)len : IMAGE_IO_CHUNK;

		if (image_read(image, offset, buf, chunk) < 0)
			goto out;

		if (memcmp(buf, zeros, chunk) &&
		    image_write(image, offset, zeros, chunk) < 0)
			goto out;

		offset += chunk;
		len -= chunk;
	}

	ret = 0;
out:
	free(zeros);
	free(buf);

	return ret;
}

/* CRC-32 as used by GPT (IEEE 802.3, reflected), fed with a running value */
static uint32_t image_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
	int bit;

	crc = ~crc;
	while (len--) {
		crc ^= *buf++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
	}

	return ~crc;
}

/* Evaluate "CRC32(<sector>,<length in bytes>)" against the image content */
static int image_eval_crc32(struct qdl_image *image, const char *args,
			    unsigned int sector_size, uint64_t *result)
{
	uint8_t buf[4096];
	uint32_t crc = 0;
	uint64_t offset;
	uint64_t value;
	uint64_t len;
	bool from_end;
	size_t chunk;
	const char *ptr;

	if (image_parse_sector_prefix(args, &ptr, &from_end, &value) < 0 || ptr[0] != ',')
		return -1;

	if (image_parse_number(ptr + 1, &ptr, &len) < 0)
		return -1;

	if (ptr[0] == '.')
		ptr++;
	if (strcmp(ptr, ")"))
		return -1;

	if (image_resolve_offset(image, from_end, value, sector_size, &offset) < 0)
		return -1;

	while (len > 0) {
		chunk = len < sizeof(buf) ? (size_t)len : sizeof(buf);

		if (image_read(image, offset, buf, chunk) < 0)
			return -1;

		crc = image_crc32(crc, buf, chunk);
		offset += chunk;
		len -= chunk;
	}

	*result = crc;

	return 0;
}

/**
 * image_patch() - apply a <patch> to the image
 * @image: disk image
 * @sector_size: sector size the patch is expressed in
 * @start_sector: expression for the sector to patch
 * @byte_offset: offset of the patched field within that sector
 * @size_in_bytes: width of the patched field
 * @value: a sector expression, or "CRC32(<sector>,<bytes>)" to checksum
 *	   the current image content
 *
 * The value is stored in little endian, as the programmer does on a device.
 *
 * Returns: 0 on success, -1 with the error reported on failure.
 */
int image_patch(struct qdl_image *image, unsigned int sector_size,
		const char *start_sector, unsigned int byte_offset,
		unsigned int size_in_bytes, const char *value)
{
	uint8_t bytes[sizeof(uint64_t)];
	uint64_t offset;
	uint64_t number;
	uint64_t result;
	bool from_end;
	unsigned int i;
	int ret;

	if (size_in_bytes == 0 || size_in_bytes > sizeof(bytes)) {
		ux_err("unsupported patch size of %u bytes\n", size_in_bytes);
		return -1;
	}

	if (image_sector_offset(image, start_sector, sector_size, &offset) < 0)
		return -1;

	if (!strncmp(value, IMAGE_CRC32_PREFIX, strlen(IMAGE_CRC32_PREFIX))) {
		ret = image_eval_crc32(image, value + strlen(IMAGE_CRC32_PREFIX),
				       sector_size, &result);
	} else {
		ret = image_parse_sector(value, &from_end, &number);
		if (!ret)
			ret = image_resolve_number(image, from_end, number,
						   sector_size, &result);
	}
	if (ret < 0) {
		ux_err("unable to evaluate patch value \"%s\"\n", value);
		return -1;
	}

	if (size_in_bytes < sizeof(bytes) && result >> (size_in_bytes * 8)) {
		ux_err("patch value \"%s\" does not fit in %u bytes\n", value, size_in_bytes);
		return -1;
	}

	for (i = 0; i < size_in_bytes; i++)
		bytes[i] = (result >> (i * 8)) & 0xff;

	return image_write(image, offset + byte_offset, bytes, size_in_bytes);
}

/*
 * Operations that end up addressing the storage of the simulated device. A
 * <program> without a file is not sent to it, but still describes a
 * partition of the disk.
 */
static bool image_op_is_relevant(struct firehose_op *op)
{
	switch (op->type) {
	case FIREHOSE_OP_PROGRAM:
		return op->filename || op->partition == 0;
	case FIREHOSE_OP_PATCH:
		return op->filename && !strcmp(op->filename, "DISK");
	case FIREHOSE_OP_ERASE:
	case FIREHOSE_OP_READ:
	case FIREHOSE_OP_GET_SHA256_DIGEST:
		return true;
	default:
		return false;
	}
}

static uint64_t image_program_sectors(struct firehose_op *op, unsigned int sector_size)
{
	uint64_t sectors = op->num_sectors;
	struct qdl_file file;

	/*
	 * The number of sectors of a <program> is the size of its partition,
	 * which is kept whatever the file needs. It is the file that counts
	 * for a partition without a size, and for a raw write, where the
	 * number of sectors, if any, only limits how much of it is written.
	 */
	if (op->filename && (!op->num_sectors || !op->sector_size) &&
	    !qdl_file_open(op->zip, op->filename, &file)) {
		sectors = ((uint64_t)qdl_file_getsize(&file) + sector_size - 1) / sector_size;
		qdl_file_close(&file);

		if (op->num_sectors && sectors > op->num_sectors)
			sectors = op->num_sectors;
	}

	/* A partition left to grow needs a sector to be a valid one at all */
	return sectors ? sectors : 1;
}

/*
 * Absolute form of @path with its directory resolved, which also works for
 * a file that does not exist yet. Returns NULL if it cannot be worked out.
 */
static char *image_canonical_path(const char *path)
{
#ifdef _WIN32
	return _fullpath(NULL, path, 0);
#else
	char resolved[PATH_MAX];
	const char *base = path;
	const char *dir = ".";
	char *canonical = NULL;
	char *slash;
	char *copy;
	size_t len;

	if (realpath(path, resolved))
		return strdup(resolved);

	copy = strdup(path);
	if (!copy)
		return NULL;

	slash = strrchr(copy, '/');
	if (slash) {
		*slash = '\0';
		dir = copy[0] ? copy : "/";
		base = slash + 1;
	} else {
		base = copy;
	}

	if (realpath(dir, resolved)) {
		len = strlen(resolved) + strlen(base) + 2;
		canonical = calloc(1, len);
		if (canonical)
			snprintf(canonical, len, "%s/%s", resolved, base);
	}
	free(copy);

	return canonical;
#endif
}

/*
 * Files are handled with size_t lengths, which on a 32-bit host cannot
 * describe a file of 4 GiB or more. Flashing one would quietly write only
 * part of it, so refuse it up front.
 */
static bool image_file_too_large(struct firehose_op *op)
{
#if SIZE_MAX < UINT64_MAX
	struct stat st;

	if (op->filename && !op->zip && !stat(op->filename, &st) &&
	    (uint64_t)st.st_size > SIZE_MAX) {
		ux_err("%s is too large to be handled on this host\n", op->filename);
		return true;
	}
#else
	(void)op;
#endif

	return false;
}

/**
 * image_same_file() - check if two paths name the same file
 * @a: one path
 * @b: other path
 *
 * Existing files are compared by identity where the platform offers one,
 * which sees through links. Otherwise, and for files yet to be created, the
 * resolved paths are compared.
 *
 * Returns: true if @a and @b are the same file.
 */
bool image_same_file(const char *a, const char *b)
{
	char *ca;
	char *cb;
	bool same;
#ifndef _WIN32
	struct stat sa;
	struct stat sb;

	if (!stat(a, &sa) && !stat(b, &sb))
		return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif

	ca = image_canonical_path(a);
	cb = image_canonical_path(b);
	if (!ca || !cb)
		same = !strcmp(a, b);
#ifdef _WIN32
	else
		same = !_stricmp(ca, cb);
#else
	else
		same = !strcmp(ca, cb);
#endif
	free(ca);
	free(cb);

	return same;
}

/**
 * image_plan() - validate the operations and size the image they need
 * @ops: list of firehose operations about to be run
 * @path: file the image is going to be written to
 * @default_sector_size: sector size of operations that do not carry one,
 *			 unless the storage type suggests another
 * @sector_size: sector size of the image
 * @min_size: smallest image, in bytes, that fits every <program>
 *
 * The image is a single flat block device, so operations addressing any
 * other physical partition (UFS LUN, eMMC hardware partition or NVMe
 * namespace) are refused, as are those that need a partition table lookup
 * on the device. For the same reason all of them must target one storage
 * device, which cannot be NAND.
 *
 * Files read or written by the operations must not be the image itself,
 * which would otherwise end up replacing them.
 *
 * The size is where the last absolutely placed <program> ends, plus room for
 * what is placed relative to the end of the disk, i.e. the backup GPT.
 *
 * Returns: 0 on success, -1 with the error reported on failure.
 */
int image_plan(struct list_head *ops, const char *path,
	       unsigned int default_sector_size,
	       unsigned int *sector_size, uint64_t *min_size)
{
	enum qdl_storage_type configured = QDL_STORAGE_UNKNOWN;
	enum qdl_storage_type storage = QDL_STORAGE_UNKNOWN;
	struct firehose_op *op;
	bool used = false;
	unsigned int size = 0;
	uint64_t sectors;
	uint64_t value;
	uint64_t head = 0;
	uint64_t tail = 0;
	bool from_end;

	list_for_each_entry(op, ops, node) {
		if (op->type == FIREHOSE_OP_CONFIGURE) {
			configured = op->storage_type;
			continue;
		}

		if (!image_op_is_relevant(op))
			continue;

		/* A configure may repeat, but not switch to other storage */
		if (used && configured != storage) {
			ux_err("disk image holds one storage device, but several are flashed\n");
			return -1;
		}
		storage = configured;
		used = true;

		if (storage == QDL_STORAGE_NAND || op->is_nand) {
			ux_err("disk image does not support NAND storage\n");
			return -1;
		}

		if ((op->type == FIREHOSE_OP_PROGRAM || op->type == FIREHOSE_OP_READ) &&
		    op->filename && !op->zip && image_same_file(op->filename, path)) {
			ux_err("%s is both the disk image and a file to %s\n", path,
			       op->type == FIREHOSE_OP_READ ? "read into" : "flash");
			return -1;
		}

		if (op->type == FIREHOSE_OP_PROGRAM && image_file_too_large(op))
			return -1;

		if (op->gpt_partition) {
			ux_err("partition \"%s\" cannot be looked up by name in a disk image\n",
			       op->gpt_partition);
			return -1;
		}

		if (op->partition != 0) {
			ux_err("disk image holds physical partition 0 only, but partition %d is addressed\n",
			       op->partition);
			return -1;
		}

		if (!op->sector_size)
			continue;

		if (size && size != op->sector_size) {
			ux_err("disk image cannot mix sector sizes %u and %u\n",
			       size, op->sector_size);
			return -1;
		}
		size = op->sector_size;
	}

	/*
	 * Raw commands carry no sector size, which a device would report. The
	 * image has none of its own either, so go by what the storage type
	 * commonly uses.
	 */
	if (!size)
		size = storage == QDL_STORAGE_EMMC ? 512 : default_sector_size;
	if (!size) {
		ux_err("unable to determine the sector size of the disk image\n");
		return -1;
	}

	list_for_each_entry(op, ops, node) {
		if (op->type != FIREHOSE_OP_PROGRAM || op->partition != 0)
			continue;

		if (image_parse_sector(op->start_sector, &from_end, &value) < 0) {
			ux_err("unsupported start sector \"%s\"\n", op->start_sector);
			return -1;
		}

		if (from_end) {
			if (value > tail)
				tail = value;
			continue;
		}

		sectors = image_program_sectors(op, size);
		if (value + sectors > head)
			head = value + sectors;
	}

	*sector_size = size;
	*min_size = (head + tail) * size;

	return 0;
}

/**
 * image_create() - create an empty disk image
 * @path: file the image ends up in
 * @size: size of the image in bytes
 *
 * The image is built in a temporary file next to @path, which only takes
 * its place once image_close() is told to keep it. Until then an existing
 * @path is left alone. The temporary file is created under a name of its
 * own, so that it cannot be one of the files the run reads or writes.
 *
 * The file is extended to @size without writing it out, so on most
 * filesystems only the programmed regions take up space.
 *
 * Returns: the image, or NULL with the error reported on failure.
 */
struct qdl_image *image_create(const char *path, uint64_t size)
{
	struct qdl_image *image;
	size_t len;
#ifndef _WIN32
	mode_t mask;
#endif

	image = calloc(1, sizeof(*image));
	if (!image)
		return NULL;

	image->path = strdup(path);
	len = strlen(path) + sizeof(IMAGE_TMP_SUFFIX);
	image->tmp_path = calloc(1, len);
	if (!image->path || !image->tmp_path)
		goto err_free;

	snprintf(image->tmp_path, len, "%s" IMAGE_TMP_SUFFIX, path);

	image->fd = mkstemp(image->tmp_path);
	if (image->fd < 0) {
		ux_err("failed to create a temporary file for %s: %s\n", path, strerror(errno));
		goto err_free;
	}

#ifdef _WIN32
	_setmode(image->fd, O_BINARY);
#else
	/* mkstemp() keeps the file private, an image is an ordinary one */
	mask = umask(0);
	umask(mask);
	fchmod(image->fd, 0644 & ~mask);
#endif

	if (image_resize(image, size) < 0) {
		close(image->fd);
		unlink(image->tmp_path);
		goto err_free;
	}
	image->size = size;

	return image;

err_free:
	free(image->tmp_path);
	free(image->path);
	free(image);

	return NULL;
}

/**
 * image_close() - finish the disk image
 * @image: disk image, may be NULL
 * @keep: move the image to its final path, as opposed to discarding it
 *
 * Returns: 0 on success, -1 with the error reported if the image could not
 * be kept.
 */
int image_close(struct qdl_image *image, bool keep)
{
	int ret = 0;

	if (!image)
		return 0;

	if (close(image->fd) < 0 && keep) {
		ux_err("failed to write %s: %s\n", image->tmp_path, strerror(errno));
		ret = -1;
	}

	if (keep && !ret) {
#ifdef _WIN32
		/* rename() does not replace an existing file there */
		if (!MoveFileExA(image->tmp_path, image->path, MOVEFILE_REPLACE_EXISTING)) {
			ux_err("failed to move the disk image to %s\n", image->path);
			ret = -1;
		}
#else
		if (rename(image->tmp_path, image->path) < 0) {
			ux_err("failed to move the disk image to %s: %s\n",
			       image->path, strerror(errno));
			ret = -1;
		}
#endif
	}

	if (!keep || ret)
		unlink(image->tmp_path);

	free(image->tmp_path);
	free(image->path);
	free(image);

	return ret;
}

const char *image_path(struct qdl_image *image)
{
	return image->path;
}

uint64_t image_size(struct qdl_image *image)
{
	return image->size;
}
