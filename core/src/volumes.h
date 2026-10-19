// SPDX-License-Identifier: MIT

#ifndef BTRFS_FUSE_VOLUMES_H
#define BTRFS_FUSE_VOLUMES_H

#include "compat.h"
#include "ondisk_format.h"

struct btrfs_fs_info;

struct btrfs_io_stripe {
	u64 devid;
	u64 physical;
};

/* One chunk: maps [logical, logical + length) to device ranges */
struct btrfs_chunk_map {
	u64 logical;
	u64 length;
	u64 stripe_len;
	u64 flags;
	u16 sub_stripes;

	/* Number of copies we can read from */
	int num_mirrors;

	int num_stripes;
	struct btrfs_io_stripe stripes[];
};

static inline int btrfs_chunk_map_size(int num_stripes)
{
	return sizeof(struct btrfs_chunk_map) +
		num_stripes * sizeof(struct btrfs_io_stripe);
}

/* Read @len bytes at physical @offset of our device */
int btrfs_read_from_disk(struct btrfs_fs_info *fs_info, char *buf, u64 offset,
			 u32 len);

int btrfs_read_sys_chunk_array(struct btrfs_fs_info *fs_info);
int btrfs_read_chunk_tree(struct btrfs_fs_info *fs_info);
void btrfs_free_chunk_maps(struct btrfs_fs_info *fs_info);

/*
 * Return >0 for the max mirror number of the chunk containing @logical.
 * Return <0 for error.
 */
int btrfs_num_copies(struct btrfs_fs_info *fs_info, u64 logical);

/*
 * Read from logical bytenr @logical with @mirror_nr as mirror number.
 *
 * This doesn't have any validation like data checksum nor metadata checksum.
 * The read never crosses a chunk boundary.
 *
 * Return the number of bytes read from @logical.
 * Return <0 for error.
 */
int btrfs_read_logical(struct btrfs_fs_info *fs_info, char *buf, size_t size,
			u64 logical, int mirror_nr);

#endif
