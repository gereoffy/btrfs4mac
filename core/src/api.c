// SPDX-License-Identifier: MIT
/*
 * Public libbtrfsro API, a thin layer over the btrfs-fuse derived internals.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include "../include/btrfsro.h"
#include "ctree.h"
#include "super.h"
#include "inode.h"
#include "data.h"
#include "metadata.h"
#include "messages.h"

/* Bounce buffer size for btrfsro_read() */
#define BTRFSRO_READ_CHUNK	(128 * 1024)

static int to_internal(btrfsro_fs *fs, const struct btrfsro_inode *in,
		       struct btrfs_inode *out)
{
	struct btrfs_root *root;

	if (in->subvol == fs->default_root->root_key.objectid) {
		root = fs->default_root;
	} else {
		if (!is_fstree(in->subvol))
			return -EINVAL;
		root = btrfs_read_root(fs, in->subvol);
		if (IS_ERR(root))
			return PTR_ERR(root);
	}
	out->root = root;
	out->ino = in->ino;
	out->file_type = in->type;
	return 0;
}

static void to_public(const struct btrfs_inode *in, struct btrfsro_inode *out)
{
	out->subvol = in->root->root_key.objectid;
	out->ino = in->ino;
	out->type = in->file_type;
}

static void fill_volinfo(const struct btrfs_super_block *sb,
			 struct btrfsro_volinfo *info)
{
	memset(info, 0, sizeof(*info));
	memcpy(info->fsid, sb->fsid, sizeof(info->fsid));
	memcpy(info->label, sb->label, sizeof(info->label) - 1);
	info->label[sizeof(info->label) - 1] = '\0';
	info->total_bytes = btrfs_super_total_bytes(sb);
	info->bytes_used = btrfs_super_bytes_used(sb);
	info->generation = btrfs_super_generation(sb);
	info->num_devices = btrfs_super_num_devices(sb);
	info->incompat_flags = btrfs_super_incompat_flags(sb);
	info->compat_ro_flags = btrfs_super_compat_ro_flags(sb);
	info->sectorsize = btrfs_super_sectorsize(sb);
	info->nodesize = btrfs_super_nodesize(sb);
	info->csum_type = btrfs_super_csum_type(sb);
}

int btrfsro_probe(const struct btrfsro_io *io, struct btrfsro_volinfo *info)
{
	struct btrfs_super_block *sb;
	int ret;

	sb = malloc(sizeof(*sb));
	if (!sb)
		return -ENOMEM;
	ret = btrfs_read_super(io, sb);
	if (ret == 0 && info)
		fill_volinfo(sb, info);
	free(sb);
	return ret;
}

int btrfsro_mount(const struct btrfsro_io *io, btrfsro_fs **fs_ret)
{
	struct btrfs_fs_info *fs_info;

	if (!io || !io->read || !fs_ret)
		return -EINVAL;
	fs_info = btrfs_mount(io);
	if (IS_ERR(fs_info))
		return PTR_ERR(fs_info);
	*fs_ret = fs_info;
	return 0;
}

void btrfsro_unmount(btrfsro_fs *fs)
{
	if (fs)
		btrfs_unmount(fs);
}

void btrfsro_get_volinfo(btrfsro_fs *fs, struct btrfsro_volinfo *info)
{
	fill_volinfo(&fs->super_copy, info);
	info->default_subvol = fs->default_root->root_key.objectid;
}

void btrfsro_set_verify_data(btrfsro_fs *fs, int enable)
{
	fs->verify_data = enable != 0;
}

void btrfsro_root(btrfsro_fs *fs, struct btrfsro_inode *root)
{
	root->subvol = fs->default_root->root_key.objectid;
	root->ino = fs->default_root->root_dirid;
	root->type = BTRFSRO_FT_DIR;
}

int btrfsro_lookup(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		   const char *name, size_t name_len,
		   struct btrfsro_inode *out)
{
	struct btrfs_inode idir;
	struct btrfs_inode found;
	int ret;

	ret = to_internal(fs, dir, &idir);
	if (ret < 0)
		return ret;
	if (memchr(name, '/', name_len))
		return -EINVAL;
	ret = btrfs_lookup_one_name(fs, &idir, name, name_len, &found);
	if (ret < 0)
		return ret;
	to_public(&found, out);
	return 0;
}

int btrfsro_resolve(btrfsro_fs *fs, const char *path,
		    struct btrfsro_inode *out)
{
	struct btrfs_inode found;
	int ret;

	ret = btrfs_resolve_path(fs, path, strlen(path), &found);
	if (ret < 0)
		return ret;
	to_public(&found, out);
	return 0;
}

int btrfsro_parent(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		   struct btrfsro_inode *out)
{
	struct btrfs_inode idir;
	struct btrfs_inode parent;
	int ret;

	ret = to_internal(fs, dir, &idir);
	if (ret < 0)
		return ret;
	ret = btrfs_lookup_parent(fs, &idir, &parent);
	if (ret < 0)
		return ret;
	to_public(&parent, out);
	return 0;
}

int btrfsro_stat(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		 struct btrfsro_stat *st)
{
	struct btrfs_inode iinode;
	int ret;

	ret = to_internal(fs, inode, &iinode);
	if (ret < 0)
		return ret;
	return btrfs_stat(fs, &iinode, st);
}

int btrfsro_readdir(btrfsro_fs *fs, const struct btrfsro_inode *dir,
		    uint64_t start, btrfsro_dir_fn fn, void *ctx)
{
	struct btrfs_iterate_dir_ctrl ctrl = {};
	struct btrfs_inode idir;
	int ret;

	ret = to_internal(fs, dir, &idir);
	if (ret < 0)
		return ret;

	ret = btrfs_iterate_dir_start(fs, &ctrl, &idir, start);
	if (ret == -ENOENT)
		return 0;
	if (ret < 0)
		return ret;

	while (ret == 0) {
		char name[BTRFS_NAME_LEN + 1];
		struct btrfs_inode entry = {};
		struct btrfsro_inode pub;
		size_t name_len;
		u64 index;

		ret = btrfs_iterate_dir_get_inode(fs, &ctrl, &entry, &index,
						  name, &name_len);
		if (ret < 0)
			break;
		name[name_len] = '\0';
		to_public(&entry, &pub);
		if (fn(ctx, name, name_len, &pub, index))
			break;
		ret = btrfs_iterate_dir_next(fs, &ctrl);
	}
	btrfs_iterate_dir_end(fs, &ctrl);
	return ret < 0 ? ret : 0;
}

ssize_t btrfsro_read(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		     uint64_t offset, void *buf, size_t len)
{
	const u32 sectorsize = fs->sectorsize;
	struct btrfs_inode iinode;
	struct btrfsro_stat st;
	char *bounce;
	size_t done = 0;
	int ret;

	ret = to_internal(fs, inode, &iinode);
	if (ret < 0)
		return ret;
	ret = btrfs_stat(fs, &iinode, &st);
	if (ret < 0)
		return ret;
	if (S_ISDIR(st.mode))
		return -EISDIR;
	if (!S_ISREG(st.mode))
		return -EINVAL;
	if (offset >= st.size || len == 0)
		return 0;
	len = MIN(len, st.size - offset);

	bounce = malloc(BTRFSRO_READ_CHUNK);
	if (!bounce)
		return -ENOMEM;

	while (done < len) {
		u64 cur = offset + done;
		u64 aligned = round_down(cur, (u64)sectorsize);
		u32 skip = (u32)(cur - aligned);
		u64 want = round_up(skip + (len - done), (u64)sectorsize);
		ssize_t got;
		size_t n;

		want = MIN(want, BTRFSRO_READ_CHUNK);
		got = btrfs_read_file(fs, &iinode, aligned, bounce, (u32)want);
		if (got < 0) {
			ret = (int)got;
			break;
		}
		if (got <= skip) {
			ret = -EIO;
			break;
		}
		n = MIN((size_t)(got - skip), len - done);
		memcpy((char *)buf + done, bounce + skip, n);
		done += n;
	}
	free(bounce);
	if (done == 0 && ret < 0)
		return ret;
	return done;
}

int btrfsro_readlink(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		     char *buf, size_t size)
{
	struct btrfs_inode iinode;
	int ret;

	ret = to_internal(fs, inode, &iinode);
	if (ret < 0)
		return ret;
	return btrfs_read_link(fs, &iinode, buf, size);
}

ssize_t btrfsro_listxattr(btrfsro_fs *fs, const struct btrfsro_inode *inode,
			  char *buf, size_t size)
{
	struct btrfs_inode iinode;
	int ret;

	ret = to_internal(fs, inode, &iinode);
	if (ret < 0)
		return ret;
	return btrfs_listxattr(fs, &iinode, buf, size);
}

ssize_t btrfsro_getxattr(btrfsro_fs *fs, const struct btrfsro_inode *inode,
			 const char *name, void *buf, size_t size)
{
	struct btrfs_inode iinode;
	int ret;

	ret = to_internal(fs, inode, &iinode);
	if (ret < 0)
		return ret;
	return btrfs_getxattr(fs, &iinode, name, buf, size);
}

int btrfsro_extents(btrfsro_fs *fs, const struct btrfsro_inode *inode,
		    btrfsro_extent_fn fn, void *ctx)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_key_range range;
	struct btrfs_inode iinode;
	struct btrfs_path path;
	int ret;

	ret = to_internal(fs, inode, &iinode);
	if (ret < 0)
		return ret;

	btrfs_init_path(&path);
	range.objectid = iinode.ino;
	range.type_start = range.type_end = BTRFS_EXTENT_DATA_KEY;
	range.offset_start = 0;
	range.offset_end = (u64)-1;
	ret = btrfs_search_keys_start(iinode.root, &path, &range);
	if (ret == -ENOENT)
		return 0;

	while (ret == 0) {
		struct extent_buffer *leaf = path.nodes[0];
		int slot = path.slots[0];
		struct btrfsro_extent ext = {};
		struct btrfs_key key;
		u32 item_size = btrfs_item_size_nr(leaf, slot);

		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (item_size < BTRFS_FILE_EXTENT_INLINE_DATA_START) {
			ret = -EUCLEAN;
			break;
		}
		fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
		ext.file_offset = key.offset;
		ext.type = btrfs_file_extent_type(leaf, fi);
		ext.compression = btrfs_file_extent_compression(leaf, fi);
		if (ext.type == BTRFS_FILE_EXTENT_INLINE) {
			ext.length = btrfs_file_extent_ram_bytes(leaf, fi);
			ext.disk_bytes = btrfs_file_extent_inline_item_len(leaf,
						btrfs_item_nr(slot));
		} else {
			if (item_size < sizeof(*fi)) {
				ret = -EUCLEAN;
				break;
			}
			ext.length = btrfs_file_extent_num_bytes(leaf, fi);
			ext.disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
			ext.disk_bytes = btrfs_file_extent_disk_num_bytes(leaf, fi);
			ext.extent_offset = btrfs_file_extent_offset(leaf, fi);
		}
		if (fn(ctx, &ext)) {
			ret = 1;
			break;
		}
		ret = btrfs_search_keys_next(&path, &range);
	}
	btrfs_release_path(&path);
	return ret < 0 ? ret : 0;
}
