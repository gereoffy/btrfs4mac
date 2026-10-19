// SPDX-License-Identifier: MIT
/*
 * BLAKE2b (RFC 7693), unkeyed, written for btrfs checksum verification.
 */

#include <string.h>
#include "blake2b.h"

static const uint64_t blake2b_iv[8] = {
	0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL,
	0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL,
	0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
	0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL,
};

static const uint8_t blake2b_sigma[12][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
};

static inline uint64_t rotr64(uint64_t x, int n)
{
	return (x >> n) | (x << (64 - n));
}

static inline uint64_t load64_le(const uint8_t *p)
{
	uint64_t v = 0;

	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

#define G(a, b, c, d, x, y)			\
	do {					\
		v[a] = v[a] + v[b] + (x);	\
		v[d] = rotr64(v[d] ^ v[a], 32);	\
		v[c] = v[c] + v[d];		\
		v[b] = rotr64(v[b] ^ v[c], 24);	\
		v[a] = v[a] + v[b] + (y);	\
		v[d] = rotr64(v[d] ^ v[a], 16);	\
		v[c] = v[c] + v[d];		\
		v[b] = rotr64(v[b] ^ v[c], 63);	\
	} while (0)

static void blake2b_compress(struct blake2b_state *S, const uint8_t *block,
			     bool last)
{
	uint64_t m[16];
	uint64_t v[16];

	for (int i = 0; i < 16; i++)
		m[i] = load64_le(block + i * 8);
	for (int i = 0; i < 8; i++) {
		v[i] = S->h[i];
		v[i + 8] = blake2b_iv[i];
	}
	v[12] ^= S->t[0];
	v[13] ^= S->t[1];
	if (last)
		v[14] = ~v[14];

	for (int r = 0; r < 12; r++) {
		const uint8_t *s = blake2b_sigma[r];

		G(0, 4, 8, 12, m[s[0]], m[s[1]]);
		G(1, 5, 9, 13, m[s[2]], m[s[3]]);
		G(2, 6, 10, 14, m[s[4]], m[s[5]]);
		G(3, 7, 11, 15, m[s[6]], m[s[7]]);
		G(0, 5, 10, 15, m[s[8]], m[s[9]]);
		G(1, 6, 11, 12, m[s[10]], m[s[11]]);
		G(2, 7, 8, 13, m[s[12]], m[s[13]]);
		G(3, 4, 9, 14, m[s[14]], m[s[15]]);
	}
	for (int i = 0; i < 8; i++)
		S->h[i] ^= v[i] ^ v[i + 8];
}

void blake2b_init(struct blake2b_state *S, size_t outlen)
{
	memset(S, 0, sizeof(*S));
	for (int i = 0; i < 8; i++)
		S->h[i] = blake2b_iv[i];
	/* Parameter block: digest length, key length 0, fanout 1, depth 1 */
	S->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;
	S->outlen = outlen;
}

void blake2b_update(struct blake2b_state *S, const void *in, size_t inlen)
{
	const uint8_t *p = in;

	while (inlen > 0) {
		if (S->buflen == sizeof(S->buf)) {
			S->t[0] += sizeof(S->buf);
			if (S->t[0] < sizeof(S->buf))
				S->t[1]++;
			blake2b_compress(S, S->buf, false);
			S->buflen = 0;
		}
		size_t n = sizeof(S->buf) - S->buflen;
		if (n > inlen)
			n = inlen;
		memcpy(S->buf + S->buflen, p, n);
		S->buflen += n;
		p += n;
		inlen -= n;
	}
}

void blake2b_final(struct blake2b_state *S, void *out)
{
	uint8_t full[64];

	S->t[0] += S->buflen;
	if (S->t[0] < S->buflen)
		S->t[1]++;
	memset(S->buf + S->buflen, 0, sizeof(S->buf) - S->buflen);
	blake2b_compress(S, S->buf, true);

	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++)
			full[i * 8 + j] = (uint8_t)(S->h[i] >> (8 * j));
	memcpy(out, full, S->outlen);
}
