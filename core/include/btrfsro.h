// SPDX-License-Identifier: MIT
/*
 * libbtrfsro - read-only btrfs access library.
 *
 * Public API. All functions returning int use 0 (or a non-negative count) for
 * success and a negative errno value for failure.
 *
 * The library never touches files or devices on its own: the caller supplies
 * a read callback, so the same code runs on top of pread() in the command line
 * tools and on top of an FSKit block device resource in the macOS extension.
 *
 * Thread safety: after a successful btrfsro_mount() all calls on the same
 * filesystem handle may be made concurrently from multiple threads.
 */

#ifndef BTRFSRO_H
#define BTRFSRO_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log levels */
enum {
	BTRFSRO_LOG_ERROR = 0,
	BTRFSRO_LOG_WARNING = 1,
	BTRFSRO_LOG_INFO = 2,
	BTRFSRO_LOG_DEBUG = 3,
};

typedef void (*btrfsro_log_fn)(void *ctx, int level, const char *msg);

/*
 * Route library messages to @fn (NULL means stderr). Messages above
 * @max_level are dropped. Not thread safe, call it before mounting.
 */
void btrfsro_set_log_handler(btrfsro_log_fn fn, void *ctx, int max_level);

/*
 * Device read callback. Must read exactly @len bytes at byte @offset of the
 * device into @buf. Return 0 on success, negative errno on failure.
 * Called concurrently from multiple threads.
 */
typedef int (*btrfsro_read_fn)(void *ctx, void *buf, uint64_t offset,
			       size_t len);

struct btrfsro_io {
	btrfsro_read_fn read;
	void *ctx;
	/* Size of the device in bytes, 0 if unknown */
	uint64_t size;
};

typedef struct btrfs_fs_info btrfsro_fs;

/* File types, same values as the on-disk BTRFS_FT_* numbers */
enum {
	BTRFSRO_FT_UNKNOWN = 0,
	BTRFSRO_FT_REG = 1,
	BTRFSRO_FT_DIR = 2,
	BTRFSRO_FT_CHRDEV = 3,
	BTRFSRO_FT_BLKDEV = 4,
	BTRFSRO_FT_FIFO = 5,
	BTRFSRO_FT_SOCK = 6,
	BTRFSRO_FT_SYMLINK = 7,
};

/*
 * An inode is identified by the subvolume tree it lives in and its inode
 * number inside that tree.
 */
struct btrfsro_inode {
	uint64_t subvol;
	uint64_t ino;
	uint8_t type;	/* BTRFSRO_FT_* */
};

struct btrfsro_volinfo {
	uint8_t fsid[16];
	char label[256];
	uint64_t total_bytes;
	uint64_t bytes_used;
	uint64_t generation;
	uint64_t num_devices;
	uint64_t incompat_flags;
	uint64_t compat_ro_flags;
	uint32_t sectorsize;
	uint32_t nodesize;
	uint16_t csum_type;
	/* Subvolume id of the default subvolume (mounted as root) */
	uint64_t default_subvol;
};

struct btrfsro_timespec {
	int64_t sec;
	uint32_t nsec;
};

struct btrfsro_stat {
	uint64_t subvol;
	uint64_t ino;
	uint32_t mode;		/* POSIX mode including S_IFMT bits */
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t rdev;		/* Linux encoded device number */
	uint64_t size;
	uint64_t nbytes;	/* Bytes allocated on disk */
	uint64_t generation;
	uint64_t flags;		/* BTRFS_INODE_* flags */
	struct btrfsro_timespec atime;
	struct btrfsro_timespec mtime;
	struct btrfsro_timespec ctime;
	struct btrfsro_timespec otime;	/* Creation time */
};

/*
 * Check whether the device carries a btrfs filesystem (superblock magic and
 * checksum only). @info may be NULL.
 * Return 0 if it is btrfs, -EINVAL if not, other negative errno on failure.
 */
int btrfsro_probe(const struct btrfsro_io *io, struct btrfsro_volinfo *info);

int btrfsro_mount(const struct btrfsro_io *io, btrfsro_fs **fs_ret);
void btrfsro_unmount(btrfsro_fs *fs);

void btrfsro_get_volinfo(btrfsro_fs *fs, struct btrfsro_volinfo *info);

/* Root directory of the default subvolume */
void btrfsro_root(btrfsro_fs *fs, struct btrfsro_inode *root);

/* Lookup one name (no '/') in directory @dir. */
int btrfsro_lookup(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		   const char *name, size_t name_len,
		   struct btrfsro_inode *out);

/*
 * Resolve an absolute path from the root. Symlinks are not followed, "." and
 * ".." are not interpreted.
 */
int btrfsro_resolve(btrfsro_fs *fs, const char *path,
		    struct btrfsro_inode *out);

/*
 * Find the parent directory of directory @dir. The root of a subvolume
 * returns the directory containing the subvolume, the filesystem root returns
 * itself.
 */
int btrfsro_parent(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		   struct btrfsro_inode *out);

int btrfsro_stat(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		 struct btrfsro_stat *st);

/*
 * Directory iteration callback. @index is the directory index of the entry;
 * to resume after it pass @index + 1 as the next start cookie.
 * Return non-zero to stop the iteration.
 */
typedef int (*btrfsro_dir_fn)(void *ctx, const char *name, size_t name_len,
			      const struct btrfsro_inode *entry,
			      uint64_t index);

/*
 * Iterate the entries of @dir whose index is >= @start. "." and ".." are not
 * reported. Return 0 when finished or stopped by the callback.
 */
int btrfsro_readdir(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		    uint64_t start, btrfsro_dir_fn fn, void *ctx);

/*
 * Read up to @len bytes at @offset from a regular file. Any offset and
 * length are allowed. Return the number of bytes read (0 at or past EOF).
 */
ssize_t btrfsro_read(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		     uint64_t offset, void *buf, size_t len);

/*
 * Read the target of a symlink into @buf, NUL terminated.
 * Return the length of the target (without the NUL).
 */
int btrfsro_readlink(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		     char *buf, size_t size);

/*
 * Extended attributes. btrfsro_listxattr() stores the NUL separated names
 * into @buf and returns the total length; with @buf == NULL it only returns
 * the required size. btrfsro_getxattr() returns the value length, and
 * -ENODATA if the attribute does not exist. -ERANGE if @size is too small
 * (with @buf != NULL).
 */
ssize_t btrfsro_listxattr(btrfsro_fs *fs, const struct btrfsro_inode *inode,
			  char *buf, size_t size);
ssize_t btrfsro_getxattr(btrfsro_fs *fs, const struct btrfsro_inode *inode,
			 const char *name, void *buf, size_t size);

/* One file extent, as reported by btrfsro_extents() */
struct btrfsro_extent {
	uint64_t file_offset;
	uint64_t length;	/* Bytes of the file covered */
	uint64_t disk_bytenr;	/* Logical address, 0 for holes and inline */
	uint64_t disk_bytes;	/* On-disk (possibly compressed) size */
	uint64_t extent_offset;	/* Offset into the (decompressed) extent */
	uint8_t type;		/* 0 inline, 1 regular, 2 prealloc */
	uint8_t compression;	/* 0 none, 1 zlib, 2 lzo, 3 zstd */
};

typedef int (*btrfsro_extent_fn)(void *ctx, const struct btrfsro_extent *ext);

/* Report the file extent items of a regular file in file offset order */
int btrfsro_extents(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		    btrfsro_extent_fn fn, void *ctx);

/*
 * Data checksum verification of file contents read with btrfsro_read() (on
 * by default). Metadata checksums are always verified.
 */
void btrfsro_set_verify_data(btrfsro_fs *fs, int enable);

#ifdef __cplusplus
}
#endif

#endif
