/** \file exec.c Helper functions for spawning new processes. */

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>


#include "gcc-compat.h"
#include "error.h"
#include "string.h"


/**
 * Spawn a new process and redirect fd 0, 1, and 2.
 *
 * \param pid Will hold the pid of the created process upon return.
 * \param file Path of the executable to execute.
 * \param args The argument array for the command.
 * \param fds a Pointer to a value-result array.
 *
 * \return Standard.
 *
 * \sa null(4), pipe(2), dup2(2), fork(2), exec(3).
 */
int dss_exec(pid_t *pid, const char *file, char *const *const args, int *fds)
{
	int ret, in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1},
		null = -1; /* ;) */

	ret = -E_DUP_PIPE;
	if (fds[0] > 0 && pipe(in) < 0)
		goto err_out;
	if (fds[1] > 0 && pipe(out) < 0)
		goto err_out;
	if (fds[2] > 0 && pipe(err) < 0)
		goto err_out;
	if (!fds[0] || !fds[1] || !fds[2]) {
		ret = -E_NULL_OPEN;
		null = open("/dev/null", O_RDONLY);
		if (null < 0)
			goto err_out;
	}
	if ((*pid = fork()) < 0)
		exit(EXIT_FAILURE);
	if (!(*pid)) { /* child */
		if (fds[0] >= 0) {
			if (fds[0]) {
				close(in[1]);
				if (in[0] != STDIN_FILENO)
					dup2(in[0], STDIN_FILENO);
			} else
				dup2(null, STDIN_FILENO);
		}
		if (fds[1] >= 0) {
			if (fds[1]) {
				close(out[0]);
				if (out[1] != STDOUT_FILENO)
					dup2(out[1], STDOUT_FILENO);
			} else
				dup2(null, STDOUT_FILENO);
		}
		if (fds[2] >= 0) {
			if (fds[2]) {
				close(err[0]);
				if (err[1] != STDERR_FILENO)
					dup2(err[1], STDERR_FILENO);
			} else
				dup2(null, STDERR_FILENO);
		}
		if (null >= 0)
			close(null);
		execvp(file, args);
		_exit(EXIT_FAILURE);
	}
	if (fds[0] > 0) {
		close(in[0]);
		*fds = in[1];
	}
	if (fds[1] > 0) {
		close(out[1]);
		*(fds + 1) = out[0];
	}
	if (fds[2] > 0) {
		close(err[1]);
		*(fds + 2) = err[0];
	}
	if (null >= 0)
		close(null);
	return 1;
err_out:
	make_err_msg("failed to exec %s", file);
	if (err[0] >= 0)
		close(err[0]);
	if (err[1] >= 0)
		close(err[1]);
	if (out[0] >= 0)
		close(out[0]);
	if (out[1] >= 0)
		close(out[1]);
	if (in[0] >= 0)
		close(in[0]);
	if (in[1] >= 0)
		close(in[1]);
	if (null >= 0)
		close(null);
	return ret;
}
