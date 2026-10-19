// SPDX-License-Identifier: MIT

#include "ondisk_format.h"
#include "super.h"
#include "messages.h"
#include "hash.h"
#include "volumes.h"
#include "metadata.h"

static void uuid_unparse(const u8 *uuid, char *out)
{
	snprintf(out, BTRFS_UUID_UNPARSED_SIZE,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
		 uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
		 uuid[12], uuid[13], uuid[14], uuid[15]);
}

int btrfs_check_super(struct btrfs_super_block *sb)
{
	u8 result[BTRFS_CSUM_SIZE];
	u32 sectorsize;
	u32 nodesize;
	u16 csum_type;
	int csum_size;

	if (btrfs_super_magic(sb) != BTRFS_MAGIC)
		return -EINVAL;

	csum_type = btrfs_super_csum_type(sb);
	if (csum_type >= btrfs_super_num_csums()) {
		error("unsupported checksum algorithm %u", csum_type);
		return -EIO;
	}
	csum_size = btrfs_super_csum_size(sb);

	btrfs_csum_data(csum_type, (u8 *)sb + BTRFS_CSUM_SIZE,
			result, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);

	if (memcmp(result, sb->csum, csum_size)) {
		error("superblock checksum mismatch");
		return -EIO;
	}

	/* We don't support dumps/FSID change in progress */
	if (btrfs_super_flags(sb) & ~BTRFS_SUPER_FLAG_SUPP) {
		error("unsupported super flags: %llx", btrfs_super_flags(sb));
		return -EOPNOTSUPP;
	}
	if (btrfs_super_incompat_flags(sb) & ~BTRFS_FEATURE_INCOMPAT_SUPP) {
		error("unsupported incompat features: %llx",
		      btrfs_super_incompat_flags(sb) &
		      ~BTRFS_FEATURE_INCOMPAT_SUPP);
		return -EOPNOTSUPP;
	}

	/* Root level checks */
	if (btrfs_super_root_level(sb) >= BTRFS_MAX_LEVEL) {
		error("tree_root level too big: %d >= %d",
			btrfs_super_root_level(sb), BTRFS_MAX_LEVEL);
		goto error_out;
	}
	if (btrfs_super_chunk_root_level(sb) >= BTRFS_MAX_LEVEL) {
		error("chunk_root level too big: %d >= %d",
			btrfs_super_chunk_root_level(sb), BTRFS_MAX_LEVEL);
		goto error_out;
	}
	if (btrfs_super_log_root_level(sb) >= BTRFS_MAX_LEVEL) {
		error("log_root level too big: %d >= %d",
			btrfs_super_log_root_level(sb), BTRFS_MAX_LEVEL);
		goto error_out;
	}

	/* Sectorsize/nodesize checks */
	sectorsize = btrfs_super_sectorsize(sb);
	nodesize = btrfs_super_nodesize(sb);

	if (!is_power_of_2(sectorsize) || sectorsize > BTRFS_SECTORSIZE_MAX ||
	    sectorsize < BTRFS_SECTORSIZE_MIN) {
		error("invalid sectorsize: %u", sectorsize);
		goto error_out;
	}
	if (!is_power_of_2(nodesize) || nodesize> BTRFS_NODESIZE_MAX ||
	    nodesize < BTRFS_NODESIZE_MIN || nodesize < sectorsize) {
		error("invalid nodesize: %u", nodesize);
		goto error_out;
	}

	/*
	 * Root alignment check
	 *
	 * We may have rare case where chunk is sectorsize aligned but not
	 * nodesize aligned.
	 * In that case, we only require sectorsize alignment.
	 */
	if (!IS_ALIGNED(btrfs_super_root(sb), sectorsize)) {
		error("tree_root block unaligned: %llu", btrfs_super_root(sb));
		goto error_out;
	}
	if (!IS_ALIGNED(btrfs_super_chunk_root(sb), sectorsize)) {
		error("chunk_root block unaligned: %llu",
			btrfs_super_chunk_root(sb));
		goto error_out;
	}
	if (!IS_ALIGNED(btrfs_super_log_root(sb), sectorsize)) {
		error("log_root block unaligned: %llu",
			btrfs_super_log_root(sb));
		goto error_out;
	}

	/* Basic size check */
	if (btrfs_super_total_bytes(sb) == 0) {
		error("invalid total_bytes 0");
		goto error_out;
	}
	if (btrfs_super_bytes_used(sb) < 6 * btrfs_super_nodesize(sb)) {
		error("invalid bytes_used %llu", btrfs_super_bytes_used(sb));
		goto error_out;
	}

	if (memcmp((btrfs_super_incompat_flags(sb) &
		    BTRFS_FEATURE_INCOMPAT_METADATA_UUID) ?
		   sb->metadata_uuid : sb->fsid,
		   sb->dev_item.fsid, BTRFS_FSID_SIZE) != 0) {
		char fsid[BTRFS_UUID_UNPARSED_SIZE];
		char dev_fsid[BTRFS_UUID_UNPARSED_SIZE];

		uuid_unparse(sb->fsid, fsid);
		uuid_unparse(sb->dev_item.fsid, dev_fsid);
		error("dev_item UUID does not match fsid: %s != %s",
				dev_fsid, fsid);
		goto error_out;
	}

	/*
	 * Hint to catch really bogus numbers, bitflips or so
	 */
	if (btrfs_super_num_devices(sb) > (1UL << 31)) {
		warning("suspicious number of devices: %llu",
			btrfs_super_num_devices(sb));
	}

	if (btrfs_super_num_devices(sb) == 0) {
		error("number of devices is 0");
		goto error_out;
	}

	/*
	 * Obvious sys_chunk_array corruptions, it must hold at least one key
	 * and one chunk
	 */
	if (btrfs_super_sys_array_size(sb) > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) {
		error("system chunk array too big %u > %u",
		      btrfs_super_sys_array_size(sb),
		      BTRFS_SYSTEM_CHUNK_ARRAY_SIZE);
		goto error_out;
	}
	if (btrfs_super_sys_array_size(sb) < sizeof(struct btrfs_disk_key)
			+ sizeof(struct btrfs_chunk)) {
		error("system chunk array too small %u < %zu",
		      btrfs_super_sys_array_size(sb),
		      sizeof(struct btrfs_disk_key) +
		      sizeof(struct btrfs_chunk));
		goto error_out;
	}

	return 0;

error_out:
	error("superblock checksum matches but it has invalid members");
	return -EIO;
}

int btrfs_read_super(const struct btrfsro_io *io, struct btrfs_super_block *sb)
{
	int ret;

	if (io->size && io->size < BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE)
		return -EINVAL;
	ret = io->read(io->ctx, sb, BTRFS_SUPER_INFO_OFFSET,
		       BTRFS_SUPER_INFO_SIZE);
	if (ret < 0) {
		error("failed to read super block: %d", ret);
		return ret;
	}
	if (btrfs_super_magic(sb) != BTRFS_MAGIC)
		return -EINVAL;
	return btrfs_check_super(sb);
}

static void free_root(struct btrfs_root *root)
{
	if (!root || IS_ERR(root))
		return;
	free_extent_buffer(root->node);
	free(root);
}

void btrfs_unmount(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root;
	struct btrfs_root *tmp;

	list_for_each_entry_safe(root, tmp, &fs_info->subvols, list) {
		list_del(&root->list);
		free_root(root);
	}

	free_root(fs_info->csum_root);
	free_root(fs_info->tree_root);
	free_root(fs_info->chunk_root);

	btrfs_free_all_extent_buffers(fs_info);
	btrfs_free_chunk_maps(fs_info);
	pthread_mutex_destroy(&fs_info->eb_lock);
	pthread_mutex_destroy(&fs_info->root_lock);
	free(fs_info);
}

static struct btrfs_root *read_default_root(struct btrfs_fs_info *fs_info)
{
	struct btrfs_key_range range;
	struct btrfs_dir_item *di;
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	range.objectid = BTRFS_ROOT_TREE_DIR_OBJECTID;
	range.type_start = range.type_end = BTRFS_DIR_ITEM_KEY;
	range.offset_start = 0;
	range.offset_end = (u64)-1;

	ret = btrfs_search_keys_start(fs_info->tree_root, &path, &range);
	if (ret == -ENOENT) {
		/* No "default" dir item, use the top level subvolume */
		return btrfs_read_root(fs_info, BTRFS_FS_TREE_OBJECTID);
	}
	if (ret < 0)
		return ERR_PTR(ret);
	if (btrfs_item_size_nr(path.nodes[0], path.slots[0]) <
	    sizeof(struct btrfs_dir_item)) {
		btrfs_release_path(&path);
		return ERR_PTR(-EUCLEAN);
	}
	di = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_dir_item);
	btrfs_dir_item_key_to_cpu(path.nodes[0], di, &key);
	btrfs_release_path(&path);

	if (!is_fstree(key.objectid)) {
		error("invalid default subvolume %llu", key.objectid);
		return ERR_PTR(-EUCLEAN);
	}
	return btrfs_read_root(fs_info, key.objectid);
}

struct btrfs_fs_info *btrfs_mount(const struct btrfsro_io *io)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_super_block *sb;
	int ret;

	fs_info = calloc(1, sizeof(*fs_info));
	if (!fs_info)
		return ERR_PTR(-ENOMEM);

	pthread_mutex_init(&fs_info->eb_lock, NULL);
	pthread_mutex_init(&fs_info->root_lock, NULL);
	INIT_LIST_HEAD(&fs_info->subvols);
	INIT_LIST_HEAD(&fs_info->eb_lru);
	fs_info->io = *io;
	fs_info->verify_data = true;
	sb = &fs_info->super_copy;

	/* Check if there is btrfs on the device */
	ret = btrfs_read_super(io, sb);
	if (ret < 0) {
		if (ret == -EINVAL)
			error("no valid btrfs found");
		goto error;
	}
	if (btrfs_super_num_devices(sb) != 1) {
		error("multi-device filesystems are not supported (%llu devices)",
		      btrfs_super_num_devices(sb));
		ret = -EOPNOTSUPP;
		goto error;
	}
	if (btrfs_super_log_root(sb))
		warning("filesystem has an unreplayed log tree, changes fsynced after the last commit are not visible");

	fs_info->sectorsize = btrfs_super_sectorsize(sb);
	fs_info->nodesize = btrfs_super_nodesize(sb);
	fs_info->csum_type = btrfs_super_csum_type(sb);
	fs_info->csum_size = btrfs_super_csum_size(sb);
	fs_info->generation = btrfs_super_generation(sb);
	fs_info->devid = btrfs_stack_device_id(&sb->dev_item);
	memcpy(fs_info->dev_uuid, sb->dev_item.uuid, BTRFS_UUID_SIZE);
	memcpy(fs_info->fsid, sb->fsid, BTRFS_UUID_SIZE);
	if (btrfs_super_incompat_flags(sb) & BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		memcpy(fs_info->metadata_uuid, sb->metadata_uuid,
		       BTRFS_UUID_SIZE);
	else
		memcpy(fs_info->metadata_uuid, sb->fsid, BTRFS_UUID_SIZE);

	/* Then read the system chunk array */
	ret = btrfs_read_sys_chunk_array(fs_info);
	if (ret < 0) {
		error("failed to read system chunk array: %d", ret);
		goto error;
	}

	/* Now we can read the chunk tree */
	fs_info->chunk_root = btrfs_read_root(fs_info,
					      BTRFS_CHUNK_TREE_OBJECTID);
	if (IS_ERR(fs_info->chunk_root)) {
		ret = PTR_ERR(fs_info->chunk_root);
		fs_info->chunk_root = NULL;
		error("failed to read chunk root: %d", ret);
		goto error;
	}

	/* Then read the chunk tree */
	ret = btrfs_read_chunk_tree(fs_info);
	if (ret < 0) {
		error("failed to iterate chunk tree: %d", ret);
		goto error;
	}

	/* Read the remaining trees */
	fs_info->tree_root = btrfs_read_root(fs_info, BTRFS_ROOT_TREE_OBJECTID);
	if (IS_ERR(fs_info->tree_root)) {
		ret = PTR_ERR(fs_info->tree_root);
		fs_info->tree_root = NULL;
		error("failed to read tree root: %d", ret);
		goto error;
	}
	fs_info->csum_root = btrfs_read_root(fs_info, BTRFS_CSUM_TREE_OBJECTID);
	if (IS_ERR(fs_info->csum_root)) {
		ret = PTR_ERR(fs_info->csum_root);
		fs_info->csum_root = NULL;
		error("failed to read csum root: %d", ret);
		goto error;
	}
	fs_info->default_root = read_default_root(fs_info);
	if (IS_ERR(fs_info->default_root)) {
		ret = PTR_ERR(fs_info->default_root);
		fs_info->default_root = NULL;
		error("failed to read default root: %d", ret);
		goto error;
	}
	return fs_info;
error:
	btrfs_unmount(fs_info);
	return ERR_PTR(ret);
}
