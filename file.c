/*
 * Copyright (C) 2006-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>


#include "gcc-compat.h"
#include "err.h"
#include "str.h"

/**
 * Call a function for each subdirectory of the current working directory.
 *
 * \param dirname The directory to traverse.
 * \param func The function to call for each subdirecrtory.
 * \param private_data Pointer to an arbitrary data structure.
 *
 * For each top-level directory under \a dirname, the supplied function \a func is
 * called.  The full path of the subdirectory and the \a private_data pointer
 * are passed to \a func.
 *
 * \return This function returns immediately if \a func returned a negative
 * value. In this case \a func must set error_txt and this negative value is
 * returned to the caller. Otherwise the function returns when all
 * subdirectories have been passed to \a func.
 */

int for_each_subdir(int (*func)(const char *, void *), void *private_data)
{
	struct dirent *entry;
	int ret;
	DIR *dir = opendir(".");

	if (!dir)
		return -ERRNO_TO_DSS_ERROR(errno);
	while ((entry = readdir(dir))) {
		mode_t m;
		struct stat s;

		if (!strcmp(entry->d_name, "."))
			continue;
		if (!strcmp(entry->d_name, ".."))
			continue;
		ret = lstat(entry->d_name, &s) == -1;
		if (ret == -1) {
			ret = -ERRNO_TO_DSS_ERROR(errno);
			goto out;
		}
		m = s.st_mode;
		if (!S_ISDIR(m))
			continue;
		ret = func(entry->d_name, private_data);
		if (ret < 0)
			goto out;
	}
	ret = 1;
out:
	closedir(dir);
	return ret;
}
/**
 * Wrapper for chdir(2).
 *
 * \param path The specified directory.
 *
 * \return Standard.
 */
int dss_chdir(const char *path)
{
	if (chdir(path) >= 0)
		return 1;
	return -ERRNO_TO_DSS_ERROR(errno);
}

/**
 * Set a file descriptor to non-blocking mode.
 *
 * \param fd The file descriptor.
 *
 * \return Standard.
 */
__must_check int mark_fd_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	flags = fcntl(fd, F_SETFL, ((long)flags) | O_NONBLOCK);
	if (flags < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	return 1;
}

/**
 * dss' wrapper for select(2).
 *
 * It calls select(2) (with no exceptfds) and starts over if select() was
 * interrupted by a signal.
 *
 * \param n The highest-numbered descriptor in any of the two sets, plus 1.
 * \param readfds fds that should be checked for readability.
 * \param writefds fds that should be checked for writablility.
 * \param timeout_tv upper bound on the amount of time elapsed before select()
 * returns.
 *
 * \return The return value of the underlying select() call on success, the
 * negative system error code on errors.
 *
 * All arguments are passed verbatim to select(2).
 * \sa select(2) select_tut(2).
 */
int dss_select(int n, fd_set *readfds, fd_set *writefds,
		struct timeval *timeout_tv)
{
	int ret, err;
	do {
		ret = select(n, readfds, writefds, NULL, timeout_tv);
		err = errno;
	} while (ret < 0 && err == EINTR);
	if (ret < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	return ret;
}
