// SPDX-License-Identifier: MIT
/*
 * btrfsro - command line front end of libbtrfsro.
 *
 *   btrfsro info     <image>
 *   btrfsro ls       [-l] [-R] <image> [path]
 *   btrfsro cat      [-o offset] [-n length] <image> <path>
 *   btrfsro stat     <image> <path>
 *   btrfsro readlink <image> <path>
 *   btrfsro xattr    <image> <path>
 *   btrfsro extract  <image> <path> <destdir>
 *   btrfsro manifest <image> [path]
 *   btrfsro extents  [-s] <image> <path>
 *
 * Global options (before the command): -v (verbose), --no-verify (skip data
 * checksum verification).
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <CommonCrypto/CommonDigest.h>
#include "btrfsro.h"

static bool opt_verbose;
static bool opt_no_verify;

/* ---- device access ---- */

struct file_dev {
	int fd;
};

static int file_read(void *ctx, void *buf, uint64_t offset, size_t len)
{
	struct file_dev *dev = ctx;
	size_t done = 0;

	while (done < len) {
		ssize_t ret = pread(dev->fd, (char *)buf + done, len - done,
				    (off_t)(offset + done));
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return -EIO;
		done += ret;
	}
	return 0;
}

static int open_device(const char *path, struct file_dev *dev,
		       struct btrfsro_io *io)
{
	struct stat st;

	dev->fd = open(path, O_RDONLY);
	if (dev->fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -errno;
	}
	memset(io, 0, sizeof(*io));
	io->read = file_read;
	io->ctx = dev;
	if (fstat(dev->fd, &st) == 0) {
		if (S_ISREG(st.st_mode)) {
			io->size = st.st_size;
		} else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
			uint64_t count = 0;
			uint32_t bsize = 0;

			if (ioctl(dev->fd, DKIOCGETBLOCKCOUNT, &count) == 0 &&
			    ioctl(dev->fd, DKIOCGETBLOCKSIZE, &bsize) == 0)
				io->size = count * bsize;
		}
	}
	return 0;
}

static btrfsro_fs *mount_image(const char *path, struct file_dev *dev)
{
	struct btrfsro_io io;
	btrfsro_fs *fs;
	int ret;

	if (open_device(path, dev, &io) < 0)
		return NULL;
	ret = btrfsro_mount(&io, &fs);
	if (ret < 0) {
		fprintf(stderr, "cannot mount %s: %s\n", path, strerror(-ret));
		close(dev->fd);
		return NULL;
	}
	if (opt_no_verify)
		btrfsro_set_verify_data(fs, 0);
	return fs;
}

static void unmount_image(btrfsro_fs *fs, struct file_dev *dev)
{
	btrfsro_unmount(fs);
	close(dev->fd);
}

static int resolve_or_die(btrfsro_fs *fs, const char *path,
			  struct btrfsro_inode *ino)
{
	int ret = btrfsro_resolve(fs, path, ino);

	if (ret < 0)
		fprintf(stderr, "%s: %s\n", path, strerror(-ret));
	return ret;
}

/* ---- helpers ---- */

static char type_char(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:	return '-';
	case S_IFDIR:	return 'd';
	case S_IFLNK:	return 'l';
	case S_IFCHR:	return 'c';
	case S_IFBLK:	return 'b';
	case S_IFIFO:	return 'p';
	case S_IFSOCK:	return 's';
	}
	return '?';
}

static void mode_string(uint32_t mode, char out[11])
{
	static const char rwx[] = "rwxrwxrwx";

	out[0] = type_char(mode);
	for (int i = 0; i < 9; i++)
		out[i + 1] = (mode & (0400 >> i)) ? rwx[i] : '-';
	if (mode & S_ISUID)
		out[3] = (mode & S_IXUSR) ? 's' : 'S';
	if (mode & S_ISGID)
		out[6] = (mode & S_IXGRP) ? 's' : 'S';
	if (mode & S_ISVTX)
		out[9] = (mode & S_IXOTH) ? 't' : 'T';
	out[10] = '\0';
}

static void format_time(const struct btrfsro_timespec *ts, char *buf,
			size_t size)
{
	time_t t = (time_t)ts->sec;
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm);
}

struct dirent_rec {
	char *name;
	struct btrfsro_inode ino;
	uint64_t index;
};

struct dirent_list {
	struct dirent_rec *ents;
	size_t nr;
	size_t max;
};

static int collect_cb(void *ctx, const char *name, size_t name_len,
		      const struct btrfsro_inode *entry, uint64_t index)
{
	struct dirent_list *list = ctx;

	if (list->nr == list->max) {
		size_t new_max = list->max ? list->max * 2 : 64;
		struct dirent_rec *n = realloc(list->ents,
					       new_max * sizeof(*n));
		if (!n)
			return 1;
		list->ents = n;
		list->max = new_max;
	}
	list->ents[list->nr].name = strndup(name, name_len);
	list->ents[list->nr].ino = *entry;
	list->ents[list->nr].index = index;
	list->nr++;
	return 0;
}

static void free_dirent_list(struct dirent_list *list)
{
	for (size_t i = 0; i < list->nr; i++)
		free(list->ents[i].name);
	free(list->ents);
	memset(list, 0, sizeof(*list));
}

static int cmp_dirent_name(const void *a, const void *b)
{
	return strcmp(((const struct dirent_rec *)a)->name,
		      ((const struct dirent_rec *)b)->name);
}

static int read_dir(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		    struct dirent_list *list, bool sorted)
{
	int ret = btrfsro_readdir(fs, dir, 0, collect_cb, list);

	if (ret == 0 && sorted)
		qsort(list->ents, list->nr, sizeof(*list->ents),
		      cmp_dirent_name);
	return ret;
}

static char *join_path(const char *a, const char *b)
{
	size_t la = strlen(a);
	char *out = malloc(la + strlen(b) + 2);

	if (la && a[la - 1] == '/')
		sprintf(out, "%s%s", a, b);
	else
		sprintf(out, "%s/%s", a, b);
	return out;
}

/* ---- info ---- */

static int cmd_info(int argc, char **argv)
{
	static const char *csum_names[] = { "crc32c", "xxhash64", "sha256",
					    "blake2b" };
	struct btrfsro_volinfo vi;
	struct file_dev dev;
	btrfsro_fs *fs;

	if (argc != 1) {
		fprintf(stderr, "usage: btrfsro info <image>\n");
		return 2;
	}
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	btrfsro_get_volinfo(fs, &vi);
	printf("label:          %s\n", vi.label);
	printf("fsid:           ");
	for (int i = 0; i < 16; i++)
		printf("%02x%s", vi.fsid[i],
		       (i == 3 || i == 5 || i == 7 || i == 9) ? "-" : "");
	printf("\n");
	printf("generation:     %" PRIu64 "\n", vi.generation);
	printf("total bytes:    %" PRIu64 "\n", vi.total_bytes);
	printf("bytes used:     %" PRIu64 "\n", vi.bytes_used);
	printf("devices:        %" PRIu64 "\n", vi.num_devices);
	printf("sectorsize:     %u\n", vi.sectorsize);
	printf("nodesize:       %u\n", vi.nodesize);
	printf("csum type:      %s\n",
	       vi.csum_type < 4 ? csum_names[vi.csum_type] : "unknown");
	printf("incompat flags: 0x%" PRIx64 "\n", vi.incompat_flags);
	printf("compat_ro:      0x%" PRIx64 "\n", vi.compat_ro_flags);
	printf("default subvol: %" PRIu64 "\n", vi.default_subvol);
	unmount_image(fs, &dev);
	return 0;
}

/* ---- ls ---- */

static void print_long(btrfsro_fs *fs, const struct btrfsro_inode *ino,
		       const char *name)
{
	struct btrfsro_stat st;
	char mode[11];
	char tbuf[32];
	int ret;

	ret = btrfsro_stat(fs, ino, &st);
	if (ret < 0) {
		printf("?????????? %s (stat: %s)\n", name, strerror(-ret));
		return;
	}
	mode_string(st.mode, mode);
	format_time(&st.mtime, tbuf, sizeof(tbuf));
	printf("%s %3u %5u %5u %12" PRIu64 " %s %s", mode, st.nlink, st.uid,
	       st.gid, st.size, tbuf, name);
	if (S_ISLNK(st.mode)) {
		char target[4096];

		if (btrfsro_readlink(fs, ino, target, sizeof(target)) >= 0)
			printf(" -> %s", target);
	}
	printf("\n");
}

static int ls_dir(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		  const char *path, bool lng, bool recursive, bool header)
{
	struct dirent_list list = {};
	int ret;

	ret = read_dir(fs, dir, &list, true);
	if (ret < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(-ret));
		free_dirent_list(&list);
		return ret;
	}
	if (header)
		printf("%s:\n", path);
	for (size_t i = 0; i < list.nr; i++) {
		if (lng)
			print_long(fs, &list.ents[i].ino, list.ents[i].name);
		else
			printf("%s\n", list.ents[i].name);
	}
	if (recursive) {
		for (size_t i = 0; i < list.nr; i++) {
			char *sub;

			if (list.ents[i].ino.type != BTRFSRO_FT_DIR)
				continue;
			sub = join_path(path, list.ents[i].name);
			printf("\n");
			ret = ls_dir(fs, &list.ents[i].ino, sub, lng, true,
				     true);
			free(sub);
		}
	}
	free_dirent_list(&list);
	return 0;
}

static int cmd_ls(int argc, char **argv)
{
	struct btrfsro_inode ino;
	struct file_dev dev;
	bool lng = false, recursive = false;
	const char *path = "/";
	btrfsro_fs *fs;
	int opt;
	int ret;

	optind = 1;
	while ((opt = getopt(argc + 1, argv - 1, "lR")) != -1) {
		if (opt == 'l')
			lng = true;
		else if (opt == 'R')
			recursive = true;
		else
			return 2;
	}
	argc -= optind - 1;
	argv += optind - 1;
	if (argc < 1 || argc > 2) {
		fprintf(stderr, "usage: btrfsro ls [-l] [-R] <image> [path]\n");
		return 2;
	}
	if (argc == 2)
		path = argv[1];
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, path, &ino);
	if (ret == 0) {
		if (ino.type == BTRFSRO_FT_DIR)
			ret = ls_dir(fs, &ino, path, lng, recursive, recursive);
		else if (lng)
			print_long(fs, &ino, path);
		else
			printf("%s\n", path);
	}
	unmount_image(fs, &dev);
	return ret < 0 ? 1 : 0;
}

/* ---- cat ---- */

#define IO_CHUNK (1024 * 1024)

static int cmd_cat(int argc, char **argv)
{
	struct btrfsro_inode ino;
	struct btrfsro_stat st;
	struct file_dev dev;
	uint64_t offset = 0;
	uint64_t length = UINT64_MAX;
	btrfsro_fs *fs;
	char *buf;
	int opt;
	int ret;

	optind = 1;
	while ((opt = getopt(argc + 1, argv - 1, "o:n:")) != -1) {
		if (opt == 'o')
			offset = strtoull(optarg, NULL, 0);
		else if (opt == 'n')
			length = strtoull(optarg, NULL, 0);
		else
			return 2;
	}
	argc -= optind - 1;
	argv += optind - 1;
	if (argc != 2) {
		fprintf(stderr, "usage: btrfsro cat [-o offset] [-n length] <image> <path>\n");
		return 2;
	}
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, argv[1], &ino);
	if (ret < 0)
		goto out;
	ret = btrfsro_stat(fs, &ino, &st);
	if (ret < 0)
		goto out;
	buf = malloc(IO_CHUNK);
	while (length > 0) {
		size_t want = length < IO_CHUNK ? (size_t)length : IO_CHUNK;
		ssize_t got = btrfsro_read(fs, &ino, offset, buf, want);

		if (got < 0) {
			fprintf(stderr, "%s: read at %" PRIu64 ": %s\n",
				argv[1], offset, strerror((int)-got));
			ret = (int)got;
			break;
		}
		if (got == 0)
			break;
		if (fwrite(buf, 1, got, stdout) != (size_t)got) {
			ret = -EIO;
			break;
		}
		offset += got;
		length -= got;
	}
	free(buf);
out:
	unmount_image(fs, &dev);
	return ret < 0 ? 1 : 0;
}

/* ---- stat ---- */

static int cmd_stat(int argc, char **argv)
{
	struct btrfsro_inode ino;
	struct btrfsro_stat st;
	struct file_dev dev;
	btrfsro_fs *fs;
	char mode[11];
	char t1[32], t2[32], t3[32], t4[32];
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: btrfsro stat <image> <path>\n");
		return 2;
	}
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, argv[1], &ino);
	if (ret == 0)
		ret = btrfsro_stat(fs, &ino, &st);
	if (ret == 0) {
		mode_string(st.mode, mode);
		format_time(&st.atime, t1, sizeof(t1));
		format_time(&st.mtime, t2, sizeof(t2));
		format_time(&st.ctime, t3, sizeof(t3));
		format_time(&st.otime, t4, sizeof(t4));
		printf("  File: %s\n", argv[1]);
		printf("  Size: %" PRIu64 "  Bytes: %" PRIu64 "  Subvol: %" PRIu64
		       "  Inode: %" PRIu64 "  Links: %u\n", st.size, st.nbytes,
		       st.subvol, st.ino, st.nlink);
		printf("Access: (%04o/%s)  Uid: %u  Gid: %u  Rdev: 0x%" PRIx64
		       "\n", st.mode & 07777, mode, st.uid, st.gid, st.rdev);
		printf(" Flags: 0x%" PRIx64 "  Generation: %" PRIu64 "\n",
		       st.flags, st.generation);
		printf("Access: %s.%09u UTC\n", t1, st.atime.nsec);
		printf("Modify: %s.%09u UTC\n", t2, st.mtime.nsec);
		printf("Change: %s.%09u UTC\n", t3, st.ctime.nsec);
		printf(" Birth: %s.%09u UTC\n", t4, st.otime.nsec);
	} else {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(-ret));
	}
	unmount_image(fs, &dev);
	return ret < 0 ? 1 : 0;
}

/* ---- readlink ---- */

static int cmd_readlink(int argc, char **argv)
{
	struct btrfsro_inode ino;
	struct file_dev dev;
	char target[4096];
	btrfsro_fs *fs;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: btrfsro readlink <image> <path>\n");
		return 2;
	}
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, argv[1], &ino);
	if (ret == 0)
		ret = btrfsro_readlink(fs, &ino, target, sizeof(target));
	if (ret >= 0)
		printf("%s\n", target);
	else
		fprintf(stderr, "%s: %s\n", argv[1], strerror(-ret));
	unmount_image(fs, &dev);
	return ret < 0 ? 1 : 0;
}

/* ---- xattr ---- */

static int cmd_xattr(int argc, char **argv)
{
	struct btrfsro_inode ino;
	struct file_dev dev;
	btrfsro_fs *fs;
	ssize_t len;
	char *names = NULL;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "usage: btrfsro xattr <image> <path>\n");
		return 2;
	}
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, argv[1], &ino);
	if (ret < 0)
		goto out;
	len = btrfsro_listxattr(fs, &ino, NULL, 0);
	if (len < 0) {
		ret = (int)len;
		goto out;
	}
	names = malloc(len + 1);
	len = btrfsro_listxattr(fs, &ino, names, len);
	for (char *n = names; len > 0 && n < names + len; n += strlen(n) + 1) {
		ssize_t vlen = btrfsro_getxattr(fs, &ino, n, NULL, 0);
		unsigned char *val;

		printf("%s (%zd bytes):", n, vlen);
		if (vlen > 0) {
			val = malloc(vlen);
			btrfsro_getxattr(fs, &ino, n, val, vlen);
			for (ssize_t i = 0; i < vlen && i < 64; i++)
				printf(" %02x", val[i]);
			free(val);
		}
		printf("\n");
	}
out:
	free(names);
	if (ret < 0)
		fprintf(stderr, "%s: %s\n", argv[1], strerror(-ret));
	unmount_image(fs, &dev);
	return ret < 0 ? 1 : 0;
}

/* ---- file hashing ---- */

static int hash_file(btrfsro_fs *fs, const struct btrfsro_inode *ino,
		     char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1])
{
	unsigned char digest[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256_CTX ctx;
	uint64_t offset = 0;
	char *buf = malloc(IO_CHUNK);
	int ret = 0;

	CC_SHA256_Init(&ctx);
	for (;;) {
		ssize_t got = btrfsro_read(fs, ino, offset, buf, IO_CHUNK);

		if (got < 0) {
			ret = (int)got;
			break;
		}
		if (got == 0)
			break;
		CC_SHA256_Update(&ctx, buf, (CC_LONG)got);
		offset += got;
	}
	CC_SHA256_Final(digest, &ctx);
	free(buf);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
		sprintf(hex + i * 2, "%02x", digest[i]);
	return ret;
}

/* ---- manifest ---- */

/*
 * One line per entry, tab separated:
 *   path type mode uid gid nlink size mtime ino content
 * content is the sha256 for regular files, the target for symlinks and "-"
 * otherwise. Same format as tests/linux-manifest.py.
 */
static int manifest_one(btrfsro_fs *fs, const struct btrfsro_inode *ino,
			const char *path, int *errors)
{
	struct btrfsro_stat st;
	char content[4096 + 1];
	int ret;

	ret = btrfsro_stat(fs, ino, &st);
	if (ret < 0) {
		fprintf(stderr, "%s: stat: %s\n", path, strerror(-ret));
		(*errors)++;
		return ret;
	}
	strcpy(content, "-");
	if (S_ISREG(st.mode)) {
		ret = hash_file(fs, ino, content);
		if (ret < 0) {
			fprintf(stderr, "%s: read: %s\n", path, strerror(-ret));
			(*errors)++;
			strcpy(content, "READ-ERROR");
		}
	} else if (S_ISLNK(st.mode)) {
		ret = btrfsro_readlink(fs, ino, content, sizeof(content));
		if (ret < 0) {
			fprintf(stderr, "%s: readlink: %s\n", path,
				strerror(-ret));
			(*errors)++;
			strcpy(content, "READLINK-ERROR");
		}
	}
	printf("%s\t%c\t%04o\t%u\t%u\t%u\t%" PRIu64 "\t%" PRId64 ".%09u\t%"
	       PRIu64 "\t%s\n", path, type_char(st.mode), st.mode & 07777,
	       st.uid, st.gid, st.nlink, S_ISDIR(st.mode) ? 0 : st.size,
	       st.mtime.sec, st.mtime.nsec, st.ino, content);
	return 0;
}

static void manifest_walk(btrfsro_fs *fs, const struct btrfsro_inode *dir,
			  const char *path, int *errors)
{
	struct dirent_list list = {};
	int ret;

	ret = read_dir(fs, dir, &list, true);
	if (ret < 0) {
		fprintf(stderr, "%s: readdir: %s\n", path, strerror(-ret));
		(*errors)++;
	}
	for (size_t i = 0; i < list.nr; i++) {
		char *sub = join_path(path, list.ents[i].name);

		manifest_one(fs, &list.ents[i].ino, sub, errors);
		if (list.ents[i].ino.type == BTRFSRO_FT_DIR)
			manifest_walk(fs, &list.ents[i].ino, sub, errors);
		free(sub);
	}
	free_dirent_list(&list);
}

static int cmd_manifest(int argc, char **argv)
{
	struct btrfsro_inode ino;
	struct file_dev dev;
	const char *path = "/";
	btrfsro_fs *fs;
	int errors = 0;
	int ret;

	if (argc < 1 || argc > 2) {
		fprintf(stderr, "usage: btrfsro manifest <image> [path]\n");
		return 2;
	}
	if (argc == 2)
		path = argv[1];
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, path, &ino);
	if (ret == 0) {
		manifest_one(fs, &ino, ".", &errors);
		if (ino.type == BTRFSRO_FT_DIR)
			manifest_walk(fs, &ino, ".", &errors);
	}
	unmount_image(fs, &dev);
	if (errors)
		fprintf(stderr, "%d errors\n", errors);
	return (ret < 0 || errors) ? 1 : 0;
}

/* ---- extract ---- */

struct hardlink {
	uint64_t subvol;
	uint64_t ino;
	char *path;
};

struct extract_ctx {
	btrfsro_fs *fs;
	struct hardlink *links;
	size_t nr_links;
	size_t max_links;
	int errors;
	unsigned long files;
	unsigned long dirs;
	unsigned long symlinks;
	unsigned long hardlinks;
	uint64_t bytes;
};

static const char *find_hardlink(struct extract_ctx *ctx,
				 const struct btrfsro_stat *st)
{
	for (size_t i = 0; i < ctx->nr_links; i++)
		if (ctx->links[i].ino == st->ino &&
		    ctx->links[i].subvol == st->subvol)
			return ctx->links[i].path;
	return NULL;
}

static void add_hardlink(struct extract_ctx *ctx, const struct btrfsro_stat *st,
			 const char *path)
{
	if (ctx->nr_links == ctx->max_links) {
		ctx->max_links = ctx->max_links ? ctx->max_links * 2 : 256;
		ctx->links = realloc(ctx->links,
				     ctx->max_links * sizeof(*ctx->links));
	}
	ctx->links[ctx->nr_links].subvol = st->subvol;
	ctx->links[ctx->nr_links].ino = st->ino;
	ctx->links[ctx->nr_links].path = strdup(path);
	ctx->nr_links++;
}

static void set_times(const char *dst, const struct btrfsro_stat *st)
{
	struct timespec ts[2] = {
		{ .tv_sec = st->atime.sec, .tv_nsec = st->atime.nsec },
		{ .tv_sec = st->mtime.sec, .tv_nsec = st->mtime.nsec },
	};

	utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW);
}

static int extract_file(struct extract_ctx *ctx,
			const struct btrfsro_inode *ino,
			const struct btrfsro_stat *st, const char *dst)
{
	uint64_t offset = 0;
	char *buf;
	int fd;
	int ret = 0;

	fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", dst, strerror(errno));
		return -errno;
	}
	buf = malloc(IO_CHUNK);
	for (;;) {
		ssize_t got = btrfsro_read(ctx->fs, ino, offset, buf, IO_CHUNK);

		if (got < 0) {
			fprintf(stderr, "%s: read at %" PRIu64 ": %s\n", dst,
				offset, strerror((int)-got));
			ret = (int)got;
			break;
		}
		if (got == 0)
			break;
		if (write(fd, buf, got) != got) {
			fprintf(stderr, "%s: write: %s\n", dst,
				strerror(errno));
			ret = -errno;
			break;
		}
		offset += got;
	}
	free(buf);
	fchmod(fd, st->mode & 07777);
	close(fd);
	ctx->bytes += offset;
	return ret;
}

static void extract_tree(struct extract_ctx *ctx,
			 const struct btrfsro_inode *dir,
			 const struct btrfsro_stat *dir_st, const char *dst);

static void extract_entry(struct extract_ctx *ctx,
			  const struct btrfsro_inode *ino, const char *dst)
{
	struct btrfsro_stat st;
	const char *link_to;
	int ret;

	ret = btrfsro_stat(ctx->fs, ino, &st);
	if (ret < 0) {
		fprintf(stderr, "%s: stat: %s\n", dst, strerror(-ret));
		ctx->errors++;
		return;
	}
	if (!S_ISDIR(st.mode) && st.nlink > 1) {
		link_to = find_hardlink(ctx, &st);
		if (link_to) {
			if (link(link_to, dst) < 0) {
				fprintf(stderr, "%s: link: %s\n", dst,
					strerror(errno));
				ctx->errors++;
			}
			ctx->hardlinks++;
			return;
		}
		add_hardlink(ctx, &st, dst);
	}

	switch (st.mode & S_IFMT) {
	case S_IFDIR:
		if (mkdir(dst, 0700) < 0 && errno != EEXIST) {
			fprintf(stderr, "%s: mkdir: %s\n", dst, strerror(errno));
			ctx->errors++;
			return;
		}
		ctx->dirs++;
		extract_tree(ctx, ino, &st, dst);
		return;
	case S_IFREG:
		if (extract_file(ctx, ino, &st, dst) < 0)
			ctx->errors++;
		ctx->files++;
		break;
	case S_IFLNK: {
		char target[4096];

		ret = btrfsro_readlink(ctx->fs, ino, target, sizeof(target));
		if (ret < 0 || symlink(target, dst) < 0) {
			fprintf(stderr, "%s: symlink: %s\n", dst,
				ret < 0 ? strerror(-ret) : strerror(errno));
			ctx->errors++;
			return;
		}
		ctx->symlinks++;
		break;
	}
	case S_IFIFO:
		if (mkfifo(dst, st.mode & 07777) < 0) {
			fprintf(stderr, "%s: mkfifo: %s\n", dst,
				strerror(errno));
			ctx->errors++;
		}
		break;
	default:
		fprintf(stderr, "%s: skipping special file (mode %06o)\n", dst,
			st.mode);
		return;
	}
	set_times(dst, &st);
}

static void extract_tree(struct extract_ctx *ctx,
			 const struct btrfsro_inode *dir,
			 const struct btrfsro_stat *dir_st, const char *dst)
{
	struct dirent_list list = {};
	int ret;

	ret = read_dir(ctx->fs, dir, &list, false);
	if (ret < 0) {
		fprintf(stderr, "%s: readdir: %s\n", dst, strerror(-ret));
		ctx->errors++;
	}
	for (size_t i = 0; i < list.nr; i++) {
		char *sub = join_path(dst, list.ents[i].name);

		extract_entry(ctx, &list.ents[i].ino, sub);
		free(sub);
	}
	free_dirent_list(&list);
	chmod(dst, dir_st->mode & 07777);
	set_times(dst, dir_st);
}

static int cmd_extract(int argc, char **argv)
{
	struct extract_ctx ctx = {};
	struct btrfsro_inode ino;
	struct btrfsro_stat st;
	struct file_dev dev;
	char *dst;
	const char *base;
	int ret;

	if (argc != 3) {
		fprintf(stderr, "usage: btrfsro extract <image> <path> <destdir>\n");
		return 2;
	}
	ctx.fs = mount_image(argv[0], &dev);
	if (!ctx.fs)
		return 1;
	ret = resolve_or_die(ctx.fs, argv[1], &ino);
	if (ret < 0)
		goto out;
	ret = btrfsro_stat(ctx.fs, &ino, &st);
	if (ret < 0)
		goto out;
	if (mkdir(argv[2], 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "%s: %s\n", argv[2], strerror(errno));
		ret = -errno;
		goto out;
	}
	if (S_ISDIR(st.mode)) {
		/* Extract the contents of the directory into destdir */
		dst = strdup(argv[2]);
		extract_tree(&ctx, &ino, &st, dst);
	} else {
		base = strrchr(argv[1], '/');
		base = base ? base + 1 : argv[1];
		dst = join_path(argv[2], base);
		extract_entry(&ctx, &ino, dst);
	}
	free(dst);
	fprintf(stderr, "extracted %lu files (%" PRIu64 " bytes), %lu dirs, "
		"%lu symlinks, %lu hardlinks, %d errors\n", ctx.files,
		ctx.bytes, ctx.dirs, ctx.symlinks, ctx.hardlinks, ctx.errors);
out:
	for (size_t i = 0; i < ctx.nr_links; i++)
		free(ctx.links[i].path);
	free(ctx.links);
	unmount_image(ctx.fs, &dev);
	return (ret < 0 || ctx.errors) ? 1 : 0;
}

/* ---- extents ---- */

struct extents_ctx {
	unsigned long count;
	unsigned long compressed;
	uint64_t ram;
	uint64_t disk;
	bool quiet;
};

static int extent_cb(void *ctx, const struct btrfsro_extent *ext)
{
	static const char *types[] = { "inline", "regular", "prealloc" };
	static const char *comp[] = { "none", "zlib", "lzo", "zstd" };
	struct extents_ctx *ec = ctx;

	ec->count++;
	ec->ram += ext->length;
	ec->disk += ext->disk_bytes;
	if (ext->compression)
		ec->compressed++;
	if (!ec->quiet)
		printf("%12" PRIu64 " %10" PRIu64 " %-8s %-4s disk %14" PRIu64
		       " len %8" PRIu64 " off %" PRIu64 "\n", ext->file_offset,
		       ext->length, ext->type < 3 ? types[ext->type] : "?",
		       ext->compression < 4 ? comp[ext->compression] : "?",
		       ext->disk_bytenr, ext->disk_bytes, ext->extent_offset);
	return 0;
}

static int cmd_extents(int argc, char **argv)
{
	struct extents_ctx ec = {};
	struct btrfsro_inode ino;
	struct file_dev dev;
	btrfsro_fs *fs;
	int ret;

	if (argc == 3 && !strcmp(argv[0], "-s")) {
		ec.quiet = true;
		argc--;
		argv++;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: btrfsro extents [-s] <image> <path>\n");
		return 2;
	}
	fs = mount_image(argv[0], &dev);
	if (!fs)
		return 1;
	ret = resolve_or_die(fs, argv[1], &ino);
	if (ret == 0)
		ret = btrfsro_extents(fs, &ino, extent_cb, &ec);
	if (ret == 0)
		printf("%lu extents, %lu compressed, %" PRIu64 " bytes in %"
		       PRIu64 " bytes on disk\n", ec.count, ec.compressed,
		       ec.ram, ec.disk);
	else
		fprintf(stderr, "%s: %s\n", argv[1], strerror(-ret));
	unmount_image(fs, &dev);
	return ret < 0 ? 1 : 0;
}

/* ---- main ---- */

static void usage(void)
{
	fprintf(stderr,
"usage: btrfsro [-v] [--no-verify] <command> [args]\n"
"\n"
"commands:\n"
"  info     <image>                      show superblock information\n"
"  ls       [-l] [-R] <image> [path]     list a directory\n"
"  cat      [-o off] [-n len] <image> <path>  write file contents to stdout\n"
"  stat     <image> <path>               show inode details\n"
"  readlink <image> <path>               print symlink target\n"
"  xattr    <image> <path>               list extended attributes\n"
"  extract  <image> <path> <destdir>     copy a file or tree out\n"
"  manifest <image> [path]               print path/metadata/sha256 list\n"
"  extents  [-s] <image> <path>          list file extents (-s: summary)\n");
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		int (*fn)(int, char **);
	} cmds[] = {
		{ "info", cmd_info },
		{ "ls", cmd_ls },
		{ "cat", cmd_cat },
		{ "stat", cmd_stat },
		{ "readlink", cmd_readlink },
		{ "xattr", cmd_xattr },
		{ "extract", cmd_extract },
		{ "manifest", cmd_manifest },
		{ "extents", cmd_extents },
	};
	int i = 1;

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-v")) {
			opt_verbose = true;
		} else if (!strcmp(argv[i], "--no-verify")) {
			opt_no_verify = true;
		} else {
			usage();
			return 2;
		}
	}
	if (i >= argc) {
		usage();
		return 2;
	}
	btrfsro_set_log_handler(NULL, NULL, opt_verbose ? BTRFSRO_LOG_DEBUG :
				BTRFSRO_LOG_WARNING);
	for (size_t c = 0; c < sizeof(cmds) / sizeof(cmds[0]); c++)
		if (!strcmp(argv[i], cmds[c].name))
			return cmds[c].fn(argc - i - 1, argv + i + 1);
	usage();
	return 2;
}
