// SPDX-License-Identifier: MIT
/* Known answer tests for the checksum algorithms btrfs can use. */

#include <stdio.h>
#include <string.h>
#include "../core/src/hash.h"
#include "../core/src/ondisk_format.h"

static int check(const char *name, u16 type, const char *msg, size_t outlen,
		 const char *want)
{
	u8 out[BTRFS_CSUM_SIZE];
	char hex[BTRFS_CSUM_SIZE * 2 + 1];

	btrfs_csum_data(type, (const u8 *)msg, out, strlen(msg));
	for (size_t i = 0; i < outlen; i++)
		sprintf(hex + i * 2, "%02x", out[i]);
	printf("%-8s %s %s\n", name, hex, strcmp(hex, want) ? "FAIL" : "ok");
	return strcmp(hex, want) != 0;
}

int main(void)
{
	int fails = 0;

	/* crc32c("123456789") = 0xe3069283, stored little endian */
	fails += check("crc32c", BTRFS_CSUM_TYPE_CRC32, "123456789", 4,
		       "839206e3");
	/* XXH64("abc", seed 0) = 0x44bc2cf5ad770999, little endian */
	fails += check("xxhash", BTRFS_CSUM_TYPE_XXHASH, "abc", 8,
		       "990977adf52cbc44");
	fails += check("sha256", BTRFS_CSUM_TYPE_SHA256, "abc", 32,
		       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	/* BLAKE2b-256("abc") */
	fails += check("blake2b", BTRFS_CSUM_TYPE_BLAKE2, "abc", 32,
		       "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
	/* Two padding blocks for sha256 (56 bytes) */
	fails += check("sha256", BTRFS_CSUM_TYPE_SHA256,
		       "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		       32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	/* Multi-block input for blake2b: 200 x 'a' */
	{
		char msg[201];

		memset(msg, 'a', 200);
		msg[200] = '\0';
		fails += check("blake2b", BTRFS_CSUM_TYPE_BLAKE2, msg, 32,
			       "6b6e59aaf00eb730cf93de53560846722184bbd92f8368c21ffa95380c2f9fe6");
	}
	return fails ? 1 : 0;
}
