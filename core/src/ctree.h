// SPDX-License-Identifier: MIT

#ifndef BTRFS_FUSE_CTREE_H
#define BTRFS_FUSE_CTREE_H

#include <pthread.h>
#include "accessors.h"
#include "list.h"
#include "../include/btrfsro.h"

#define BTRFS_UUID_UNPARSED_SIZE 37

struct btrfs_root {
	struct extent_buffer *node;
	struct btrfs_key root_key;
	struct btrfs_fs_info *fs_info;
	struct list_head list;

	/*
	 * Indicates the first inode number, which is also the inode for the
	 * subvolume root.
	 */
	u64 root_dirid;
};

/* Number of buckets of the tree block hash table, power of 2 */
#define BTRFS_EB_HASH_BUCKETS	4096

/* Tree blocks kept in memory after their last user released them */
#define BTRFS_EB_CACHE_MAX	2048

/* Represents a btrfs filesystem */
struct btrfs_fs_info {
	u8 chunk_tree_uuid[BTRFS_UUID_SIZE];
	u8 fsid[BTRFS_UUID_SIZE];
	/* fsid used in tree block headers (differs with METADATA_UUID) */
	u8 metadata_uuid[BTRFS_UUID_SIZE];

	struct btrfsro_io io;

	struct btrfs_root *tree_root;
	struct btrfs_root *default_root;
	struct btrfs_root *chunk_root;
	struct btrfs_root *csum_root;

	/* Records all subvolume trees that are in use, protected by root_lock */
	struct list_head subvols;
	pthread_mutex_t root_lock;

	/* Records logical->physical mappings, sorted by logical */
	struct btrfs_chunk_map **chunk_maps;
	int nr_chunk_maps;
	int max_chunk_maps;

	/* Tree block cache, protected by eb_lock */
	struct extent_buffer *eb_hash[BTRFS_EB_HASH_BUCKETS];
	/* Unreferenced but cached tree blocks, least recently used first */
	struct list_head eb_lru;
	int nr_eb_lru;
	pthread_mutex_t eb_lock;

	/* Cached generation, the same as superblock::generation */
	u64 generation;

	/* Our own device */
	u64 devid;
	u8 dev_uuid[BTRFS_UUID_SIZE];

	/* Cached basic sizes */
	u32 nodesize;
	u32 sectorsize;
	u16 csum_size;
	u16 csum_type;
	bool verify_data;
	struct btrfs_super_block super_copy;
};

#endif
