// SPDX-License-Identifier: MIT
/*
 * LZO1X decompressor, written from the bitstream description in the Linux
 * kernel documentation (Documentation/staging/lzo.rst).
 *
 * Instruction set (state = literals copied by the previous instruction):
 *
 *   0000LLLL state 0: long literal run, len = 3 + (L ?: 15 + ext)
 *   0000DDSS state 1-3: 2 byte match, dist = (H << 2) + D + 1
 *   0000DDSS state 4: 3 byte match, dist = (H << 2) + D + 2049
 *   0001HLLL  len = 2 + (L ?: 7 + ext), dist = 16384 + (H << 14) + D (LE16)
 *             dist == 16384 is the end of stream
 *   001LLLLL  len = 2 + (L ?: 31 + ext), dist = D + 1 (LE16)
 *   01LDDDSS  len = 3 + L, dist = (H << 3) + D + 1
 *   1LLDDDSS  len = 5 + L, dist = (H << 3) + D + 1
 *
 * where ext is (number of zero bytes * 255 + the following non-zero byte)
 * and SS is the number of literals to copy after the match.
 */

#include <errno.h>
#include <string.h>
#include "lzo.h"

#define NEED_IN(n)	do { if ((size_t)(ip_end - ip) < (size_t)(n)) \
				goto input_overrun; } while (0)
#define NEED_OUT(n)	do { if ((size_t)(op_end - op) < (size_t)(n)) \
				goto output_overrun; } while (0)

int lzo1x_decompress_safe(const uint8_t *in, size_t in_len,
			  uint8_t *out, size_t *out_len)
{
	const uint8_t *ip = in;
	const uint8_t *const ip_end = in + in_len;
	uint8_t *op = out;
	uint8_t *const op_end = out + *out_len;
	size_t state = 0;
	size_t t;

	NEED_IN(3);

	/* The first byte may encode a literal run on its own */
	if (*ip > 17) {
		t = *ip++ - 17;
		NEED_IN(t);
		NEED_OUT(t);
		memcpy(op, ip, t);
		op += t;
		ip += t;
		state = t < 4 ? t : 4;
	}

	for (;;) {
		size_t len;
		size_t dist;
		size_t next_state;

		NEED_IN(1);
		t = *ip++;

		if (t < 16) {
			if (state == 0) {
				/* Long literal run */
				len = t;
				if (len == 0) {
					len = 15;
					for (;;) {
						NEED_IN(1);
						if (*ip)
							break;
						len += 255;
						ip++;
					}
					len += *ip++;
				}
				len += 3;
				NEED_IN(len);
				NEED_OUT(len);
				memcpy(op, ip, len);
				op += len;
				ip += len;
				state = 4;
				continue;
			}
			NEED_IN(1);
			next_state = t & 3;
			if (state != 4) {
				dist = ((size_t)*ip++ << 2) + ((t >> 2) & 3) + 1;
				len = 2;
			} else {
				dist = ((size_t)*ip++ << 2) + ((t >> 2) & 3) +
				       2049;
				len = 3;
			}
		} else if (t < 32) {
			size_t v;

			len = t & 7;
			if (len == 0) {
				len = 7;
				for (;;) {
					NEED_IN(1);
					if (*ip)
						break;
					len += 255;
					ip++;
				}
				len += *ip++;
			}
			len += 2;
			NEED_IN(2);
			v = ip[0] | ((size_t)ip[1] << 8);
			ip += 2;
			dist = 16384 + ((t & 8) << 11) + (v >> 2);
			next_state = v & 3;
			if (dist == 16384) {
				/* End of stream */
				*out_len = op - out;
				return 0;
			}
		} else if (t < 64) {
			size_t v;

			len = t & 31;
			if (len == 0) {
				len = 31;
				for (;;) {
					NEED_IN(1);
					if (*ip)
						break;
					len += 255;
					ip++;
				}
				len += *ip++;
			}
			len += 2;
			NEED_IN(2);
			v = ip[0] | ((size_t)ip[1] << 8);
			ip += 2;
			dist = (v >> 2) + 1;
			next_state = v & 3;
		} else {
			NEED_IN(1);
			if (t < 128)
				len = 3 + ((t >> 5) & 1);
			else
				len = 5 + ((t >> 5) & 3);
			dist = ((size_t)*ip++ << 3) + ((t >> 2) & 7) + 1;
			next_state = t & 3;
		}

		/* Copy the match, which may overlap the output */
		if (dist > (size_t)(op - out))
			goto lookbehind_overrun;
		NEED_OUT(len);
		{
			const uint8_t *m = op - dist;

			while (len--)
				*op++ = *m++;
		}

		/* Trailing literals */
		NEED_IN(next_state);
		NEED_OUT(next_state);
		for (t = 0; t < next_state; t++)
			*op++ = *ip++;
		state = next_state;
	}

input_overrun:
	*out_len = op - out;
	return -EIO;
output_overrun:
	*out_len = op - out;
	return -EOVERFLOW;
lookbehind_overrun:
	*out_len = op - out;
	return -EIO;
}
