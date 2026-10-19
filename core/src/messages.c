// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdarg.h>
#include "messages.h"

static btrfsro_log_fn log_handler;
static void *log_ctx;
static int log_level = BTRFSRO_LOG_WARNING;

void btrfsro_set_log_handler(btrfsro_log_fn fn, void *ctx, int max_level)
{
	log_handler = fn;
	log_ctx = ctx;
	log_level = max_level;
}

void btrfs_msg(int level, const char *fmt, ...)
{
	static const char *prefix[] = {
		[BTRFSRO_LOG_ERROR] = "ERROR: ",
		[BTRFSRO_LOG_WARNING] = "WARNING: ",
		[BTRFSRO_LOG_INFO] = "INFO: ",
		[BTRFSRO_LOG_DEBUG] = "DEBUG: ",
	};
	char buf[512];
	va_list args;

	if (level > log_level)
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (log_handler)
		log_handler(log_ctx, level, buf);
	else
		fprintf(stderr, "%s%s\n", prefix[level], buf);
}
