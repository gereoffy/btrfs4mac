// SPDX-License-Identifier: MIT
/*
 * Logical to physical address mapping.
 *
 * Only single device filesystems are supported. Within that device we can
 * read SINGLE, DUP, RAID0 and the RAID1 family (all copies on this device).
 */

#include <sys/param.h>
#include "compat.h"
#include "volumes.h"
#include "messages.h"
#include "metadata.h"
#include "ctree.h"

int btrfs_read_from_disk(struct btrfs_fs_info *fs_info, char *buf, u64 offset,
			 u32 len)
{
	int ret;

	if (fs_info->io.size && (offset > fs_info->io.size ||
				 len > fs_info->io.size - offset)) {
		error("read beyond device end, offset %llu len %u device size %llu",
		      offset, len, fs_info->io.size);
		return -EIO;
	}
	ret = fs_info->io.read(fs_info->io.ctx, buf, offset, len);
	if (ret < 0)
		return ret;
	return len;
}

static inline int btrfs_chunk_item_size(int num_stripes)
{
	return sizeof(struct btrfs_chunk) +
		num_stripes * sizeof(struct btrfs_stripe);
}

/* Index of the first chunk map whose logical is > @logical */
static int chunk_map_upper_bound(struct btrfs_fs_info *fs_info, u64 logical)
{
	int low = 0;
	int high = fs_info->nr_chunk_maps;

	while (low < high) {
		int mid = low + (high - low) / 2;

		if (fs_info->chunk_maps[mid]->logical <= logical)
			low = mid + 1;
		else
			high = mid;
	}
	return low;
}

static struct btrfs_chunk_map *lookup_chunk_map(struct btrfs_fs_info *fs_info,
						u64 logical)
{
	int idx = chunk_map_upper_bound(fs_info, logical);
	struct btrfs_chunk_map *map;

	if (idx == 0)
		return NULL;
	map = fs_info->chunk_maps[idx - 1];
	if (logical >= map->logical && logical - map->logical < map->length)
		return map;
	return NULL;
}

void btrfs_free_chunk_maps(struct btrfs_fs_info *fs_info)
{
	for (int i = 0; i < fs_info->nr_chunk_maps; i++)
		free(fs_info->chunk_maps[i]);
	free(fs_info->chunk_maps);
	fs_info->chunk_maps = NULL;
	fs_info->nr_chunk_maps = 0;
	fs_info->max_chunk_maps = 0;
}

static int insert_chunk_map(struct btrfs_fs_info *fs_info,
			    struct btrfs_chunk_map *map)
{
	int idx = chunk_map_upper_bound(fs_info, map->logical);

	/* Exact duplicate (sys chunk array and chunk tree overlap) */
	if (idx > 0) {
		struct btrfs_chunk_map *prev = fs_info->chunk_maps[idx - 1];

		if (prev->logical == map->logical &&
		    prev->length == map->length &&
		    prev->num_stripes == map->num_stripes) {
			free(map);
			return 0;
		}
		if (prev->logical + prev->length > map->logical) {
			error("chunk %llu overlaps chunk %llu",
			      map->logical, prev->logical);
			free(map);
			return -EUCLEAN;
		}
	}
	if (idx < fs_info->nr_chunk_maps &&
	    map->logical + map->length > fs_info->chunk_maps[idx]->logical) {
		error("chunk %llu overlaps chunk %llu",
		      map->logical, fs_info->chunk_maps[idx]->logical);
		free(map);
		return -EUCLEAN;
	}

	if (fs_info->nr_chunk_maps == fs_info->max_chunk_maps) {
		int new_max = fs_info->max_chunk_maps ?
			      fs_info->max_chunk_maps * 2 : 64;
		struct btrfs_chunk_map **new_maps;

		new_maps = realloc(fs_info->chunk_maps,
				   new_max * sizeof(*new_maps));
		if (!new_maps) {
			free(map);
			return -ENOMEM;
		}
		fs_info->chunk_maps = new_maps;
		fs_info->max_chunk_maps = new_max;
	}
	memmove(&fs_info->chunk_maps[idx + 1], &fs_info->chunk_maps[idx],
		(fs_info->nr_chunk_maps - idx) * sizeof(*fs_info->chunk_maps));
	fs_info->chunk_maps[idx] = map;
	fs_info->nr_chunk_maps++;
	return 0;
}

/*
 * Add a chunk map to @fs_info.
 *
 * @logical:	 Logical bytenr of the chunk
 * @stack_chunk: The chunk item (in memory, little endian)
 * @max_size:	 The maximum chunk item size, this is to co-operate with
 *		 superblock sys_chunk_array which doesn't have item_size
 */
static int add_chunk_map(struct btrfs_fs_info *fs_info, u64 logical,
			 struct btrfs_chunk *stack_chunk, u32 max_size)
{
	struct btrfs_chunk_map *map;
	u64 length;
	u64 profile;
	int num_stripes;
	int i;

	/* Sanity check to ensure we don't go beyond @max_size */
	if (btrfs_chunk_item_size(1) > max_size) {
		error("invalid chunk size, expected max %u has minimal %u",
			max_size, btrfs_chunk_item_size(1));
		return -EUCLEAN;
	}
	num_stripes = btrfs_stack_chunk_num_stripes(stack_chunk);
	if (num_stripes == 0 || btrfs_chunk_item_size(num_stripes) > max_size) {
		error("invalid chunk at %llu: %d stripes, item size %u",
			logical, num_stripes, max_size);
		return -EUCLEAN;
	}
	length = btrfs_stack_chunk_length(stack_chunk);
	if (length == 0 || logical + length < logical ||
	    !IS_ALIGNED(logical, fs_info->sectorsize) ||
	    !IS_ALIGNED(length, fs_info->sectorsize)) {
		error("invalid chunk at %llu length %llu", logical, length);
		return -EUCLEAN;
	}

	map = calloc(1, btrfs_chunk_map_size(num_stripes));
	if (!map)
		return -ENOMEM;
	map->length = length;
	map->logical = logical;
	map->stripe_len = btrfs_stack_chunk_stripe_len(stack_chunk);
	map->sub_stripes = btrfs_stack_chunk_sub_stripes(stack_chunk);
	map->flags = btrfs_stack_chunk_type(stack_chunk);
	map->num_stripes = num_stripes;

	for (i = 0; i < num_stripes; i++) {
		map->stripes[i].devid =
			btrfs_stack_stripe_devid(&stack_chunk->stripes[i]);
		map->stripes[i].physical =
			btrfs_stack_stripe_offset(&stack_chunk->stripes[i]);
	}

	profile = map->flags & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	switch (profile) {
	case 0:
	case BTRFS_BLOCK_GROUP_DUP:
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID1C3:
	case BTRFS_BLOCK_GROUP_RAID1C4:
		map->num_mirrors = num_stripes;
		break;
	case BTRFS_BLOCK_GROUP_RAID0:
		map->sub_stripes = 1;
		map->num_mirrors = 1;
		break;
	case BTRFS_BLOCK_GROUP_RAID10:
		if (map->sub_stripes < 1 || num_stripes % map->sub_stripes) {
			error("invalid RAID10 chunk at %llu", logical);
			free(map);
			return -EUCLEAN;
		}
		map->num_mirrors = map->sub_stripes;
		break;
	default:
		/* RAID5/6: we don't do parity reads, treat as unreadable */
		map->num_mirrors = 0;
		break;
	}
	if ((profile == BTRFS_BLOCK_GROUP_RAID0 ||
	     profile == BTRFS_BLOCK_GROUP_RAID10) &&
	    (!is_power_of_2(map->stripe_len) ||
	     map->stripe_len < fs_info->sectorsize)) {
		error("invalid stripe length %llu for chunk %llu",
		      map->stripe_len, logical);
		free(map);
		return -EUCLEAN;
	}

	return insert_chunk_map(fs_info, map);
}

int btrfs_read_sys_chunk_array(struct btrfs_fs_info *fs_info)
{
	struct btrfs_super_block *sb = &fs_info->super_copy;
	u32 sys_chunk_size = btrfs_super_sys_array_size(sb);
	u32 cur = 0;

	while (cur < sys_chunk_size) {
		struct btrfs_disk_key *disk_key;
		struct btrfs_chunk *chunk;
		u16 num_stripes;
		int ret;
		/*
		 * Make sure we have enough space to contain one disk_key +
		 * one chunk.
		 */
		if (sys_chunk_size - cur < sizeof(struct btrfs_disk_key) +
		    btrfs_chunk_item_size(1)) {
			error(
		"invalid sys_chunk_size, has %u bytes left expected minimal %zu",
				sys_chunk_size - cur,
				sizeof(struct btrfs_disk_key) +
				btrfs_chunk_item_size(1));
			return -EUCLEAN;
		}
		disk_key = (struct btrfs_disk_key *)(sb->sys_chunk_array + cur);
		if (btrfs_disk_key_objectid(disk_key) !=
		    BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
		    btrfs_disk_key_type(disk_key) != BTRFS_CHUNK_ITEM_KEY) {
			error("invalid disk key found, (%llu %u %llu)",
				btrfs_disk_key_objectid(disk_key),
				btrfs_disk_key_type(disk_key),
				btrfs_disk_key_offset(disk_key));
			return -EUCLEAN;
		}
		chunk = (struct btrfs_chunk *)(disk_key + 1);
		num_stripes = btrfs_stack_chunk_num_stripes(chunk);

		ret = add_chunk_map(fs_info, btrfs_disk_key_offset(disk_key),
				chunk, sys_chunk_size - sizeof(*disk_key) - cur);
		if (ret < 0) {
			error("failed to add chunk %llu: %d",
				btrfs_disk_key_offset(disk_key), ret);
			return ret;
		}
		cur += btrfs_chunk_item_size(num_stripes) + sizeof(*disk_key);
	}
	return 0;
}

int btrfs_read_chunk_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_path path = {};
	struct btrfs_key_range range;
	int ret = 0;

	range.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	range.type_start = range.type_end = BTRFS_CHUNK_ITEM_KEY;
	range.offset_start = 0;
	range.offset_end = (u64)-1;
	ret = btrfs_search_keys_start(fs_info->chunk_root, &path, &range);
	if (ret < 0) {
		error("failed to read chunk items: %d", ret);
		return ret;
	}

	/* Read all chunk items */
	while (true) {
		struct btrfs_key key;
		u8 chunk_buf[4096] __attribute__((aligned(8)));
		u32 item_size;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		item_size = btrfs_item_size_nr(path.nodes[0], path.slots[0]);
		if (item_size > sizeof(chunk_buf)) {
			error("chunk item %llu too large: %u", key.offset,
			      item_size);
			ret = -EUCLEAN;
			goto out;
		}
		read_extent_buffer(path.nodes[0], chunk_buf,
				   btrfs_item_ptr_offset(path.nodes[0],
							 path.slots[0]),
				   item_size);

		ret = add_chunk_map(fs_info, key.offset,
				    (struct btrfs_chunk *)chunk_buf, item_size);
		if (ret < 0)
			goto out;

		ret = btrfs_search_keys_next(&path, &range);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			break;
		}
	}

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Pick the stripe for @mirror_nr (1 based) of a mirrored chunk, skipping
 * stripes that live on other devices.
 */
static struct btrfs_io_stripe *mirror_stripe(struct btrfs_fs_info *fs_info,
					     struct btrfs_chunk_map *map,
					     int first, int count,
					     int mirror_nr)
{
	struct btrfs_io_stripe *stripe = &map->stripes[first + mirror_nr - 1];

	if (mirror_nr < 1 || mirror_nr > count)
		return NULL;
	if (stripe->devid != fs_info->devid)
		return NULL;
	return stripe;
}

static int mirrored_read(struct btrfs_fs_info *fs_info,
			 struct btrfs_chunk_map *map, char *buf, size_t size,
			 u64 logical, int mirror_nr)
{
	struct btrfs_io_stripe *stripe;
	u64 offset = logical - map->logical;

	stripe = mirror_stripe(fs_info, map, 0, map->num_stripes, mirror_nr);
	if (!stripe)
		return -EIO;
	size = MIN(size, map->length - offset);
	return btrfs_read_from_disk(fs_info, buf, stripe->physical + offset,
				    size);
}

static int simple_stripe_read(struct btrfs_fs_info *fs_info,
			      struct btrfs_chunk_map *map, char *buf, size_t size,
			      u64 logical, int mirror_nr)
{
	struct btrfs_io_stripe *stripe;
	const u64 offset = logical - map->logical;
	const u64 stripe_len = map->stripe_len;
	const u16 sub_stripes = map->sub_stripes;
	const u16 data_stripes = map->num_stripes / map->sub_stripes;
	u64 stripe_nr = offset / stripe_len;
	u64 physical;
	u16 index;
	u64 len;

	len = MIN(size, round_down(offset + stripe_len, stripe_len) - offset);
	len = MIN(len, map->length - offset);

	index = (stripe_nr % data_stripes) * sub_stripes;
	stripe = mirror_stripe(fs_info, map, index, sub_stripes, mirror_nr);
	if (!stripe)
		return -EIO;

	physical = stripe->physical + (stripe_nr / data_stripes) * stripe_len +
		   offset % stripe_len;
	return btrfs_read_from_disk(fs_info, buf, physical, len);
}

int btrfs_num_copies(struct btrfs_fs_info *fs_info, u64 logical)
{
	struct btrfs_chunk_map *map;

	map = lookup_chunk_map(fs_info, logical);
	if (!map) {
		error("can not find chunk for logical %llu", logical);
		return -ENOENT;
	}
	if (map->num_mirrors == 0) {
		error("unsupported RAID profile 0x%llx for logical %llu",
		      map->flags & BTRFS_BLOCK_GROUP_PROFILE_MASK, logical);
		return -EOPNOTSUPP;
	}
	return map->num_mirrors;
}

int btrfs_read_logical(struct btrfs_fs_info *fs_info, char *buf, size_t size,
			u64 logical, int mirror_nr)
{
	struct btrfs_chunk_map *map;
	u64 profile;

	map = lookup_chunk_map(fs_info, logical);
	if (!map) {
		error("can not find chunk for logical %llu", logical);
		return -ENOENT;
	}
	profile = map->flags & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	switch (profile) {
	case 0:
	case BTRFS_BLOCK_GROUP_DUP:
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID1C3:
	case BTRFS_BLOCK_GROUP_RAID1C4:
		return mirrored_read(fs_info, map, buf, size, logical,
				     mirror_nr);
	case BTRFS_BLOCK_GROUP_RAID0:
	case BTRFS_BLOCK_GROUP_RAID10:
		return simple_stripe_read(fs_info, map, buf, size, logical,
					  mirror_nr);
	}
	error("unsupported RAID profile 0x%llx for logical %llu", profile,
	      logical);
	return -EOPNOTSUPP;
}
