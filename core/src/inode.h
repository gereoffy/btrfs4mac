// SPDX-License-Identifier: MIT

#ifndef BTRFS_FUSE_INODE_H
#define BTRFS_FUSE_INODE_H

#include "compat.h"
#include "ondisk_format.h"
#include "ctree.h"
#include "metadata.h"

/*
 * Represent one inode in a btrfs.
 *
 * Since each subvolume is a separate inode space, we have can same inode
 * numbers in different subvolumes.
 *
 * Thus in btrfs to locate one inode, we need (subvolid, inode), not just inode
 * number.
 */
struct btrfs_inode {
	struct btrfs_root *root;
	u64 ino;

	/* File type, indicated using BTRFS_FT_* numbers */
	u8 file_type;
};

/*
 * Lookup one name for @dir.
 *
 * NOTE: @name should not contain '/', thus it's really just one name, not
 * a complete path.
 */
int btrfs_lookup_one_name(struct btrfs_fs_info *fs_info,
			  struct btrfs_inode *dir, const char *name,
			  size_t name_len, struct btrfs_inode *inode_ret);

/*
 * Resolve a full path.
 *
 * NOTE: the path should not contain soft link (or ".." or "."), and should be
 * absolute path (starts with '/').
 */
int btrfs_resolve_path(struct btrfs_fs_info *fs_info,
		       const char *path, size_t path_len,
		       struct btrfs_inode *inode_ret);

/* Find the parent directory of directory @dir */
int btrfs_lookup_parent(struct btrfs_fs_info *fs_info, struct btrfs_inode *dir,
			struct btrfs_inode *parent_ret);

/*
 * Read the softlink destination into @output, NUL terminated.
 *
 * Return >0 for the size of the content (not including the tailing '\0')
 * Return <0 for error.
 */
int btrfs_read_link(struct btrfs_fs_info *fs_info,
		    struct btrfs_inode *inode, char *output,
		    size_t output_size);

struct btrfs_iterate_dir_ctrl {
	struct btrfs_path path;
	struct btrfs_inode dir;
	struct btrfs_key_range range;
};

/*
 * Interafaces to iterate one dir.
 *
 * The common usage would be:
 *
 *	ret = btrfs_iterate_dir_start(fs_info, &ctrl, dir, 0);
 *	while (ret == 0) {
 *		ret = btrfs_iterate_dir_get_inode(...);
 *		...
 *		ret = btrfs_iterate_dir_next(fs_info, &ctrl);
 *	}
 *	btrfs_iterate_dir_end(fs_info, &ctrl);
 *
 * btrfs_iterate_dir_start() returns -ENOENT for an empty range.
 */
int btrfs_iterate_dir_start(struct btrfs_fs_info *fs_info,
			    struct btrfs_iterate_dir_ctrl *ctrl,
			    const struct btrfs_inode *dir, u64 start_index);

/* @name must have room for BTRFS_NAME_LEN bytes */
int btrfs_iterate_dir_get_inode(struct btrfs_fs_info *fs_info,
				struct btrfs_iterate_dir_ctrl *ctrl,
				struct btrfs_inode *entry,
				u64 *index_ret, char *name, size_t *name_len);

static inline int btrfs_iterate_dir_next(struct btrfs_fs_info *fs_info,
					 struct btrfs_iterate_dir_ctrl *ctrl)
{
	return btrfs_search_keys_next(&ctrl->path, &ctrl->range);
}

static inline void btrfs_iterate_dir_end(struct btrfs_fs_info *fs_info,
					 struct btrfs_iterate_dir_ctrl *ctrl)
{
	btrfs_release_path(&ctrl->path);
}

int btrfs_stat(struct btrfs_fs_info *fs_info, struct btrfs_inode *inode,
	       struct btrfsro_stat *st);

ssize_t btrfs_listxattr(struct btrfs_fs_info *fs_info,
			struct btrfs_inode *inode, char *buf, size_t size);
ssize_t btrfs_getxattr(struct btrfs_fs_info *fs_info,
		       struct btrfs_inode *inode, const char *name,
		       void *buf, size_t size);
#endif
