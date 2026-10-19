// SPDX-License-Identifier: MIT

#ifndef BTRFS_RO_LZO_H
#define BTRFS_RO_LZO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Decompress one LZO1X stream with full bounds checking.
 * @out_len: in: size of @out, out: number of bytes produced.
 * Return 0 on success, negative errno on malformed input or overrun.
 */
int lzo1x_decompress_safe(const uint8_t *in, size_t in_len,
			  uint8_t *out, size_t *out_len);

#endif
