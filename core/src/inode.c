// SPDX-License-Identifier: MIT

#include <sys/param.h>
#include <sys/stat.h>
#include "inode.h"
#include "ctree.h"
#include "metadata.h"
#include "hash.h"
#include "messages.h"

/*
 * Walk the dir items packed into one DIR_ITEM/XATTR_ITEM item and find the one
 * named @name. All dir items are bounds checked against the item.
 *
 * Return the offset of the matching btrfs_dir_item inside the leaf,
 * -ENOENT if not found, -EUCLEAN for a corrupted item.
 */
static long find_dir_item_by_name(struct extent_buffer *leaf, int slot,
				  const char *name, size_t name_len)
{
	u32 item_start = btrfs_item_ptr_offset(leaf, slot);
	u32 item_len = btrfs_item_size_nr(leaf, slot);
	u32 cur = item_start;

	/*
	 * We can have name hash conflicts, thus still need to verify the
	 * found dir_item one by one.
	 */
	while (cur < item_start + item_len) {
		struct btrfs_dir_item *di = (struct btrfs_dir_item *)(long)cur;
		u32 this_item_size;
		u16 this_name_len;

		if (item_start + item_len - cur < sizeof(*di))
			return -EUCLEAN;
		this_name_len = btrfs_dir_name_len(leaf, di);
		this_item_size = sizeof(*di) + btrfs_dir_data_len(leaf, di) +
				 this_name_len;
		if (this_item_size > item_start + item_len - cur) {
			error(
"invalid dir item size, cur=%u dir_item size=%u item start=%u item len=%u",
				cur, this_item_size, item_start, item_len);
			return -EUCLEAN;
		}

		if (this_name_len == name_len &&
		    !memcmp_extent_buffer(leaf, name, cur + sizeof(*di),
					  name_len))
			return cur;
		cur += this_item_size;
	}
	return -ENOENT;
}

int btrfs_lookup_one_name(struct btrfs_fs_info *fs_info,
			  struct btrfs_inode *dir, const char *name,
			  size_t name_len, struct btrfs_inode *inode_ret)
{
	struct btrfs_dir_item *di;
	struct btrfs_root *root;
	struct btrfs_key key;
	struct btrfs_path path;
	u64 ino;
	u8 file_type;
	long offset;
	int ret;

	if (dir->file_type != BTRFS_FT_DIR)
		return -ENOTDIR;
	if (name_len == 0)
		return -ENOENT;
	if (name_len > BTRFS_NAME_LEN)
		return -ENAMETOOLONG;

	btrfs_init_path(&path);
	key.objectid = dir->ino;
	key.type = BTRFS_DIR_ITEM_KEY;
	key.offset = btrfs_name_hash(name, name_len);

	ret = btrfs_search_key(dir->root, &path, &key);
	if (ret < 0)
		return ret;

	offset = find_dir_item_by_name(path.nodes[0], path.slots[0], name,
				       name_len);
	if (offset < 0) {
		btrfs_release_path(&path);
		return (int)offset;
	}
	di = (struct btrfs_dir_item *)offset;

	/* Found the dir item we want, extract root/ino from it */
	btrfs_dir_item_key_to_cpu(path.nodes[0], di, &key);
	file_type = btrfs_dir_type(path.nodes[0], di);
	btrfs_release_path(&path);

	if (key.type == BTRFS_ROOT_ITEM_KEY) {
		root = btrfs_read_root(fs_info, key.objectid);
		if (IS_ERR(root))
			return PTR_ERR(root);
		ino = root->root_dirid;
		file_type = BTRFS_FT_DIR;
	} else if (key.type == BTRFS_INODE_ITEM_KEY){
		root = dir->root;
		ino = key.objectid;
	} else {
		error("invalid dir item key found: (%llu %u %llu)",
			key.objectid, key.type, key.offset);
		return -EUCLEAN;
	}

	inode_ret->root = root;
	inode_ret->ino = ino;
	inode_ret->file_type = file_type;
	return 0;
}

int btrfs_resolve_path(struct btrfs_fs_info *fs_info,
		       const char *path, size_t path_len,
		       struct btrfs_inode *inode_ret)
{
	struct btrfs_inode cur_inode;
	struct btrfs_inode next_inode;
	size_t offset = 0;
	char *slash_char;
	int ret;

	cur_inode.root = fs_info->default_root;
	cur_inode.ino = fs_info->default_root->root_dirid;
	cur_inode.file_type = BTRFS_FT_DIR;

	while (offset < path_len) {
		size_t name_len;

		/* Skip any '/' in the path*/
		if (path[offset] == '/') {
			offset++;
			continue;
		}

		/* Extract the next filename to resolve */
		slash_char = memchr(path + offset, '/', path_len - offset);
		if (slash_char) {
			name_len = slash_char - (path + offset);
		} else {
			/* Last name, no more '/' */
			name_len = path_len - offset;
		}

		ret = btrfs_lookup_one_name(fs_info, &cur_inode, path + offset,
					    name_len, &next_inode);
		if (ret < 0)
			return ret;

		memcpy(&cur_inode, &next_inode, sizeof(next_inode));
		offset += name_len;
	}

	memcpy(inode_ret, &cur_inode, sizeof(cur_inode));
	return 0;
}

int btrfs_lookup_parent(struct btrfs_fs_info *fs_info, struct btrfs_inode *dir,
			struct btrfs_inode *parent_ret)
{
	struct btrfs_key_range range;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root *root;
	int ret;

	if (dir->file_type != BTRFS_FT_DIR)
		return -ENOTDIR;

	btrfs_init_path(&path);
	if (dir->ino == dir->root->root_dirid) {
		struct btrfs_root_ref *ref;
		u64 parent_dirid;

		/* The filesystem root is its own parent */
		if (dir->root == fs_info->default_root) {
			*parent_ret = *dir;
			return 0;
		}

		/* Subvolume root, find where it is linked from */
		range.objectid = dir->root->root_key.objectid;
		range.type_start = range.type_end = BTRFS_ROOT_BACKREF_KEY;
		range.offset_start = 0;
		range.offset_end = (u64)-1;
		ret = btrfs_search_keys_start(fs_info->tree_root, &path,
					      &range);
		if (ret == -ENOENT) {
			/* Not linked anywhere (e.g. top level above default) */
			*parent_ret = *dir;
			return 0;
		}
		if (ret < 0)
			return ret;
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (btrfs_item_size_nr(path.nodes[0], path.slots[0]) <
		    sizeof(*ref)) {
			btrfs_release_path(&path);
			return -EUCLEAN;
		}
		ref = btrfs_item_ptr(path.nodes[0], path.slots[0],
				     struct btrfs_root_ref);
		parent_dirid = btrfs_root_ref_dirid(path.nodes[0], ref);
		btrfs_release_path(&path);

		root = btrfs_read_root(fs_info, key.offset);
		if (IS_ERR(root))
			return PTR_ERR(root);
		parent_ret->root = root;
		parent_ret->ino = parent_dirid;
		parent_ret->file_type = BTRFS_FT_DIR;
		return 0;
	}

	/* Directories have exactly one INODE_REF, keyed by the parent */
	range.objectid = dir->ino;
	range.type_start = range.type_end = BTRFS_INODE_REF_KEY;
	range.offset_start = 0;
	range.offset_end = (u64)-1;
	ret = btrfs_search_keys_start(dir->root, &path, &range);
	if (ret < 0)
		return ret == -ENOENT ? -EUCLEAN : ret;
	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	btrfs_release_path(&path);

	parent_ret->root = dir->root;
	parent_ret->ino = key.offset;
	parent_ret->file_type = BTRFS_FT_DIR;
	return 0;
}

int btrfs_read_link(struct btrfs_fs_info *fs_info,
		    struct btrfs_inode *inode, char *output,
		    size_t output_size)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u64 link_len;
	u32 inline_len;
	u32 read_size;
	int ret;

	if (inode->file_type != BTRFS_FT_SYMLINK)
		return -EINVAL;
	if (output_size == 0)
		return -ERANGE;

	btrfs_init_path(&path);
	key.objectid = inode->ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_key(inode->root, &path, &key);
	if (ret < 0)
		return ret == -ENOENT ? -EUCLEAN : ret;

	leaf = path.nodes[0];
	ret = -EUCLEAN;
	if (btrfs_item_size_nr(leaf, path.slots[0]) <
	    BTRFS_FILE_EXTENT_INLINE_DATA_START)
		goto out;
	fi = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_file_extent_item);
	if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_INLINE) {
		error("invalid file extent type, has %u expect %u",
			btrfs_file_extent_type(leaf, fi),
			BTRFS_FILE_EXTENT_INLINE);
		goto out;
	}
	if (btrfs_file_extent_compression(leaf, fi) != BTRFS_COMPRESS_NONE) {
		error("invalid file extent compression, has %u expect %u",
			btrfs_file_extent_compression(leaf, fi),
			BTRFS_COMPRESS_NONE);
		goto out;
	}
	link_len = btrfs_file_extent_ram_bytes(leaf, fi);
	inline_len = btrfs_file_extent_inline_item_len(leaf,
						btrfs_item_nr(path.slots[0]));
	if (link_len == 0 || link_len > inline_len) {
		error("invalid link length %llu (inline %u)", link_len,
		      inline_len);
		goto out;
	}
	if (link_len >= output_size) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	read_size = (u32)link_len;
	read_extent_buffer(leaf, output, btrfs_file_extent_inline_start(fi),
			   read_size);
	output[read_size] = '\0';
	ret = read_size;
out:
	btrfs_release_path(&path);
	return ret;
}

int btrfs_iterate_dir_start(struct btrfs_fs_info *fs_info,
			    struct btrfs_iterate_dir_ctrl *ctrl,
			    const struct btrfs_inode *dir, u64 start_index)
{
	struct btrfs_key_range *range = &ctrl->range;

	if (dir->file_type != BTRFS_FT_DIR)
		return -ENOTDIR;

	range->objectid = dir->ino;
	range->type_start = range->type_end = BTRFS_DIR_INDEX_KEY;
	range->offset_start = start_index;
	range->offset_end = (u64)-1;

	ctrl->dir.ino = dir->ino;
	ctrl->dir.root = dir->root;
	ctrl->dir.file_type = dir->file_type;
	btrfs_init_path(&ctrl->path);

	return btrfs_search_keys_start(ctrl->dir.root, &ctrl->path, &ctrl->range);
}

int btrfs_iterate_dir_get_inode(struct btrfs_fs_info *fs_info,
				struct btrfs_iterate_dir_ctrl *ctrl,
				struct btrfs_inode *entry,
				u64 *index_ret, char *name, size_t *name_len)
{
	struct extent_buffer *leaf = ctrl->path.nodes[0];
	int slot = ctrl->path.slots[0];
	struct btrfs_dir_item *di;
	struct btrfs_key key;
	u32 item_size;
	u16 len;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	if (key.type != BTRFS_DIR_INDEX_KEY)
		return -EUCLEAN;

	if (index_ret)
		*index_ret = key.offset;

	item_size = btrfs_item_size_nr(leaf, slot);
	if (item_size < sizeof(*di))
		return -EUCLEAN;
	di = btrfs_item_ptr(leaf, slot, struct btrfs_dir_item);
	len = btrfs_dir_name_len(leaf, di);
	if (len == 0 || len > BTRFS_NAME_LEN || sizeof(*di) + len > item_size) {
		error("invalid dir index item, name len %u item size %u",
		      len, item_size);
		return -EUCLEAN;
	}
	btrfs_dir_item_key_to_cpu(leaf, di, &key);

	if (key.type == BTRFS_INODE_ITEM_KEY) {
		entry->root = ctrl->dir.root;
		entry->ino = key.objectid;
		entry->file_type = btrfs_dir_type(leaf, di);
	} else if (key.type == BTRFS_ROOT_ITEM_KEY) {
		struct btrfs_root *root;

		root = btrfs_read_root(fs_info, key.objectid);
		if (IS_ERR(root))
			return PTR_ERR(root);

		entry->root = root;
		entry->ino = root->root_dirid;
		entry->file_type = BTRFS_FT_DIR;
	} else {
		return -EUCLEAN;
	}
	if (entry->file_type > BTRFS_FT_SYMLINK)
		entry->file_type = BTRFS_FT_UNKNOWN;

	*name_len = len;
	read_extent_buffer(leaf, name, (unsigned long)(di + 1), len);
	return 0;
}

static void copy_timespec(struct extent_buffer *leaf, struct btrfs_timespec *ts,
			  struct btrfsro_timespec *out)
{
	out->sec = (int64_t)btrfs_timespec_sec(leaf, ts);
	out->nsec = btrfs_timespec_nsec(leaf, ts);
	if (out->nsec >= 1000000000U)
		out->nsec = 0;
}

int btrfs_stat(struct btrfs_fs_info *fs_info, struct btrfs_inode *inode,
	       struct btrfsro_stat *st)
{
	struct btrfs_inode_item *ii;
	struct extent_buffer *leaf;
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = inode->ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_key(inode->root, &path, &key);
	if (ret < 0)
		return ret;

	leaf = path.nodes[0];
	if (btrfs_item_size_nr(leaf, path.slots[0]) < sizeof(*ii)) {
		btrfs_release_path(&path);
		return -EUCLEAN;
	}
	ii = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);
	memset(st, 0, sizeof(*st));
	st->subvol = inode->root->root_key.objectid;
	st->ino = inode->ino;
	st->mode = btrfs_inode_mode(leaf, ii);
	st->nlink = btrfs_inode_nlink(leaf, ii);
	st->uid = btrfs_inode_uid(leaf, ii);
	st->gid = btrfs_inode_gid(leaf, ii);
	st->rdev = btrfs_inode_rdev(leaf, ii);
	st->size = btrfs_inode_size(leaf, ii);
	st->nbytes = btrfs_inode_nbytes(leaf, ii);
	st->generation = btrfs_inode_generation(leaf, ii);
	st->flags = btrfs_inode_flags(leaf, ii);
	copy_timespec(leaf, btrfs_inode_atime(ii), &st->atime);
	copy_timespec(leaf, btrfs_inode_mtime(ii), &st->mtime);
	copy_timespec(leaf, btrfs_inode_ctime(ii), &st->ctime);
	copy_timespec(leaf, btrfs_inode_otime(ii), &st->otime);
	btrfs_release_path(&path);
	return 0;
}

ssize_t btrfs_listxattr(struct btrfs_fs_info *fs_info,
			struct btrfs_inode *inode, char *buf, size_t size)
{
	struct btrfs_key_range range;
	struct btrfs_path path;
	size_t total = 0;
	int ret;

	btrfs_init_path(&path);
	range.objectid = inode->ino;
	range.type_start = range.type_end = BTRFS_XATTR_ITEM_KEY;
	range.offset_start = 0;
	range.offset_end = (u64)-1;

	ret = btrfs_search_keys_start(inode->root, &path, &range);
	if (ret == -ENOENT)
		return 0;
	if (ret < 0)
		return ret;

	while (ret == 0) {
		struct extent_buffer *leaf = path.nodes[0];
		u32 item_start = btrfs_item_ptr_offset(leaf, path.slots[0]);
		u32 item_len = btrfs_item_size_nr(leaf, path.slots[0]);
		u32 cur = item_start;

		/* One item may hold several xattrs with colliding hashes */
		while (cur < item_start + item_len) {
			struct btrfs_dir_item *di;
			u16 name_len;
			u32 this_size;

			di = (struct btrfs_dir_item *)(long)cur;
			if (item_start + item_len - cur < sizeof(*di))
				goto corrupted;
			name_len = btrfs_dir_name_len(leaf, di);
			this_size = sizeof(*di) + name_len +
				    btrfs_dir_data_len(leaf, di);
			if (this_size > item_start + item_len - cur)
				goto corrupted;
			if (buf) {
				if (total + name_len + 1 > size) {
					btrfs_release_path(&path);
					return -ERANGE;
				}
				read_extent_buffer(leaf, buf + total,
						   cur + sizeof(*di), name_len);
				buf[total + name_len] = '\0';
			}
			total += name_len + 1;
			cur += this_size;
		}
		ret = btrfs_search_keys_next(&path, &range);
	}
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return total;

corrupted:
	btrfs_release_path(&path);
	return -EUCLEAN;
}

ssize_t btrfs_getxattr(struct btrfs_fs_info *fs_info,
		       struct btrfs_inode *inode, const char *name,
		       void *buf, size_t size)
{
	struct btrfs_dir_item *di;
	struct btrfs_path path;
	struct btrfs_key key;
	size_t name_len = strlen(name);
	u16 data_len;
	long offset;
	int ret;

	if (name_len == 0 || name_len > BTRFS_NAME_LEN)
		return -ENODATA;

	btrfs_init_path(&path);
	key.objectid = inode->ino;
	key.type = BTRFS_XATTR_ITEM_KEY;
	key.offset = btrfs_name_hash(name, name_len);

	ret = btrfs_search_key(inode->root, &path, &key);
	if (ret == -ENOENT)
		return -ENODATA;
	if (ret < 0)
		return ret;

	offset = find_dir_item_by_name(path.nodes[0], path.slots[0], name,
				       name_len);
	if (offset < 0) {
		btrfs_release_path(&path);
		return offset == -ENOENT ? -ENODATA : offset;
	}
	di = (struct btrfs_dir_item *)offset;
	data_len = btrfs_dir_data_len(path.nodes[0], di);
	if (buf) {
		if (data_len > size) {
			btrfs_release_path(&path);
			return -ERANGE;
		}
		read_extent_buffer(path.nodes[0], buf,
				   offset + sizeof(*di) + name_len, data_len);
	}
	btrfs_release_path(&path);
	return data_len;
}
