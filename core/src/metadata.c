// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <sys/param.h>
#include "metadata.h"
#include "volumes.h"
#include "messages.h"
#include "hash.h"

static inline unsigned int eb_hash_bucket(u64 logical)
{
	u64 v = logical >> 12;

	v ^= v >> 17;
	v *= 0x9E3779B97F4A7C15ULL;
	return (unsigned int)(v >> 40) & (BTRFS_EB_HASH_BUCKETS - 1);
}

/* Caller must hold eb_lock */
static struct extent_buffer *eb_hash_find(struct btrfs_fs_info *fs_info,
					  u64 logical)
{
	struct extent_buffer *eb = fs_info->eb_hash[eb_hash_bucket(logical)];

	while (eb && eb->start != logical)
		eb = eb->hash_next;
	return eb;
}

/* Caller must hold eb_lock */
static void eb_hash_remove(struct btrfs_fs_info *fs_info,
			   struct extent_buffer *eb)
{
	struct extent_buffer **p = &fs_info->eb_hash[eb_hash_bucket(eb->start)];

	while (*p && *p != eb)
		p = &(*p)->hash_next;
	if (*p)
		*p = eb->hash_next;
	eb->hash_next = NULL;
}

/* Caller must hold eb_lock */
static void eb_hash_insert(struct btrfs_fs_info *fs_info,
			   struct extent_buffer *eb)
{
	unsigned int bucket = eb_hash_bucket(eb->start);

	eb->hash_next = fs_info->eb_hash[bucket];
	fs_info->eb_hash[bucket] = eb;
}

void free_extent_buffer(struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info;

	if (!eb)
		return;
	ASSERT(eb->refs > 0);

	fs_info = eb->fs_info;
	pthread_mutex_lock(&fs_info->eb_lock);
	eb->refs--;
	if (eb->refs == 0) {
		/* Keep it around, evict the least recently used one if needed */
		list_add_tail(&eb->lru, &fs_info->eb_lru);
		fs_info->nr_eb_lru++;
		while (fs_info->nr_eb_lru > BTRFS_EB_CACHE_MAX) {
			struct extent_buffer *victim;

			victim = list_first_entry(&fs_info->eb_lru,
						  struct extent_buffer, lru);
			list_del(&victim->lru);
			fs_info->nr_eb_lru--;
			eb_hash_remove(fs_info, victim);
			free(victim);
		}
	}
	pthread_mutex_unlock(&fs_info->eb_lock);
}

void btrfs_free_all_extent_buffers(struct btrfs_fs_info *fs_info)
{
	for (int i = 0; i < BTRFS_EB_HASH_BUCKETS; i++) {
		struct extent_buffer *eb = fs_info->eb_hash[i];

		while (eb) {
			struct extent_buffer *next = eb->hash_next;

			if (eb->refs)
				warning("eb %llu unreleased", eb->start);
			free(eb);
			eb = next;
		}
		fs_info->eb_hash[i] = NULL;
	}
	INIT_LIST_HEAD(&fs_info->eb_lru);
	fs_info->nr_eb_lru = 0;
}

void btrfs_init_path(struct btrfs_path *path)
{
	memset(path, 0, sizeof(*path));
}

void btrfs_release_path(struct btrfs_path *path)
{
	int i;

	for (i = BTRFS_MAX_LEVEL - 1; i >= 0; i--) {
		free_extent_buffer(path->nodes[i]);
		path->nodes[i] = NULL;
		path->slots[i] = 0;
	}
}

/* Check the sanity of the tree block, before doing the csum check */
static int verify_tree_block(struct extent_buffer *eb, u8 level,
			     u64 transid, struct btrfs_key *first_key)
{
	if (btrfs_header_bytenr(eb) != eb->start) {
		error("tree block %llu bad bytenr, has %llu expect %llu",
			eb->start, btrfs_header_bytenr(eb), eb->start);
		return -EIO;
	}
	if (btrfs_header_level(eb) != level) {
		error("tree block %llu bad level, has %u expect %u",
			eb->start, btrfs_header_level(eb), level);
		return -EIO;
	}
	if (btrfs_header_generation(eb) != transid) {
		error("tree block %llu bad trasid, has %llu expect %llu",
			eb->start, btrfs_header_generation(eb), transid);
		return -EIO;
	}
	if (first_key && btrfs_header_nritems(eb)) {
		struct btrfs_key found_key;

		if (btrfs_header_level(eb))
			btrfs_node_key_to_cpu(eb, &found_key, 0);
		else
			btrfs_item_key_to_cpu(eb, &found_key, 0);
		if (btrfs_comp_cpu_keys(first_key, &found_key)) {
			error(
	"tree block %llu key mismatch, has (%llu %u %llu) want (%llu %u %llu)",
			      eb->start, found_key.objectid, found_key.type,
			      found_key.offset, first_key->objectid,
			      first_key->type, first_key->offset);
			return -EIO;
		}
	}
	return 0;
}

/*
 * Structural check of a freshly read (checksum verified) tree block, so the
 * rest of the code can trust item offsets, sizes and node pointers.
 */
static int check_tree_block_layout(struct btrfs_fs_info *fs_info,
				   struct extent_buffer *eb)
{
	const u32 data_size = fs_info->nodesize - sizeof(struct btrfs_header);
	u32 nritems = btrfs_header_nritems(eb);
	struct btrfs_key prev_key;
	struct btrfs_key key;
	u32 i;

	if (memcmp_extent_buffer(eb, fs_info->metadata_uuid,
				 btrfs_header_fsid(), BTRFS_FSID_SIZE)) {
		error("tree block %llu has wrong fsid", eb->start);
		return -EIO;
	}
	if (btrfs_header_level(eb) >= BTRFS_MAX_LEVEL) {
		error("tree block %llu has invalid level %u", eb->start,
		      btrfs_header_level(eb));
		return -EUCLEAN;
	}

	if (btrfs_header_level(eb) > 0) {
		if (nritems == 0 ||
		    nritems > data_size / sizeof(struct btrfs_key_ptr)) {
			error("tree node %llu has invalid nritems %u",
			      eb->start, nritems);
			return -EUCLEAN;
		}
		for (i = 0; i < nritems; i++) {
			u64 bytenr = btrfs_node_blockptr(eb, i);

			if (!bytenr || !IS_ALIGNED(bytenr, fs_info->sectorsize)) {
				error("tree node %llu slot %u bad blockptr %llu",
				      eb->start, i, bytenr);
				return -EUCLEAN;
			}
			btrfs_node_key_to_cpu(eb, &key, i);
			if (i && btrfs_comp_cpu_keys(&prev_key, &key) >= 0) {
				error("tree node %llu slot %u keys out of order",
				      eb->start, i);
				return -EUCLEAN;
			}
			prev_key = key;
		}
		return 0;
	}

	if (nritems > data_size / sizeof(struct btrfs_item)) {
		error("tree leaf %llu has invalid nritems %u", eb->start,
		      nritems);
		return -EUCLEAN;
	}
	for (i = 0; i < nritems; i++) {
		u32 offset = btrfs_item_offset_nr(eb, i);
		u32 size = btrfs_item_size_nr(eb, i);

		if (offset < nritems * sizeof(struct btrfs_item) ||
		    offset > data_size || size > data_size - offset) {
			error("tree leaf %llu slot %u bad item offset %u size %u",
			      eb->start, i, offset, size);
			return -EUCLEAN;
		}
		btrfs_item_key_to_cpu(eb, &key, i);
		if (i && btrfs_comp_cpu_keys(&prev_key, &key) >= 0) {
			error("tree leaf %llu slot %u keys out of order",
			      eb->start, i);
			return -EUCLEAN;
		}
		prev_key = key;
	}
	return 0;
}

static struct extent_buffer *read_tree_block_disk(struct btrfs_fs_info *fs_info,
						  u64 logical, u8 level,
						  u64 transid,
						  struct btrfs_key *first_key)
{
	struct extent_buffer *eb;
	int mirror_nr;
	int max_mirror;
	int ret = -EIO;

	if (!IS_ALIGNED(logical, fs_info->sectorsize))
		return ERR_PTR(-EUCLEAN);

	max_mirror = btrfs_num_copies(fs_info, logical);
	if (max_mirror < 0)
		return ERR_PTR(max_mirror);

	eb = calloc(1, sizeof(*eb) + fs_info->nodesize);
	if (!eb)
		return ERR_PTR(-ENOMEM);
	eb->start = logical;
	eb->len = fs_info->nodesize;
	eb->refs = 0;
	eb->fs_info = fs_info;
	INIT_LIST_HEAD(&eb->lru);

	for (mirror_nr = 1; mirror_nr <= max_mirror; mirror_nr++) {
		u8 csum[BTRFS_CSUM_SIZE];

		ret = btrfs_read_logical(fs_info, eb->data,
					 fs_info->nodesize, logical, mirror_nr);
		/* Btrfs metadata should be read out in one go. */
		if (ret < (int)fs_info->nodesize) {
			ret = -EIO;
			continue;
		}
		btrfs_csum_data(fs_info->csum_type,
				(u8 *)eb->data + BTRFS_CSUM_SIZE, csum,
				fs_info->nodesize - BTRFS_CSUM_SIZE);
		if (memcmp(csum, eb->data, fs_info->csum_size)) {
			warning("tree block %llu mirror %d checksum mismatch",
				logical, mirror_nr);
			ret = -EIO;
			continue;
		}
		ret = verify_tree_block(eb, level, transid, first_key);
		if (ret < 0)
			continue;
		ret = check_tree_block_layout(fs_info, eb);
		if (ret < 0)
			continue;
		return eb;
	}

	free(eb);
	return ERR_PTR(ret);
}

struct extent_buffer *btrfs_read_tree_block(struct btrfs_fs_info *fs_info,
					    u64 logical, u8 level, u64 transid,
					    struct btrfs_key *first_key)
{
	struct extent_buffer *eb;
	struct extent_buffer *found;
	int ret;

	if (level >= BTRFS_MAX_LEVEL)
		return ERR_PTR(-EUCLEAN);

	pthread_mutex_lock(&fs_info->eb_lock);
	found = eb_hash_find(fs_info, logical);
	if (found)
		goto use_cached;
	pthread_mutex_unlock(&fs_info->eb_lock);

	/* Do the IO without holding the lock */
	eb = read_tree_block_disk(fs_info, logical, level, transid, first_key);
	if (IS_ERR(eb))
		return eb;

	pthread_mutex_lock(&fs_info->eb_lock);
	found = eb_hash_find(fs_info, logical);
	if (found) {
		/* Somebody else read it meanwhile */
		free(eb);
		goto use_cached;
	}
	eb->refs = 1;
	eb_hash_insert(fs_info, eb);
	pthread_mutex_unlock(&fs_info->eb_lock);
	return eb;

use_cached:
	/*
	 * Even for cached tree block, we still need to verify it in case of
	 * bad level/transid/first_key.
	 */
	ret = verify_tree_block(found, level, transid, first_key);
	if (ret < 0) {
		pthread_mutex_unlock(&fs_info->eb_lock);
		return ERR_PTR(ret);
	}
	if (found->refs == 0) {
		list_del(&found->lru);
		fs_info->nr_eb_lru--;
	}
	found->refs++;
	pthread_mutex_unlock(&fs_info->eb_lock);
	return found;
}

/*
 * Binary search inside an extent buffer.
 *
 * Since btrfs extent buffer has all its items/nodes put together sequentially,
 * we can do a binary search here.
 */
static int generic_bin_search(struct extent_buffer *eb, unsigned long p,
			      int item_size, const struct btrfs_key *key,
			      int max, int *slot)
{
	int low = 0;
	int high = max;
	int mid;
	int ret;
	unsigned long offset;

	while(low < high) {
		struct btrfs_disk_key *tmp;
		struct btrfs_key tmp_cpu_key;

		mid = (low + high) / 2;
		offset = p + mid * item_size;

		tmp = (struct btrfs_disk_key *)(eb->data + offset);
		btrfs_disk_key_to_cpu(&tmp_cpu_key, tmp);
		ret = btrfs_comp_cpu_keys(&tmp_cpu_key, key);

		if (ret < 0)
			low = mid + 1;
		else if (ret > 0)
			high = mid;
		else {
			*slot = mid;
			return 0;
		}
	}
	*slot = low;
	return 1;
}

/* Locate the slot inside the extent buffer */
static int search_slot_in_eb(struct extent_buffer *eb,
			     const struct btrfs_key *key, int *slot)
{
	if (btrfs_header_level(eb) == 0)
		return generic_bin_search(eb,
					  offsetof(struct btrfs_leaf, items),
					  sizeof(struct btrfs_item),
					  key, btrfs_header_nritems(eb),
					  slot);
	else
		return generic_bin_search(eb,
					  offsetof(struct btrfs_node, ptrs),
					  sizeof(struct btrfs_key_ptr),
					  key, btrfs_header_nritems(eb),
					  slot);
}

static struct extent_buffer *read_node_child(struct extent_buffer *parent,
					     int slot)
{
	struct btrfs_key first_key;
	u64 bytenr;
	u64 gen;

	ASSERT(btrfs_header_level(parent) > 0);
	ASSERT(slot < btrfs_header_nritems(parent));

	bytenr = btrfs_node_blockptr(parent, slot);
	gen = btrfs_node_ptr_generation(parent, slot);
	btrfs_node_key_to_cpu(parent, &first_key, slot);

	return btrfs_read_tree_block(parent->fs_info, bytenr,
			btrfs_header_level(parent) - 1, gen, &first_key);
}

int __btrfs_search_slot(struct btrfs_root *root, struct btrfs_path *path,
			struct btrfs_key *key)
{
	int level;
	int ret = 0;

	/* The path must not hold any tree blocks, or we will leak some eb */
	ASSERT(path->nodes[0] == NULL);
	level = btrfs_header_level(root->node);
	path->nodes[level] = extent_buffer_get(root->node);

	for (; level >= 0; level--) {
		int slot;

		ASSERT(path->nodes[level]);
		ret = search_slot_in_eb(path->nodes[level], key, &slot);
		/*
		 * For nodes if we didn't found a match, we should go previous
		 * slot.
		 * As the current slot has key value larger than our target,
		 * continue search will never hit our target, like this example:
		 *
		 * key = (1, 1, 1)
		 *
		 * 	(1, 1, 0)		(1, 2, 0)
		 * 	    /			    \
		 * (1, 1, 0), (1, 1, 1)		(1, 2, 0), (1, 2, 1)
		 *
		 * In above example, we should go through the child of (1, 1, 0)
		 * other than the slot returned (1, 2, 0).
		 * Not to mention returned slot may be unused.
		 */
		if (level && ret && slot > 0)
			slot--;
		path->slots[level] = slot;

		/* Now read the node for next level */
		if (level > 0) {
			struct extent_buffer *eb;

			eb = read_node_child(path->nodes[level], slot);
			if (IS_ERR(eb)) {
				ret = PTR_ERR(eb);
				goto error;
			}
			path->nodes[level - 1] = eb;
		}
	}
	return ret;
error:
	btrfs_release_path(path);
	return ret;
}

int btrfs_next_leaf(struct btrfs_path *path)
{
	int slot;
	int level;

	for (level = 1; level < BTRFS_MAX_LEVEL; level++) {
		/* No more parent */
		if (!path->nodes[level])
			return 1;

		slot = path->slots[level] + 1;
		/* Parent has next slot, continue to next step */
		if (slot < btrfs_header_nritems(path->nodes[level])) {
			path->slots[level] = slot;
			break;
		}
		/* Parent has no next slot, continue to higher level */
	}
	if (level >= BTRFS_MAX_LEVEL)
		return 1;

	/* Now we're at @slot of @level, go to the left most path */
	for (; level; level--) {
		struct extent_buffer *eb;

		slot = path->slots[level];
		eb = read_node_child(path->nodes[level], slot);
		if (IS_ERR(eb)) {
			btrfs_release_path(path);
			return PTR_ERR(eb);
		}
		free_extent_buffer(path->nodes[level - 1]);
		path->nodes[level - 1] = eb;
		path->slots[level - 1] = 0;
	}
	return 0;
}

int btrfs_search_key(struct btrfs_root *root, struct btrfs_path *path,
		     struct btrfs_key *key)
{
	int ret;

	ret = __btrfs_search_slot(root, path, key);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		btrfs_release_path(path);
	return ret;
}

static int key_in_range(struct btrfs_key *key,
			struct btrfs_key_range *range)
{
	struct btrfs_key range_key1;
	struct btrfs_key range_key2;

	range_key1.objectid = range->objectid;
	range_key1.type = range->type_start;
	range_key1.offset = range->offset_start;

	range_key2.objectid = range->objectid;
	range_key2.type = range->type_end;
	range_key2.offset = range->offset_end;

	return (btrfs_comp_cpu_keys(&range_key1, key) <= 0 &&
		btrfs_comp_cpu_keys(key, &range_key2) <= 0);
}

int btrfs_search_keys_start(struct btrfs_root *root, struct btrfs_path *path,
			    struct btrfs_key_range *range)
{
	struct btrfs_key key;
	int ret;

	key.objectid = range->objectid;
	key.type = range->type_start;
	key.offset = range->offset_start;

	ret = __btrfs_search_slot(root, path, &key);
	/* Either found or error */
	if (ret <= 0)
		return ret;

	/* Check if current slot is used first */
	if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
		ret = btrfs_next_leaf(path);
		if (ret > 0)
			ret = -ENOENT;
		if (ret < 0) {
			btrfs_release_path(path);
			return ret;
		}
	}

	/* Check if the found key is in the target range */
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	if (!key_in_range(&key, range)) {
		btrfs_release_path(path);
		return -ENOENT;
	}
	return 0;
}

int btrfs_search_keys_next(struct btrfs_path *path,
			   struct btrfs_key_range *range)
{
	struct btrfs_key key;
	int ret;

	ASSERT(path->nodes[0]);

	path->slots[0]++;
	if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
		ret = btrfs_next_leaf(path);
		if (ret)
			return ret;
	}

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	if (key_in_range(&key, range))
		return 0;
	return 1;
}

/* Caller must hold root_lock */
static struct btrfs_root *find_cached_subvol_root(struct btrfs_fs_info *fs_info,
						  u64 rootid)
{
	struct btrfs_root *root;

	list_for_each_entry(root, &fs_info->subvols, list)
		if (root->root_key.objectid == rootid)
			return root;
	return NULL;
}

static int search_root_item(struct btrfs_fs_info *fs_info, u64 rootid,
			    struct btrfs_key *found_key,
			    struct btrfs_root_item *ri)
{
	struct btrfs_key_range key_range;
	struct btrfs_path path;
	int ret;

	/* At this stage, root tree must be initialized */
	ASSERT(fs_info->tree_root);

	btrfs_init_path(&path);
	key_range.objectid = rootid;
	key_range.type_start = key_range.type_end = BTRFS_ROOT_ITEM_KEY;
	key_range.offset_start = 0;
	key_range.offset_end = (u64)-1;

	ret = btrfs_search_keys_start(fs_info->tree_root, &path, &key_range);
	if (ret < 0)
		return ret;

	memset(ri, 0, sizeof(*ri));
	read_extent_buffer(path.nodes[0], ri,
			btrfs_item_ptr_offset(path.nodes[0], path.slots[0]),
			MIN(btrfs_item_size_nr(path.nodes[0], path.slots[0]),
			    sizeof(*ri)));
	btrfs_item_key_to_cpu(path.nodes[0], found_key, path.slots[0]);
	btrfs_release_path(&path);
	return 0;
}

static struct btrfs_root *__read_root(struct btrfs_fs_info *fs_info,
				      u64 rootid)
{
	struct btrfs_super_block *sb = &fs_info->super_copy;
	struct btrfs_root *root;
	struct btrfs_key root_key = {};
	u64 gen;
	u64 bytenr;
	u8 level;
	int ret;

	root = calloc(1, sizeof(*root));
	if (!root)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&root->list);
	root->fs_info = fs_info;

	root_key.objectid = rootid;
	root_key.type = BTRFS_ROOT_ITEM_KEY;
	root_key.offset = 0;
	/*
	 * Allocate a new root and read from disk, we need to grab the info for
	 * the root tree block.
	 *
	 * For chunk and root tree, they need to be grabbed from superblock, all
	 * other trees needs to be grabed from tree root.
	 */
	if (rootid == BTRFS_CHUNK_TREE_OBJECTID) {
		gen = btrfs_super_chunk_root_generation(sb);
		level = btrfs_super_chunk_root_level(sb);
		bytenr = btrfs_super_chunk_root(sb);
	} else if (rootid == BTRFS_ROOT_TREE_OBJECTID){
		gen = btrfs_super_generation(sb);
		level = btrfs_super_root_level(sb);
		bytenr = btrfs_super_root(sb);
	} else {
		struct btrfs_root_item ri;

		ret = search_root_item(fs_info, rootid, &root_key, &ri);
		if (ret < 0) {
			free(root);
			return ERR_PTR(ret);
		}
		gen = btrfs_root_generation(&ri);
		level = btrfs_root_level(&ri);
		bytenr = btrfs_root_bytenr(&ri);
		root->root_dirid = btrfs_root_dirid(&ri);
	}

	memcpy(&root->root_key, &root_key, sizeof(root_key));
	root->node = btrfs_read_tree_block(fs_info, bytenr, level, gen, NULL);
	if (IS_ERR(root->node)) {
		ret = PTR_ERR(root->node);
		free(root);
		return ERR_PTR(ret);
	}
	return root;
}

struct btrfs_root *btrfs_read_root(struct btrfs_fs_info *fs_info, u64 rootid)
{
	struct btrfs_root *root;

	/* For non-subvolume trees, return cached result */
	if (rootid == BTRFS_CHUNK_TREE_OBJECTID && fs_info->chunk_root)
		return fs_info->chunk_root;
	if (rootid == BTRFS_ROOT_TREE_OBJECTID && fs_info->tree_root)
		return fs_info->tree_root;
	if (rootid == BTRFS_CSUM_TREE_OBJECTID && fs_info->csum_root)
		return fs_info->csum_root;

	if (!is_fstree(rootid))
		return __read_root(fs_info, rootid);

	/* Subvolume trees are cached for the lifetime of the mount */
	pthread_mutex_lock(&fs_info->root_lock);
	root = find_cached_subvol_root(fs_info, rootid);
	if (!root) {
		root = __read_root(fs_info, rootid);
		if (!IS_ERR(root))
			list_add_tail(&root->list, &fs_info->subvols);
	}
	pthread_mutex_unlock(&fs_info->root_lock);
	return root;
}
