// SPDX-License-Identifier: MIT

#ifndef BTRFS_FUSE_MESSAGES_H
#define BTRFS_FUSE_MESSAGES_H

#include "../include/btrfsro.h"

__attribute__ ((format (printf, 2, 3)))
void btrfs_msg(int level, const char *fmt, ...);

#define error(...)	btrfs_msg(BTRFSRO_LOG_ERROR, __VA_ARGS__)
#define warning(...)	btrfs_msg(BTRFSRO_LOG_WARNING, __VA_ARGS__)
#define info(...)	btrfs_msg(BTRFSRO_LOG_INFO, __VA_ARGS__)
#define debug(...)	btrfs_msg(BTRFSRO_LOG_DEBUG, __VA_ARGS__)

#endif
