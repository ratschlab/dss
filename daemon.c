/*
 * Copyright (C) 1997-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

/** \file daemon.c Some helpers for programs that detach from the console. */

#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#include "gcc-compat.h"
#include "error.h"
#include "log.h"
#include "string.h"
#include "daemon.h"

/**
 * Do the usual stuff to become a daemon.
 *
 * Fork, become session leader, dup fd 0, 1, 2 to /dev/null.
 *
 * \sa fork(2), setsid(2), dup(2).
 */
void daemon_init(void)
{
	pid_t pid;
	int null;

	DSS_INFO_LOG("daemonizing\n");
	pid = fork();
	if (pid < 0)
		goto err;
	if (pid)
		exit(EXIT_SUCCESS); /* parent exits */
	/* become session leader */
	if (setsid() < 0)
		goto err;
	if (chdir("/") < 0)
		goto err;
	umask(0);
	null = open("/dev/null", O_RDONLY);
	if (null < 0)
		goto err;
	if (dup2(null, STDIN_FILENO) < 0)
		goto err;
	if (dup2(null, STDOUT_FILENO) < 0)
		goto err;
	if (dup2(null, STDERR_FILENO) < 0)
		goto err;
	close(null);
	return;
err:
	DSS_EMERG_LOG("fatal: %s\n", strerror(errno));
	exit(EXIT_FAILURE);
}

/**
 * fopen() the given file in append mode.
 *
 * \param logfile_name The name of the file to open.
 *
 * \return Either calls exit() or returns a valid file handle.
 */
FILE *open_log(const char *logfile_name)
{
	FILE *logfile;

	assert(logfile_name);
	logfile = fopen(logfile_name, "a");
	if (!logfile) {
		DSS_EMERG_LOG("can not open %s: %s\n", logfile_name,
			strerror(errno));
		exit(EXIT_FAILURE);
	}
	setlinebuf(logfile);
	return logfile;
}

/**
 * Close the log file of the daemon.
 *
 * \param logfile The log file handle.
 *
 * It's OK to call this with logfile == \p NULL.
 */
void close_log(FILE* logfile)
{
	if (!logfile)
		return;
	DSS_INFO_LOG("closing logfile\n");
	fclose(logfile);
}

/**
 * Log the startup message.
 */
void log_welcome(int loglevel)
{
	DSS_INFO_LOG("***** welcome to dss ******\n");
	DSS_DEBUG_LOG("using loglevel %d\n", loglevel);
}
