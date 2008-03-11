#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>


#include "gcc-compat.h"
#include "error.h"
#include "string.h"

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

	if (!dir) {
		ret = -ERRNO_TO_DSS_ERROR(errno);
		make_err_msg("opendir(\".\") failed");
		return ret;
	}
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
			make_err_msg("lstat(\"%s\") failed", entry->d_name);
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
	int ret = chdir(path);

	if (ret >= 0)
		return 1;
	ret = -ERRNO_TO_DSS_ERROR(errno);
	make_err_msg("chdir to %s failed", path);
	return ret;
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

