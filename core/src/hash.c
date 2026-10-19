// SPDX-License-Identifier: MIT

#include <CommonCrypto/CommonDigest.h>
#include "ondisk_format.h"
#include "messages.h"
#include "crc32c.h"
#include "blake2b.h"
#include "hash.h"

/* xxHash comes from the vendored zstd copy, which namespaces it with ZSTD_ */
#define XXH_NAMESPACE ZSTD_
#define XXH_STATIC_LINKING_ONLY
#include "../vendor/zstd/common/xxhash.h"

static int hash_crc32c(const u8* buf, size_t length, u8 *out)
{
	u32 crc;

	crc = crc32c(~0U, buf, length);
	put_unaligned_le32(~crc, out);
	return 0;
}

static int hash_xxhash(const u8* buf, size_t length, u8 *out)
{
	XXH64_hash_t hash;

	hash = XXH64(buf, length, 0);
	put_unaligned_le64(hash, out);
	return 0;
}

static int hash_sha256(const u8* buf, size_t length, u8 *out)
{
	CC_SHA256(buf, (CC_LONG)length, out);
	return 0;
}

static int hash_blake2b(const u8* buf, size_t length, u8 *out)
{
	struct blake2b_state S;

	blake2b_init(&S, BTRFS_CSUM_SIZE);
	blake2b_update(&S, buf, length);
	blake2b_final(&S, out);
	return 0;
}

int btrfs_csum_data(u16 csum_type, const u8 *data, u8 *out, size_t len)
{
	memset(out, 0, BTRFS_CSUM_SIZE);
	switch(csum_type) {
	case BTRFS_CSUM_TYPE_CRC32:
		return hash_crc32c(data, len, out);
	case BTRFS_CSUM_TYPE_XXHASH:
		return hash_xxhash(data, len, out);
	case BTRFS_CSUM_TYPE_SHA256:
		return hash_sha256(data, len, out);
	case BTRFS_CSUM_TYPE_BLAKE2:
		return hash_blake2b(data, len, out);
	default:
		error("unknown csum type: %d", csum_type);
	}
	return -EINVAL;
}
