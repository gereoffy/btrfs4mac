// SPDX-License-Identifier: MIT

#ifndef BTRFS_RO_CRC32C_H
#define BTRFS_RO_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/* Raw CRC32C update: no initial or final inversion. */
uint32_t crc32c(uint32_t crc, const void *data, size_t len);

#endif
