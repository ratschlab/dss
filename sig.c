/*
 * Copyright (C) 2004-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>


#include "gcc-compat.h"
#include "err.h"
#include "log.h"
#include "str.h"
#include "file.h"
#include "sig.h"

static int signal_pipe[2];

/**
 * Initialize the signal subsystem.
 *
 * This function creates a pipe, the signal pipe, to deliver pending signals to
 * the application (Bernstein's trick). It should be called during the
 * application's startup part, followed by subsequent calls to
 * install_sighandler() for each signal that should be caught.
 *
 * signal_init() installs a generic signal handler which is used for all
 * signals simultaneously. When a signal arrives, this generic signal handler
 * writes the corresponding signal number to the signal pipe so that the
 * application can test for pending signals simply by checking the signal pipe
 * for reading, e.g. by using the select(2) system call.
 *
 * \return This function either succeeds or calls exit(2) to terminate
 * the current process. On success, the file descriptor of the signal pipe is
 * returned.
 */
int signal_init(void)
{
	int ret;
	if (pipe(signal_pipe) < 0) {
		ret = -ERRNO_TO_DSS_ERROR(errno);
		goto err_out;
	}
	ret = mark_fd_nonblocking(signal_pipe[0]);
	if (ret < 0)
		goto err_out;
	ret = mark_fd_nonblocking(signal_pipe[1]);
	if (ret < 0)
		goto err_out;
	return signal_pipe[0];
err_out:
	DSS_EMERG_LOG(("%s\n", dss_strerror(-ret)));
	exit(EXIT_FAILURE);
}

/*
 * just write one integer to signal pipe
 */
static void generic_signal_handler(int s)
{
	write(signal_pipe[1], &s, sizeof(int));
}

/**
 * Reap one child.
 *
 * \param pid In case a child died, its pid is returned here.
 *
 * Call waitpid() and print a log message containing the pid and the cause of
 * the child's death.
 *
 * \return A (negative) error code on errors, zero, if no child died, one
 * otherwise. If and only if the function returns one, the content of \a pid is
 * meaningful.
 *
 * \sa waitpid(2)
 */
int reap_child(pid_t *pid, int *status)
{
	*pid = waitpid(-1, status, WNOHANG);

	if (!*pid)
		return 0;
	if (*pid < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	if (WIFEXITED(*status))
		DSS_DEBUG_LOG(("child %i exited. Exit status: %i\n", (int)*pid,
			WEXITSTATUS(*status)));
	else if (WIFSIGNALED(*status))
		DSS_DEBUG_LOG(("child %i was killed by signal %i\n", (int)*pid,
			WTERMSIG(*status)));
	else
		DSS_WARNING_LOG(("child %i terminated abormally\n", (int)*pid));
	return 1;
}

/**
 * Wrapper around signal(2)
 *
 * \param sig The number of the signal to catch.
 *
 * This installs the generic signal handler for the given signal.
 *
 * \return This function returns 1 on success and \p -E_SIGNAL_SIG_ERR on errors.
 * \sa signal(2)
 */
int install_sighandler(int sig)
{
	DSS_DEBUG_LOG(("catching signal %d\n", sig));
	if (signal(sig, &generic_signal_handler) != SIG_ERR)
		return 1;
	return -E_SIGNAL_SIG_ERR;
}

/**
 * Return number of next pending signal.
 *
 * This should be called if the fd for the signal pipe is ready for reading.
 *
 * \return On success, the number of the received signal is returned.
 * If the read was interrupted by another signal the function returns 0.
 * Otherwise a negative error code is returned.
 */
int next_signal(void)
{
	int s, err;
	ssize_t r;

	r = read(signal_pipe[0], &s, sizeof(s));
	if (r == sizeof(s)) {
		DSS_DEBUG_LOG(("next signal: %d\n", s));
		return s;
	}
	err = errno;
	assert(r < 0);
	if (err == EAGAIN)
		return 0;
	DSS_ERROR_LOG(("failed to read from signal pipe\n"));
	return -ERRNO_TO_DSS_ERROR(err);
}

/**
 * Close the signal pipe.
 */
void signal_shutdown(void)
{
	close(signal_pipe[1]);
}
