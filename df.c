/*
 * Copyright (C) 2008-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */
#include <sys/statvfs.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>

#include "gcc-compat.h"
#include "log.h"
#include "error.h"
#include "string.h"
#include "df.h"

int get_disk_space(const char *path, struct disk_space *result)
{
	/* using floats allows to not care about integer overflows */
	float total_blocks, available_blocks, blocksize;
	float total_inodes, available_inodes;

	struct statvfs vfs;
	int ret = statvfs(path, &vfs);
	if (ret < 0)
		return -ERRNO_TO_DSS_ERROR(errno);

	available_blocks = vfs.f_bavail;
	blocksize = vfs.f_bsize;
	total_blocks = vfs.f_blocks;
	total_inodes = vfs.f_files;
	available_inodes = vfs.f_ffree;

	result->total_mb = total_blocks * blocksize / 1024.0 / 1024.0;
	result->free_mb =  available_blocks * blocksize / 1024.0 / 1024.0;
	result->percent_free = 100.0 * available_blocks / total_blocks + 0.5;
	result->percent_free_inodes = 100.0 * available_inodes / total_inodes + 0.5;
	return 1;
}

void log_disk_space(struct disk_space *ds)
{
	DSS_INFO_LOG("free: %uM/%uM (%u%%), %u%% inodes unused\n",
		ds->free_mb, ds->total_mb, ds->percent_free,
		ds->percent_free_inodes);
}
