/*
 * Copyright (C) 2003-2011 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

/** \file exec.c Helper functions for spawning new processes. */

#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#include "gcc-compat.h"
#include "log.h"
#include "err.h"
#include "str.h"

/**
 * Spawn a new process using execvp().
 *
 * \param pid Will hold the pid of the created process upon return.
 * \param file Path of the executable to execute.
 * \param args The argument array for the command.
 *
 * \return Standard.
 *
 * \sa fork(2), exec(3).
 */
void dss_exec(pid_t *pid, const char *file, char *const *const args)
{
	if ((*pid = fork()) < 0) {
		DSS_EMERG_LOG("fork error: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (*pid) /* parent */
		return;
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	execvp(file, args);
	DSS_EMERG_LOG("execvp error: %s\n", strerror(errno));
	_exit(EXIT_FAILURE);
}

/**
 * Exec the command given as a command line.
 *
 * \param pid Will hold the pid of the created process upon return.
 * \param cmdline Holds the command and its arguments, seperated by spaces.
 *
 * This function uses fork/exec to create a new process.
 *
 * \return Standard.
 */
void dss_exec_cmdline_pid(pid_t *pid, const char *cmdline)
{
	char **argv, *tmp = dss_strdup(cmdline);

	split_args(tmp, &argv, " \t");
	dss_exec(pid, argv[0], argv);
	free(argv);
	free(tmp);
}
