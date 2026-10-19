// SPDX-License-Identifier: MIT

#ifndef BTRFS_FUSE_SUPER_H
#define BTRFS_FUSE_SUPER_H

#include "ctree.h"
#include "volumes.h"

int btrfs_check_super(struct btrfs_super_block *sb);

/* Read and check the primary superblock */
int btrfs_read_super(const struct btrfsro_io *io, struct btrfs_super_block *sb);

/* The equivalent to open_ctree() of kernel/progs */
struct btrfs_fs_info *btrfs_mount(const struct btrfsro_io *io);

/* The equivalent to close_ctree() of kernel/progs */
void btrfs_unmount(struct btrfs_fs_info *fs_info);

#endif
