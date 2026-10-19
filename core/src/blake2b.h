// SPDX-License-Identifier: MIT

#ifndef BTRFS_RO_BLAKE2B_H
#define BTRFS_RO_BLAKE2B_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct blake2b_state {
	uint64_t h[8];
	uint64_t t[2];
	uint8_t buf[128];
	size_t buflen;
	size_t outlen;
};

void blake2b_init(struct blake2b_state *S, size_t outlen);
void blake2b_update(struct blake2b_state *S, const void *in, size_t inlen);
void blake2b_final(struct blake2b_state *S, void *out);

#endif
