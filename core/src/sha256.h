// SPDX-License-Identifier: MIT

#ifndef BTRFS_RO_SHA256_H
#define BTRFS_RO_SHA256_H

#include <stdint.h>
#include <stddef.h>

void sha256(const void *data, size_t len, uint8_t out[32]);

#endif
