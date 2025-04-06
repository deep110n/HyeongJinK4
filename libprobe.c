#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <err.h>
#include <linux/fs.h>

#include "libprobe.h"

static const char const *ftype_to_name[FKTY_MAX] = {
	[FKTY_GOOD]		= "good",
	[FKTY_BAD]		= "bad",
	[FKTY_LIMBO]		= "limbo",
	[FKTY_WRAPAROUND]	= "wraparound",
};

const char *fake_type_to_name(enum fake_type fake_type)
{
	assert(fake_type < FKTY_MAX);
	return ftype_to_name[fake_type];
}

struct device {
	int (*read_block)(struct device *dev, char *buf, uint64_t block);
	int (*write_block)(struct device *dev, char *buf, uint64_t block);
	int (*get_size_gb)(struct device *dev);
	void (*free)(struct device *dev);
};

struct file_device {
	/* This must be the first field. See dev_fdev() for details. */
	struct device dev;

	const char *filename;
	int fd;
	int file_size_gb;
	int fake_size_gb;
	enum fake_type fake_type;
	/* 3 free bytes. */
};

static inline struct file_device *dev_fdev(struct device *dev)
{
	return (struct file_device *)dev;
}

#define GIGABYTE	(1 << 30)

/* XXX Replace expressions block * BLOCK_SIZE to block << BLOCK_SIZE_BITS,
 * and do the same for expressions like GIGABYTE * variable.
 */
static int fdev_read_block(struct device *dev, char *buf, uint64_t block)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t offset = block * BLOCK_SIZE;
	int done;

	switch (fdev->fake_type) {
	case FKTY_LIMBO:
		if (offset >= GIGABYTE * fdev->file_size_gb) {
			memset(buf, 0, BLOCK_SIZE);
			return 0;
		}
		break;

	/* XXX Support FKTY_TRUNCATE.
	 * That is, it drops the highest bits, and addresses the real memory
	 * with the resulting address.
	 *
	 * If @fake_size_gb % @file_size_gb == 0, it's identical to
	 * FKTY_WRAPAROUND.
	 */

	case FKTY_WRAPAROUND:
		offset %= GIGABYTE * fdev->file_size_gb;
		/* Fall through. */

	case  FKTY_GOOD:
		break;

	default:
		assert(0);
	}

	assert(lseek(fdev->fd, offset, SEEK_SET) == offset);

	done = 0;
	do {
		ssize_t rc = read(fdev->fd, buf + done, BLOCK_SIZE - done);
		assert(rc >= 0);
		if (!rc) {
			/* Tried to read beyond the end of the file. */
			assert(!done);
			memset(buf, 0, BLOCK_SIZE);
			done += BLOCK_SIZE;
		}
		done += rc;
	} while (done < BLOCK_SIZE);

	return 0;
}

static int write_all(int fd, char *buf, int count)
{
	int done = 0;
	do {
		ssize_t rc = write(fd, buf + done, count - done);
		assert(rc >= 0); /* Did the write() went right? */
		done += rc;
	} while (done < count);
	return 0;
}

static int fdev_write_block(struct device *dev, char *buf, uint64_t block)
{
	struct file_device *fdev = dev_fdev(dev);
	off_t offset = block * BLOCK_SIZE;

	switch (fdev->fake_type) {
	case FKTY_LIMBO:
		if (offset >= GIGABYTE * fdev->file_size_gb)
			return 0;
		break;

	case FKTY_WRAPAROUND:
		offset %= GIGABYTE * fdev->file_size_gb;
		/* Fall through. */

	case  FKTY_GOOD:
		break;

	default:
		assert(0);
	}

	assert(lseek(fdev->fd, offset, SEEK_SET) == offset);
	return write_all(fdev->fd, buf, BLOCK_SIZE);
}

static int fdev_get_size_gb(struct device *dev)
{
	return dev_fdev(dev)->fake_size_gb;
}

static void fdev_free(struct device *dev)
{
	struct file_device *fdev = dev_fdev(dev);
	assert(!close(fdev->fd));
	assert(!unlink(fdev->filename));
	free((void *)fdev->filename);
}

/* XXX Validate parameters.
 * For example, if @fake_type == FKTY_GOOD, then @fake_size_gb and
 * fake_size_gb must be equal.
 */
struct device *create_file_device(const char *filename,
	int file_size_gb, int fake_size_gb, enum fake_type fake_type)
{
	struct file_device *fdev = malloc(sizeof(*fdev));
	if (!fdev)
		goto error;

	fdev->filename = strdup(filename);
	if (!fdev->filename)
		goto fdev;

	fdev->fd = open(filename, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fdev->fd < 0) {
		err(errno, "Can't create file `%s'", filename);
		goto filename;
	}

	fdev->file_size_gb = file_size_gb;
	fdev->fake_size_gb = fake_size_gb;
	fdev->fake_type = fake_type;

	fdev->dev.read_block = fdev_read_block;
	fdev->dev.write_block = fdev_write_block;
	fdev->dev.get_size_gb = fdev_get_size_gb;
	fdev->dev.free = fdev_free;

	return &fdev->dev;

filename:
	free((void *)fdev->filename);
fdev:
	free(fdev);
error:
	return NULL;
}

struct block_device {
	/* This must be the first field. See dev_bdev() for details. */
	struct device dev;

	int fd;
};

static inline struct block_device *dev_bdev(struct device *dev)
{
	return (struct block_device *)dev;
}

static int read_all(int fd, char *buf, int count)
{
	int done = 0;
	do {
		ssize_t rc = read(fd, buf + done, count - done);
		assert(rc >= 0); /* Did the read() went right? */
		assert(rc != 0); /* We should never hit the end of the file. */
		done += rc;
	} while (done < count);
	return 0;
}

static int bdev_read_block(struct device *dev, char *buf, uint64_t block)
{
	struct block_device *bdev = dev_bdev(dev);
	off_t offset = block * BLOCK_SIZE;
	assert(lseek(bdev->fd, offset, SEEK_SET) == offset);
	return read_all(bdev->fd, buf, BLOCK_SIZE);
}

static int bdev_write_block(struct device *dev, char *buf, uint64_t block)
{
	struct block_device *bdev = dev_bdev(dev);
	off_t offset = block * BLOCK_SIZE;
	assert(lseek(bdev->fd, offset, SEEK_SET) == offset);
	return write_all(bdev->fd, buf, BLOCK_SIZE);
}

static int bdev_get_size_gb(struct device *dev)
{
	uint64_t size_bytes;
	assert(!ioctl(dev_bdev(dev)->fd, BLKGETSIZE64, &size_bytes));
	/* XXX Support everything. Specially devices smaller than 1GB! */
	return size_bytes >> 30;
}

static void bdev_free(struct device *dev)
{
	assert(!close(dev_bdev(dev)->fd));
}

/* XXX Test if it's a device, or a partition.
 * If a partition, warn user, and ask for confirmation before
 * going ahead.
 * Suggest how to call f3probe with the correct device name if
 * the block device is a partition.
 */
/* XXX Test for write access of the block device to give
 * a nice error message.
 * If it fails, suggest running f3probe as root.
 */
struct device *create_block_device(const char *filename)
{
	struct block_device *bdev = malloc(sizeof(*bdev));
	if (!bdev)
		goto error;

	bdev->fd = open(filename, O_RDWR | O_DIRECT | O_SYNC);
	if (bdev->fd < 0) {
		err(errno, "Can't open device `%s'", filename);
		goto bdev;
	}

	bdev->dev.read_block = bdev_read_block;
	bdev->dev.write_block = bdev_write_block;
	bdev->dev.get_size_gb = bdev_get_size_gb;
	bdev->dev.free = bdev_free;

	return &bdev->dev;

bdev:
	free(bdev);
error:
	return NULL;
}

void free_device(struct device *dev)
{
	dev->free(dev);
	free(dev);
}

static inline int dev_read_block(struct device *dev, char *buf, uint64_t block)
{
	return dev->read_block(dev, buf, block);
}

static inline int dev_write_block(struct device *dev, char *buf, uint64_t block)
{
	return dev->write_block(dev, buf, block);
}

static inline int dev_get_size_gb(struct device *dev)
{
	return dev->get_size_gb(dev);
}

/* XXX Write random data for testing.
 * There would be a random seed, and all the other blocks would be
 * this seed XOR'd with the number of the test.
 */
static void fill_buffer(char *buf, int len, int signature)
{
	memset(buf, signature, len);
}

static inline int equal_blk(const char *b1, const char *b2)
{
	return !memcmp(b1, b2, BLOCK_SIZE);
}

static inline void *align_512(void *p)
{
	uintptr_t ip = (uintptr_t)p;
	return (void *)(   (ip + 511) & ~511   );
}

/* XXX Don't write at the very beginning of the card to avoid
 * losing the partition table.
 * But write at a random locations to make harder for fake chips
 * to become "smarter".
 */
/* XXX Finish testing the last block, and the next one that should fail.
 * Then report the last block, so user can create the largest partition.
 */
/* XXX Properly handle read and write errors. */
enum fake_type probe_device(struct device *dev, int *preal_size_gb)
{
	int device_size_gb = dev_get_size_gb(dev);
	char stack[511 + 3 * BLOCK_SIZE];
	char *first_blk, *stamp_blk, *probe_blk;
	const int step = GIGABYTE / BLOCK_SIZE;
	uint64_t first_pos = 10;
	uint64_t pos = first_pos + step;
	int i;

	assert(device_size_gb > 0);

	/* Aligning these pointers is necessary to directly read and write
	 * the block device.
	 * For the file device, this is superfluous.
	 */
	first_blk = align_512(stack);
	stamp_blk = first_blk + BLOCK_SIZE;
	probe_blk = stamp_blk + BLOCK_SIZE;

	/* Base case. */
	fill_buffer(first_blk, BLOCK_SIZE, 1);
	dev_write_block(dev, first_blk, first_pos);
	dev_read_block(dev, probe_blk, first_pos);
	if (!equal_blk(first_blk, probe_blk)) {
		/* There is a block before the first 1GB that seems to
		 * be damaged. Trying a second time...
		 */
		dev_write_block(dev, first_blk, first_pos);
		dev_read_block(dev, probe_blk, first_pos);
		if (!equal_blk(first_blk, probe_blk)) {
			/* Okay, this device is damaged. */
			goto bad;
		}
	}

	/* Inductive step. */
	fill_buffer(stamp_blk, BLOCK_SIZE, 2);
	for (i = 1; i < device_size_gb; i++) {
		dev_write_block(dev, stamp_blk, pos);

		dev_read_block(dev, probe_blk, first_pos);
		if (!equal_blk(first_blk, probe_blk)) {
			/* Wrapping around? */
			if (equal_blk(stamp_blk, probe_blk)) {
				/* yes. */
				*preal_size_gb = i;
				return FKTY_WRAPAROUND;
			}

			/* The block at @first_pos changed to a value
			 * different from the one written.
			 * Trying a second time...
			 */
			dev_write_block(dev, first_blk, first_pos);
			dev_write_block(dev, stamp_blk, pos);
			dev_read_block(dev, probe_blk, first_pos);
			if (!equal_blk(first_blk, probe_blk)) {
				if (equal_blk(stamp_blk, probe_blk)) {
					*preal_size_gb = i;
					return FKTY_WRAPAROUND;
				}
				/* Okay, this device is damaged. */
				goto bad;
			}
		}

		dev_read_block(dev, probe_blk, pos);
		if (!equal_blk(stamp_blk, probe_blk)) {
			*preal_size_gb = i;
			return FKTY_LIMBO;
		}

		pos += step;
	}

	*preal_size_gb = device_size_gb;
	return FKTY_GOOD;

bad:
	*preal_size_gb = 0;
	return FKTY_BAD;
}
