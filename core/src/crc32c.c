// SPDX-License-Identifier: MIT
/*
 * CRC32C (Castagnoli), reflected polynomial 0x82F63B78, slicing-by-8.
 *
 * crc32c() follows the Linux kernel convention: the caller passes the seed and
 * no final inversion is applied.
 */

#include <pthread.h>
#include "crc32c.h"

static uint32_t crc_table[8][256];
static pthread_once_t crc_table_once = PTHREAD_ONCE_INIT;

static void crc32c_init_table(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t crc = i;

		for (int j = 0; j < 8; j++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78U : 0);
		crc_table[0][i] = crc;
	}
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t crc = crc_table[0][i];

		for (int t = 1; t < 8; t++) {
			crc = crc_table[0][crc & 0xff] ^ (crc >> 8);
			crc_table[t][i] = crc;
		}
	}
}

uint32_t crc32c(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	pthread_once(&crc_table_once, crc32c_init_table);

	while (len && ((uintptr_t)p & 7)) {
		crc = crc_table[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
		len--;
	}
	while (len >= 8) {
		uint32_t lo = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
				     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
		uint32_t hi = (uint32_t)p[4] | (uint32_t)p[5] << 8 |
			      (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;

		crc = crc_table[7][lo & 0xff] ^
		      crc_table[6][(lo >> 8) & 0xff] ^
		      crc_table[5][(lo >> 16) & 0xff] ^
		      crc_table[4][lo >> 24] ^
		      crc_table[3][hi & 0xff] ^
		      crc_table[2][(hi >> 8) & 0xff] ^
		      crc_table[1][(hi >> 16) & 0xff] ^
		      crc_table[0][hi >> 24];
		p += 8;
		len -= 8;
	}
	while (len--)
		crc = crc_table[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc;
}
