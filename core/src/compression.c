// SPDX-License-Identifier: MIT

#include <zlib.h>
#include <sys/param.h>
#include "../vendor/zstd/zstd.h"
#include "compression.h"
#include "messages.h"
#include "lzo.h"

/*
 * All decompressors fill exactly @output_len bytes: btrfs compressed extents
 * may decompress to less than ram_bytes (the tail is then zero) and the
 * compressed stream may carry padding after its end.
 */

static int decompress_zlib(char *input, u32 input_len, char *output,
			   u32 output_len)
{
	z_stream strm;
	int ret;

	memset(&strm, 0, sizeof(strm));
	ret = inflateInit(&strm);
	if (ret != Z_OK) {
		error("zlib init failed: %d", ret);
		return -EIO;
	}

	strm.avail_in = input_len;
	strm.next_in = (unsigned char *)input;
	strm.avail_out = output_len;
	strm.next_out = (unsigned char *)output;
	ret = inflate(&strm, Z_FINISH);
	inflateEnd(&strm);
	/* Output buffer full is fine: the extent may be truncated */
	if (ret != Z_STREAM_END && !(strm.avail_out == 0 &&
				     (ret == Z_OK || ret == Z_BUF_ERROR))) {
		error("zlib inflate failed: %d", ret);
		return -EIO;
	}
	memset(strm.next_out, 0, strm.avail_out);
	return 0;
}

static int decompress_zstd(char *input, u32 input_len, char *output,
			   u32 output_len)
{
	ZSTD_DCtx *dctx;
	ZSTD_inBuffer in = {
		.src	= input,
		.size	= input_len,
		.pos	= 0,
	};
	ZSTD_outBuffer out = {
		.dst	= output,
		.size	= output_len,
		.pos	= 0,
	};
	size_t zret;
	int ret = 0;

	dctx = ZSTD_createDCtx();
	if (!dctx) {
		error("failed to alloc zstd context");
		return -ENOMEM;
	}

	/* Loop until the frame ends or the output is full */
	do {
		zret = ZSTD_decompressStream(dctx, &out, &in);
		if (ZSTD_isError(zret)) {
			error("zstd decompress failed: %s",
			      ZSTD_getErrorName(zret));
			ret = -EIO;
			goto out;
		}
	} while (zret != 0 && out.pos < out.size && in.pos < in.size);

	if (zret != 0 && out.pos < out.size) {
		error("zstd frame incomplete");
		ret = -EIO;
		goto out;
	}
	memset(output + out.pos, 0, output_len - out.pos);
out:
	ZSTD_freeDCtx(dctx);
	return ret;
}

#define LZO_LEN		(4)

static inline u32 read_compress_length(const char *buf)
{
	__le32 dlen;

	memcpy(&dlen, buf, LZO_LEN);
	return le32_to_cpu(dlen);
}

/* Worst lzo compressed size */
static inline u32 lzo1x_worst_compress(u32 size)
{
	return (size + size / 16) + 64 + 3 + 2;
}

/*
 * Unlike zlib/zstd, lzo doesn't have its embedded stream format, thus
 * it relies on btrfs defined segment headers:
 *
 * 1.  Header
 *     Fixed size. LZO_LEN (4) bytes long, LE32.
 *     Records the total size (including the header) of compressed data.
 *
 * 2.  Segment(s)
 *     Variable size. Each segment includes one segment header, followed by data
 *     payload.
 *     One regular LZO compressed extent can have one or more segments.
 *     For inlined LZO compressed extent, only one segment is allowed.
 *     One segment represents at most one sector of uncompressed data.
 *
 * 2.1 Segment header
 *     Fixed size. LZO_LEN (4) bytes long, LE32.
 *     Records the total size of the segment (not including the header).
 *     Segment header never crosses sector boundary, thus it's possible to
 *     have at most 3 padding zeros at the end of the sector.
 *
 * 2.2 Data Payload
 *     Variable size. Size up limit should be lzo1x_worst_compress(sectorsize)
 *     which is 4419 for a 4KiB sectorsize.
 */
static int decompress_lzo(const struct btrfs_fs_info *fs_info, char *input,
			  u32 input_len, char *output, u32 output_len)
{
	const u32 sectorsize = fs_info->sectorsize;
	u32 len_in;
	u32 cur_in = 0;	/* Current offset inside @input */
	u32 cur_out = 0; /* current offset inside @output */
	u8 *seg_out;
	int ret = 0;

	if (input_len < LZO_LEN)
		return -EUCLEAN;
	len_in = read_compress_length(input);
	cur_in += LZO_LEN;

	/* Basic lzo header checks */
	if (len_in > MIN(BTRFS_MAX_COMPRESSED, input_len) || len_in < LZO_LEN) {
		error("invalid lzo header, lzo len %u compressed len %u",
			len_in, input_len);
		return -EUCLEAN;
	}

	seg_out = malloc(sectorsize);
	if (!seg_out)
		return -ENOMEM;

	while (cur_in < len_in && cur_out < output_len) {
		u32 seg_len;	/* length of the compressed segment */
		u32 sector_bytes_left;
		size_t out_len = sectorsize;
		u32 copy_len;

		/* Segment headers never cross sector boundaries */
		sector_bytes_left = sectorsize - (cur_in % sectorsize);
		if (sector_bytes_left < LZO_LEN) {
			cur_in += sector_bytes_left;
			continue;
		}
		if (len_in - cur_in < LZO_LEN) {
			ret = -EUCLEAN;
			break;
		}
		seg_len = read_compress_length(input + cur_in);
		cur_in += LZO_LEN;
		if (seg_len > lzo1x_worst_compress(sectorsize) ||
		    seg_len > len_in - cur_in) {
			error("invalid lzo segment len %u at %u", seg_len,
			      cur_in);
			ret = -EUCLEAN;
			break;
		}

		ret = lzo1x_decompress_safe((const u8 *)input + cur_in,
					    seg_len, seg_out, &out_len);
		if (ret < 0) {
#ifdef LZO_DEBUG
			fprintf(stderr, "lzo: len_in %u input_len %u output_len %u cur_in %u seg_len %u out_len %zu cur_out %u\n",
				len_in, input_len, output_len, cur_in, seg_len, out_len, cur_out);
			for (u32 i = 0; i < seg_len && i < 64; i++)
				fprintf(stderr, "%02x ", (u8)input[cur_in + i]);
			fprintf(stderr, "\n");
#endif
			error("lzo decompress failed: %d", ret);
			ret = -EIO;
			break;
		}
		cur_in += seg_len;

		copy_len = MIN(out_len, output_len - cur_out);
		memcpy(output + cur_out, seg_out, copy_len);
		cur_out += copy_len;
	}
	free(seg_out);
	if (ret < 0)
		return ret;
	memset(output + cur_out, 0, output_len - cur_out);
	return 0;
}

int btrfs_decompress(const struct btrfs_fs_info *fs_info,
		     char *input, u32 input_len,
		     char *output, u32 output_len, u8 compression)
{
	switch (compression) {
	case BTRFS_COMPRESS_ZLIB:
		return decompress_zlib(input, input_len, output, output_len);
	case BTRFS_COMPRESS_LZO:
		return decompress_lzo(fs_info, input, input_len, output,
				      output_len);
	case BTRFS_COMPRESS_ZSTD:
		return decompress_zstd(input, input_len, output, output_len);
	}

	error("invalid compression algorithm: %d", compression);
	return -EUCLEAN;
}
