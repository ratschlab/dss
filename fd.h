int dss_chdir(const char *path);
int for_each_subdir(int (*func)(const char *, void *), void *private_data);
__must_check int mark_fd_nonblocking(int fd);
/**
 * A wrapper for rename(2).
 *
 * \param old_path The source path.
 * \param new_path The destination path.
 *
 * \return Standard.
 *
 * \sa rename(2).
 */
_static_inline_ int dss_rename(const char *old_path, const char *new_path)
{
	if (rename(old_path, new_path) >= 0)
		return 1;
	return -ERRNO_TO_DSS_ERROR(errno);
}

int dss_select(int n, fd_set *readfds, fd_set *writefds,
		struct timeval *timeout_tv);
