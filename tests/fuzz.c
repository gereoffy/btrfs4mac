// SPDX-License-Identifier: MIT
/*
 * Corruption fuzzer for libbtrfsro.
 *
 * Every metadata-sized read from the image is mutated with some probability
 * and its checksum is recomputed afterwards, so the corruption gets past the
 * checksum verification and has to be caught by the parsers. After mounting,
 * the whole tree is walked (readdir, stat, readlink, xattrs, reading files).
 * Build with sanitizers (make fuzz) and any memory error or crash is a bug.
 *
 * With "data" as the last argument data checksums are not verified and
 * data reads are mutated as well, which feeds corrupted compressed extents to
 * the decompressors.
 *
 * usage: fuzz <image> <first-seed> <iterations> [max-entries] [data]
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "btrfsro.h"
#include "../core/src/crc32c.h"

#define SUPER_OFFSET	65536
#define SUPER_SIZE	4096

struct fuzz_dev {
	int fd;
	uint32_t nodesize;
	uint64_t seed;
	/* Probability of mutating one metadata block, in 1/1000 */
	unsigned int rate;
	unsigned long mutations;
	bool data;
};

static uint64_t splitmix(uint64_t *x)
{
	uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void fix_csum(uint8_t *buf, size_t len)
{
	uint32_t crc = ~crc32c(~0U, buf + 32, len - 32);

	memset(buf, 0, 32);
	buf[0] = crc;
	buf[1] = crc >> 8;
	buf[2] = crc >> 16;
	buf[3] = crc >> 24;
}

static void mutate(struct fuzz_dev *dev, uint8_t *buf, size_t len,
		   uint64_t offset)
{
	/* Deterministic per (seed, offset), so cached and re-read agree */
	uint64_t rng = dev->seed * 0x100000001B3ULL ^ offset;
	uint32_t nritems;
	size_t hot;
	int count;

	if (splitmix(&rng) % 1000 >= dev->rate)
		return;
	dev->mutations++;

	/* The header plus the item/key-pointer array is the interesting part */
	nritems = buf[96] | buf[97] << 8 | buf[98] << 16 | (uint32_t)buf[99] << 24;
	hot = 101 + (size_t)(nritems > 1000 ? 1000 : nritems) * 33;
	if (hot > len)
		hot = len;

	count = 1 + splitmix(&rng) % 4;
	for (int i = 0; i < count; i++) {
		uint64_t r = splitmix(&rng);
		size_t pos;

		if (r & 1)
			pos = 32 + (r >> 8) % (hot - 32);
		else
			pos = 32 + (r >> 8) % (len - 32);
		switch ((r >> 1) % 4) {
		case 0:
			buf[pos] ^= 1 << ((r >> 4) % 8);
			break;
		case 1:
			buf[pos] = 0xff;
			break;
		case 2:
			buf[pos] = 0;
			break;
		default:
			buf[pos] = (uint8_t)(r >> 40);
			break;
		}
	}
	fix_csum(buf, len);
}

static void mutate_data(struct fuzz_dev *dev, uint8_t *buf, size_t len,
			uint64_t offset)
{
	uint64_t rng = dev->seed * 0x9E3779B97F4A7C15ULL ^ offset;
	int count;

	if (splitmix(&rng) % 1000 >= dev->rate)
		return;
	dev->mutations++;
	count = 1 + splitmix(&rng) % 8;
	for (int i = 0; i < count; i++) {
		uint64_t r = splitmix(&rng);

		buf[(r >> 8) % len] = (r & 1) ? (uint8_t)(r >> 40) :
				      buf[(r >> 8) % len] ^ (1 << (r % 8));
	}
}

static int fuzz_read(void *ctx, void *buf, uint64_t offset, size_t len)
{
	struct fuzz_dev *dev = ctx;
	size_t done = 0;

	while (done < len) {
		ssize_t ret = pread(dev->fd, (char *)buf + done, len - done,
				    (off_t)(offset + done));
		if (ret <= 0)
			return ret < 0 ? -errno : -EIO;
		done += ret;
	}
	if (len == dev->nodesize ||
	    (offset == SUPER_OFFSET && len == SUPER_SIZE))
		mutate(dev, buf, len, offset);
	else if (dev->data)
		mutate_data(dev, buf, len, offset);
	return 0;
}

struct walk_state {
	btrfsro_fs *fs;
	unsigned long entries;
	unsigned long max_entries;
	unsigned long errors;
	int depth;
};

struct ent {
	struct btrfsro_inode ino;
};

struct ent_list {
	struct ent *e;
	size_t nr, max;
};

static int collect(void *ctx, const char *name, size_t name_len,
		   const struct btrfsro_inode *entry, uint64_t index)
{
	struct ent_list *l = ctx;

	if (l->nr == l->max) {
		l->max = l->max ? l->max * 2 : 32;
		l->e = realloc(l->e, l->max * sizeof(*l->e));
	}
	l->e[l->nr++].ino = *entry;
	/* Bound the work on corrupted huge directories */
	return l->nr >= 100000;
}

static void visit(struct walk_state *ws, const struct btrfsro_inode *ino)
{
	static char buf[256 * 1024];
	struct btrfsro_stat st;
	struct btrfsro_inode parent;
	ssize_t len;

	if (ws->entries++ >= ws->max_entries)
		return;
	if (btrfsro_stat(ws->fs, ino, &st) < 0) {
		ws->errors++;
		return;
	}
	len = btrfsro_listxattr(ws->fs, ino, buf, sizeof(buf));
	if (len > 0) {
		for (char *n = buf; n < buf + len; n += strlen(n) + 1) {
			char val[4096];

			btrfsro_getxattr(ws->fs, ino, n, val, sizeof(val));
		}
	}
	if (ino->type == BTRFSRO_FT_SYMLINK) {
		char target[4096];

		btrfsro_readlink(ws->fs, ino, target, sizeof(target));
	} else if (ino->type == BTRFSRO_FT_REG) {
		/* Head, a middle part and the tail */
		btrfsro_read(ws->fs, ino, 0, buf, 65536);
		if (st.size > 65536) {
			btrfsro_read(ws->fs, ino, st.size / 2 - 1000, buf, 70000);
			btrfsro_read(ws->fs, ino, st.size - 5000, buf, 10000);
		}
	} else if (ino->type == BTRFSRO_FT_DIR && ws->depth < 64) {
		struct ent_list l = {};

		btrfsro_parent(ws->fs, ino, &parent);
		if (btrfsro_readdir(ws->fs, ino, 0, collect, &l) < 0)
			ws->errors++;
		ws->depth++;
		for (size_t i = 0; i < l.nr; i++) {
			/* Corruption can create loops, do not revisit the dir */
			if (l.e[i].ino.ino == ino->ino &&
			    l.e[i].ino.subvol == ino->subvol)
				continue;
			visit(ws, &l.e[i].ino);
		}
		ws->depth--;
		free(l.e);
	}
}

int main(int argc, char **argv)
{
	struct fuzz_dev dev = {};
	uint8_t super[SUPER_SIZE];
	uint64_t first, iterations;
	unsigned long max_entries = 3000;
	unsigned long mounted = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: fuzz <image> <first-seed> <iterations> [max-entries]\n");
		return 2;
	}
	first = strtoull(argv[2], NULL, 0);
	iterations = strtoull(argv[3], NULL, 0);
	if (argc > 4)
		max_entries = strtoul(argv[4], NULL, 0);
	if (argc > 5 && !strcmp(argv[5], "data"))
		dev.data = true;

	dev.fd = open(argv[1], O_RDONLY);
	if (dev.fd < 0) {
		perror(argv[1]);
		return 1;
	}
	if (pread(dev.fd, super, sizeof(super), SUPER_OFFSET) != sizeof(super))
		return 1;
	dev.nodesize = super[0x94] | super[0x95] << 8 | super[0x96] << 16 |
		       (uint32_t)super[0x97] << 24;

	btrfsro_set_log_handler(NULL, NULL, -1);
	for (uint64_t seed = first; seed < first + iterations; seed++) {
		struct walk_state ws = { .max_entries = max_entries };
		struct btrfsro_io io = { .read = fuzz_read, .ctx = &dev };
		struct btrfsro_inode root;

		dev.seed = seed;
		/* Alternate between light and heavy corruption */
		dev.rate = (seed % 3 == 0) ? 300 : (seed % 3 == 1) ? 50 : 10;
		if (btrfsro_mount(&io, &ws.fs) < 0)
			continue;
		mounted++;
		if (dev.data)
			btrfsro_set_verify_data(ws.fs, 0);
		btrfsro_root(ws.fs, &root);
		visit(&ws, &root);
		btrfsro_unmount(ws.fs);
	}
	printf("%" PRIu64 " iterations, %lu mounted, %lu blocks mutated\n",
	       iterations, mounted, dev.mutations);
	close(dev.fd);
	return 0;
}
