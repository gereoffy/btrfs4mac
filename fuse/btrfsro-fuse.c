// SPDX-License-Identifier: MIT
/*
 * btrfsro-fuse - mount a btrfs image or device read-only with FUSE
 * (macFUSE on macOS, libfuse 2 on Linux), on top of libbtrfsro.
 *
 *   btrfsro-fuse [-f] [-o option,...] <image|device> <mountpoint>
 *
 * Default options: ro, fsname=btrfs:<device>, volname=<label> (macOS).
 * Useful extra options: -o defer_permissions (macOS: ignore the Linux
 * owners/permissions), -o allow_other, -o uid=N,gid=N.
 */

#define FUSE_USE_VERSION 26
#define _GNU_SOURCE	/* asprintf on Linux */

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "btrfsro.h"

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

static btrfsro_fs *fs;
static struct btrfsro_volinfo volinfo;
static const char *device;
static int device_fd = -1;

/* ---- device access ---- */

static int dev_read(void *ctx, void *buf, uint64_t offset, size_t len)
{
	size_t done = 0;

	(void)ctx;
	while (done < len) {
		ssize_t n = pread(device_fd, (char *)buf + done, len - done,
				  (off_t)(offset + done));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		done += n;
	}
	return 0;
}

/* ---- helpers ---- */

/* Keep inode numbers unique across subvolumes */
static ino_t make_ino(const struct btrfsro_inode *inode)
{
	return (ino_t)(inode->ino ^ ((inode->subvol - volinfo.default_subvol) << 40));
}

static void set_times(struct stat *st, const struct btrfsro_stat *bs)
{
#ifdef __APPLE__
	st->st_atimespec.tv_sec = bs->atime.sec;
	st->st_atimespec.tv_nsec = bs->atime.nsec;
	st->st_mtimespec.tv_sec = bs->mtime.sec;
	st->st_mtimespec.tv_nsec = bs->mtime.nsec;
	st->st_ctimespec.tv_sec = bs->ctime.sec;
	st->st_ctimespec.tv_nsec = bs->ctime.nsec;
	st->st_birthtimespec.tv_sec = bs->otime.sec;
	st->st_birthtimespec.tv_nsec = bs->otime.nsec;
#else
	st->st_atim.tv_sec = bs->atime.sec;
	st->st_atim.tv_nsec = bs->atime.nsec;
	st->st_mtim.tv_sec = bs->mtime.sec;
	st->st_mtim.tv_nsec = bs->mtime.nsec;
	st->st_ctim.tv_sec = bs->ctime.sec;
	st->st_ctim.tv_nsec = bs->ctime.nsec;
#endif
}

/* Linux new_encode_dev() layout to major/minor */
static dev_t linux_rdev(uint64_t rdev)
{
	unsigned int major = (unsigned int)((rdev & 0xfff00) >> 8);
	unsigned int minor = (unsigned int)((rdev & 0xff) |
					    ((rdev >> 12) & 0xfff00));

	return makedev(major, minor);
}

static int fill_stat(const struct btrfsro_inode *inode, struct stat *st)
{
	struct btrfsro_stat bs;
	int ret;

	ret = btrfsro_stat(fs, inode, &bs);
	if (ret < 0)
		return ret;
	memset(st, 0, sizeof(*st));
	st->st_ino = make_ino(inode);
	st->st_mode = bs.mode;
	st->st_nlink = bs.nlink;
	st->st_uid = bs.uid;
	st->st_gid = bs.gid;
	st->st_rdev = linux_rdev(bs.rdev);
	st->st_size = (off_t)bs.size;
	st->st_blocks = (blkcnt_t)(bs.nbytes / 512);
	st->st_blksize = volinfo.sectorsize;
	set_times(st, &bs);
	return 0;
}

/* ---- FUSE operations ---- */

static int op_getattr(const char *path, struct stat *st)
{
	struct btrfsro_inode inode;
	int ret;

	ret = btrfsro_resolve(fs, path, &inode);
	if (ret < 0)
		return ret;
	return fill_stat(&inode, st);
}

static int op_readlink(const char *path, char *buf, size_t size)
{
	struct btrfsro_inode inode;
	int ret;

	char target[4096];

	ret = btrfsro_resolve(fs, path, &inode);
	if (ret < 0)
		return ret;
	ret = btrfsro_readlink(fs, &inode, target, sizeof(target));
	if (ret < 0)
		return ret;
	/* FUSE: a target longer than the buffer is truncated */
	if (size > 0) {
		size_t n = (size_t)ret < size - 1 ? (size_t)ret : size - 1;

		memcpy(buf, target, n);
		buf[n] = '\0';
	}
	return 0;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
	struct btrfsro_inode *inode;
	int ret;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EROFS;
	inode = malloc(sizeof(*inode));
	if (!inode)
		return -ENOMEM;
	ret = btrfsro_resolve(fs, path, inode);
	if (ret == 0 && inode->type == BTRFSRO_FT_DIR)
		ret = -EISDIR;
	if (ret < 0) {
		free(inode);
		return ret;
	}
	fi->fh = (uint64_t)(uintptr_t)inode;
	/* The file system never changes: keep cached pages across opens */
	fi->keep_cache = 1;
	return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t offset,
		   struct fuse_file_info *fi)
{
	const struct btrfsro_inode *inode =
		(const struct btrfsro_inode *)(uintptr_t)fi->fh;
	ssize_t ret;

	(void)path;
	if (offset < 0)
		return -EINVAL;
	ret = btrfsro_read(fs, inode, (uint64_t)offset, buf, size);
	return (int)ret;
}

static int op_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	free((void *)(uintptr_t)fi->fh);
	return 0;
}

struct readdir_ctx {
	void *buf;
	fuse_fill_dir_t filler;
};

static int readdir_cb(void *ctx, const char *name, size_t name_len,
		      const struct btrfsro_inode *entry, uint64_t index)
{
	struct readdir_ctx *rc = ctx;
	char namebuf[256];
	struct stat st;

	if (name_len >= sizeof(namebuf))
		return 0;
	memcpy(namebuf, name, name_len);
	namebuf[name_len] = '\0';

	memset(&st, 0, sizeof(st));
	st.st_ino = make_ino(entry);
	switch (entry->type) {
	case BTRFSRO_FT_DIR:		st.st_mode = S_IFDIR; break;
	case BTRFSRO_FT_SYMLINK:	st.st_mode = S_IFLNK; break;
	case BTRFSRO_FT_CHRDEV:		st.st_mode = S_IFCHR; break;
	case BTRFSRO_FT_BLKDEV:		st.st_mode = S_IFBLK; break;
	case BTRFSRO_FT_FIFO:		st.st_mode = S_IFIFO; break;
	case BTRFSRO_FT_SOCK:		st.st_mode = S_IFSOCK; break;
	default:			st.st_mode = S_IFREG; break;
	}
	/* The offset is the cookie of the next entry: index + 1 */
	return rc->filler(rc->buf, namebuf, &st, (off_t)(index + 1));
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		      off_t offset, struct fuse_file_info *fi)
{
	struct readdir_ctx rc = { buf, filler };
	struct btrfsro_inode dir;
	int ret;

	(void)fi;
	ret = btrfsro_resolve(fs, path, &dir);
	if (ret < 0)
		return ret;
	if (dir.type != BTRFSRO_FT_DIR)
		return -ENOTDIR;

	/* Offsets: 0 -> ".", 1 -> "..", n >= 2 -> btrfs dir index n */
	if (offset < 1 && filler(buf, ".", NULL, 1))
		return 0;
	if (offset < 2 && filler(buf, "..", NULL, 2))
		return 0;
	return btrfsro_readdir(fs, &dir, offset < 2 ? 2 : (uint64_t)offset,
			       readdir_cb, &rc);
}

static int op_statfs(const char *path, struct statvfs *sv)
{
	uint64_t bs = volinfo.sectorsize;
	uint64_t used = volinfo.bytes_used < volinfo.total_bytes ?
			volinfo.bytes_used : volinfo.total_bytes;

	(void)path;
	memset(sv, 0, sizeof(*sv));
	sv->f_bsize = bs;
	sv->f_frsize = bs;
	sv->f_blocks = volinfo.total_bytes / bs;
	sv->f_bfree = (volinfo.total_bytes - used) / bs;
	sv->f_bavail = sv->f_bfree;
	sv->f_namemax = 255;
	return 0;
}

static int op_listxattr(const char *path, char *list, size_t size)
{
	struct btrfsro_inode inode;
	int ret;

	ret = btrfsro_resolve(fs, path, &inode);
	if (ret < 0)
		return ret;
	return (int)btrfsro_listxattr(fs, &inode, size ? list : NULL, size);
}

#ifdef __APPLE__
static int op_getxattr(const char *path, const char *name, char *value,
		       size_t size, uint32_t position)
#else
static int op_getxattr(const char *path, const char *name, char *value,
		       size_t size)
#endif
{
	struct btrfsro_inode inode;
	ssize_t ret;

#ifdef __APPLE__
	if (position != 0)
		return -ENOATTR;
#endif
	ret = btrfsro_resolve(fs, path, &inode);
	if (ret < 0)
		return (int)ret;
	ret = btrfsro_getxattr(fs, &inode, name, size ? value : NULL, size);
	if (ret == -ENODATA)
		return -ENOATTR;
	return (int)ret;
}

static void op_destroy(void *data)
{
	(void)data;
	btrfsro_unmount(fs);
	fs = NULL;
}

static const struct fuse_operations ops = {
	.getattr	= op_getattr,
	.readlink	= op_readlink,
	.open		= op_open,
	.read		= op_read,
	.release	= op_release,
	.readdir	= op_readdir,
	.statfs		= op_statfs,
	.listxattr	= op_listxattr,
	.getxattr	= op_getxattr,
	.destroy	= op_destroy,
};

/* ---- main ---- */

static int opt_proc(void *data, const char *arg, int key,
		    struct fuse_args *outargs)
{
	(void)data;
	(void)outargs;
	if (key == FUSE_OPT_KEY_NONOPT && device == NULL) {
		device = arg;
		return 0;	/* consumed */
	}
	return 1;
}

int main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct btrfsro_io io;
	struct stat st;
	char *opt;
	int ret;

	if (fuse_opt_parse(&args, NULL, NULL, opt_proc) < 0)
		return 1;
	if (device == NULL) {
		fprintf(stderr,
			"usage: %s [-f] [-o option,...] <image|device> <mountpoint>\n",
			argv[0]);
		return 2;
	}

	device_fd = open(device, O_RDONLY);
	if (device_fd < 0) {
		perror(device);
		return 1;
	}
	memset(&io, 0, sizeof(io));
	io.read = dev_read;
	if (fstat(device_fd, &st) == 0 && S_ISREG(st.st_mode))
		io.size = (uint64_t)st.st_size;

	ret = btrfsro_mount(&io, &fs);
	if (ret < 0) {
		fprintf(stderr, "%s: cannot read btrfs: %s\n", device,
			strerror(-ret));
		return 1;
	}
	btrfsro_get_volinfo(fs, &volinfo);

	fuse_opt_add_arg(&args, "-oro");
	fuse_opt_add_arg(&args, "-ouse_ino");
	char *abs_device = realpath(device, NULL);

	if (asprintf(&opt, "-ofsname=btrfs:%s",
		     abs_device ? abs_device : device) > 0) {
		fuse_opt_add_arg(&args, opt);
		free(opt);
	}
	free(abs_device);
#ifdef __APPLE__
	if (asprintf(&opt, "-ovolname=%s",
		     volinfo.label[0] ? volinfo.label : "btrfs") > 0) {
		fuse_opt_add_arg(&args, opt);
		free(opt);
	}
#endif

	ret = fuse_main(args.argc, args.argv, &ops, NULL);
	fuse_opt_free_args(&args);
	close(device_fd);
	return ret;
}
