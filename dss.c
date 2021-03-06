/*
 * Copyright (C) 2008-2011 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/time.h>
#include <time.h>
#include <sys/wait.h>
#include <fnmatch.h>
#include <limits.h>


#include "gcc-compat.h"
#include "cmdline.h"
#include "log.h"
#include "str.h"
#include "err.h"
#include "file.h"
#include "exec.h"
#include "daemon.h"
#include "sig.h"
#include "df.h"
#include "tv.h"
#include "snap.h"
#include "ipc.h"

/** Command line and config file options. */
static struct gengetopt_args_info conf;
/** Non-NULL if we log to a file. */
static FILE *logfile;
/** The read end of the signal pipe */
static int signal_pipe;
/** Process id of current pre-create-hook/rsync/post-create-hook process. */
static pid_t create_pid;
/** Whether the pre-create-hook/rsync/post-create-hook is currently stopped. */
static int create_process_stopped;
/** Process id of current pre-remove/rm/post-remove process. */
static pid_t remove_pid;
/** When the next snapshot is due. */
static int64_t next_snapshot_time;
/** When to try to remove something. */
static struct timeval next_removal_check;
/** Creation time of the snapshot currently being created. */
static int64_t current_snapshot_creation_time;
/** The snapshot currently being removed. */
struct snapshot *snapshot_currently_being_removed;
/** Needed by the post-create hook. */
static char *path_to_last_complete_snapshot;
static char *name_of_reference_snapshot;
/** \sa \ref snap.h for details. */
enum hook_status snapshot_creation_status;
/** \sa \ref snap.h for details. */
enum hook_status snapshot_removal_status;


DEFINE_DSS_ERRLIST;
static const char *hook_status_description[] = {HOOK_STATUS_ARRAY};

/* may be called with ds == NULL. */
static int disk_space_low(struct disk_space *ds)
{
	struct disk_space ds_struct;

	if (!ds) {
		int ret = get_disk_space(".", &ds_struct);
		if (ret < 0)
			return ret;
		ds = &ds_struct;
	}
	if (conf.min_free_mb_arg)
		if (ds->free_mb < conf.min_free_mb_arg)
			return 1;
	if (conf.min_free_percent_arg)
		if (ds->percent_free < conf.min_free_percent_arg)
			return 1;
	if (conf.min_free_percent_inodes_arg)
		if (ds->percent_free_inodes < conf.min_free_percent_inodes_arg)
			return 1;
	return 0;
}

static void dump_dss_config(const char *msg)
{
	const char dash[] = "-----------------------------";
	int ret;
	FILE *log = logfile? logfile : stderr;
	struct disk_space ds;
	int64_t now = get_current_time();

	if (conf.loglevel_arg > INFO)
		return;

	fprintf(log, "%s <%s config> %s\n", dash, msg, dash);
	fprintf(log, "\n*** disk space ***\n\n");
	ret = get_disk_space(".", &ds);
	if (ret >= 0) {
		DSS_INFO_LOG(("disk space low: %s\n", disk_space_low(&ds)?
			"yes" : "no"));
		log_disk_space(&ds);
	} else
		DSS_ERROR_LOG(("can not get free disk space: %s\n",
			dss_strerror(-ret)));

	/* we continue on errors from get_disk_space */

	fprintf(log, "\n*** command line and config file options ***\n\n");
	cmdline_parser_dump(log, &conf);
	fprintf(log, "\n*** internal state ***\n\n");
	fprintf(log,
		"pid: %d\n"
		"logile: %s\n"
		"snapshot_currently_being_removed: %s\n"
		"path_to_last_complete_snapshot: %s\n"
		"reference_snapshot: %s\n"
		"snapshot_creation_status: %s\n"
		"snapshot_removal_status: %s\n"
		,
		(int) getpid(),
		logfile? conf.logfile_arg : "stderr",
		snapshot_currently_being_removed?
			snapshot_currently_being_removed->name : "(none)",
		path_to_last_complete_snapshot?
			path_to_last_complete_snapshot : "(none)",
		name_of_reference_snapshot?
			name_of_reference_snapshot : "(none)",
		hook_status_description[snapshot_creation_status],
		hook_status_description[snapshot_removal_status]
	);
	if (create_pid != 0)
		fprintf(log,
			"create_pid: %" PRId32 "\n"
			"create process is %sstopped\n"
			,
			create_pid,
			create_process_stopped? "" : "not "
		);
	if (remove_pid != 0)
		fprintf(log, "remove_pid: %" PRId32 "\n", remove_pid);
	if (next_snapshot_time != 0)
		fprintf(log, "next snapshot due in %" PRId64 " seconds\n",
			next_snapshot_time - now);
	if (current_snapshot_creation_time != 0)
		fprintf(log, "current_snapshot_creation_time: %"
			PRId64 " (%" PRId64 " seconds ago)\n",
			current_snapshot_creation_time,
			now - current_snapshot_creation_time
		);
	if (next_removal_check.tv_sec != 0) {
		fprintf(log, "next removal check: %llu (%llu seconds ago)\n",
			(long long unsigned)next_removal_check.tv_sec,
			now - (long long unsigned)next_removal_check.tv_sec
		);

	}
	fprintf(log, "%s </%s config> %s\n", dash, msg, dash);
}

/* a litte cpp magic helps to DRY */
#define COMMANDS \
	COMMAND(ls) \
	COMMAND(create) \
	COMMAND(prune) \
	COMMAND(run) \
	COMMAND(kill) \
	COMMAND(reload) \

#define COMMAND(x) static int com_ ##x(void);
COMMANDS
#undef COMMAND
#define COMMAND(x) if (conf.x ##_given) return com_ ##x();
static int call_command_handler(void)
{
	COMMANDS
	DSS_EMERG_LOG(("BUG: did not find command handler\n"));
	return -E_BUG;
}
#undef COMMAND
#undef COMMANDS

static int loglevel = -1;
static const char *location_file = NULL;
static int         location_line = -1;
static const char *location_func = NULL;

void dss_log_set_params(int ll, const char *file, int line, const char *func)
{
	loglevel = ll;
	location_file = file;
	location_line = line;
	location_func = func;
}

/**
 * The log function of dss.
 *
 * \param ll Loglevel.
 * \param fml Usual format string.
 *
 * All DSS_XXX_LOG() macros use this function.
 */
__printf_1_2 void dss_log(const char* fmt,...)
{
	va_list argp;
	FILE *outfd;
	struct tm *tm;
	time_t t1;
	char str[255] = "";

	if (loglevel < conf.loglevel_arg)
		return;
	outfd = logfile? logfile : stderr;
	time(&t1);
	tm = localtime(&t1);
	strftime(str, sizeof(str), "%b %d %H:%M:%S", tm);
	fprintf(outfd, "%s ", str);
	if (conf.loglevel_arg <= INFO)
		fprintf(outfd, "%i: ", loglevel);
#ifdef DSS_NO_FUNC_NAMES
	fprintf(outfd, "%s:%d: ", location_file, location_line);
#else
	fprintf(outfd, "%s: ", location_func);
#endif
	va_start(argp, fmt);
	vfprintf(outfd, fmt, argp);
	va_end(argp);
}

/**
 * Print a message either to stdout or to the log file.
 */
static __printf_1_2 void dss_msg(const char* fmt,...)
{
	FILE *outfd = conf.daemon_given? logfile : stdout;
	va_list argp;
	va_start(argp, fmt);
	vfprintf(outfd, fmt, argp);
	va_end(argp);
}

static char *get_config_file_name(void)
{
	char *home, *config_file;

	if (conf.config_file_given)
		return dss_strdup(conf.config_file_arg);
	home = get_homedir();
	config_file = make_message("%s/.dssrc", home);
	free(home);
	return config_file;
}

static int send_signal(int sig)
{
	pid_t pid;
	char *config_file = get_config_file_name();
	int ret = get_dss_pid(config_file, &pid);

	free(config_file);
	if (ret < 0)
		return ret;
	if (conf.dry_run_given) {
		dss_msg("%d\n", (int)pid);
		return 0;
	}
	ret = kill(pid, sig);
	if (ret < 0)
		return -ERRNO_TO_DSS_ERROR(errno);
	return 1;
}

static int com_kill(void)
{
	return send_signal(SIGTERM);
}

static int com_reload(void)
{
	return send_signal(SIGHUP);
}

static void dss_get_snapshot_list(struct snapshot_list *sl)
{
	get_snapshot_list(sl, conf.unit_interval_arg, conf.num_intervals_arg);
}

static int64_t compute_next_snapshot_time(void)
{
	int64_t x = 0, now = get_current_time(), unit_interval
		= 24 * 3600 * conf.unit_interval_arg, ret;
	unsigned wanted = desired_number_of_snapshots(0, conf.num_intervals_arg),
		num_complete_snapshots = 0;
	int i;
	struct snapshot *s = NULL;
	struct snapshot_list sl;

	dss_get_snapshot_list(&sl);
	FOR_EACH_SNAPSHOT(s, i, &sl) {
		if (!(s->flags & SS_COMPLETE))
			continue;
		num_complete_snapshots++;
		x += s->completion_time - s->creation_time;
	}
	assert(x >= 0);

	ret = now;
	if (num_complete_snapshots == 0)
		goto out;
	x /= num_complete_snapshots; /* avg time to create one snapshot */
	if (unit_interval < x * wanted) /* oops, no sleep at all */
		goto out;
	ret = s->completion_time + unit_interval / wanted - x;
out:
	free_snapshot_list(&sl);
	return ret;
}

static inline void invalidate_next_snapshot_time(void)
{
	next_snapshot_time = 0;
}

static inline int next_snapshot_time_is_valid(void)
{
	return next_snapshot_time != 0;
}

static int next_snapshot_is_due(void)
{
	int64_t now = get_current_time();

	if (!next_snapshot_time_is_valid())
		next_snapshot_time = compute_next_snapshot_time();
	if (next_snapshot_time <= now) {
		DSS_DEBUG_LOG(("next snapshot: now\n"));
		return 1;
	}
	DSS_DEBUG_LOG(("next snapshot due in %" PRId64 " seconds\n",
		next_snapshot_time - now));
	return 0;
}

static void pre_create_hook(void)
{
	assert(snapshot_creation_status == HS_READY);
	/* make sure that the next snapshot time will be recomputed */
	invalidate_next_snapshot_time();
	DSS_DEBUG_LOG(("executing %s\n", conf.pre_create_hook_arg));
	dss_exec_cmdline_pid(&create_pid, conf.pre_create_hook_arg);
	snapshot_creation_status = HS_PRE_RUNNING;
}

static void pre_remove_hook(struct snapshot *s, const char *why)
{
	char *cmd;

	if (!s)
		return;
	DSS_DEBUG_LOG(("%s snapshot %s\n", why, s->name));
	assert(snapshot_removal_status == HS_READY);
	assert(remove_pid == 0);
	assert(!snapshot_currently_being_removed);

	snapshot_currently_being_removed = dss_malloc(sizeof(struct snapshot));
	*snapshot_currently_being_removed = *s;
	snapshot_currently_being_removed->name = dss_strdup(s->name);

	cmd = make_message("%s %s/%s", conf.pre_remove_hook_arg,
		conf.dest_dir_arg, s->name);
	DSS_DEBUG_LOG(("executing %s\n", cmd));
	dss_exec_cmdline_pid(&remove_pid, cmd);
	free(cmd);
	snapshot_removal_status = HS_PRE_RUNNING;
}

static int exec_rm(void)
{
	struct snapshot *s = snapshot_currently_being_removed;
	char *new_name = being_deleted_name(s);
	char *argv[4];
	int ret;

	argv[0] = "rm";
	argv[1] = "-rf";
	argv[2] = new_name;
	argv[3] = NULL;

	assert(snapshot_removal_status == HS_PRE_SUCCESS);
	assert(remove_pid == 0);

	DSS_NOTICE_LOG(("removing %s (interval = %i)\n", s->name, s->interval));
	ret = dss_rename(s->name, new_name);
	if (ret < 0)
		goto out;
	dss_exec(&remove_pid, argv[0], argv);
	snapshot_removal_status = HS_RUNNING;
out:
	free(new_name);
	return ret;
}

static int snapshot_is_being_created(struct snapshot *s)
{
	return s->creation_time == current_snapshot_creation_time;
}

static struct snapshot *find_orphaned_snapshot(struct snapshot_list *sl)
{
	struct snapshot *s;
	int i;

	DSS_DEBUG_LOG(("looking for orphaned snapshots\n"));
	FOR_EACH_SNAPSHOT(s, i, sl) {
		if (snapshot_is_being_created(s))
			continue;
		/*
		 * We know that no rm is currently running, so if s is marked
		 * as being deleted, a previously started rm must have failed.
		 */
		if (s->flags & SS_BEING_DELETED)
			return s;

		if (s->flags & SS_COMPLETE) /* good snapshot */
			continue;
		/*
		 * This snapshot is incomplete and it is not the snapshot
		 * currently being created. However, we must not remove it if
		 * rsync is about to be restarted. As only the newest snapshot
		 * can be restarted, this snapshot is orphaned if it is not the
		 * newest snapshot or if we are not about to restart rsync.
		 */
		if (get_newest_snapshot(sl) != s)
			return s;
		if (snapshot_creation_status != HS_NEEDS_RESTART)
			return s;
	}
	/* no orphaned snapshots */
	return NULL;
}

static int is_reference_snapshot(struct snapshot *s)
{
	if (!name_of_reference_snapshot)
		return 0;
	return strcmp(s->name, name_of_reference_snapshot)? 0 : 1;
}

/*
 * return: 0: no redundant snapshots, 1: rm process started, negative: error
 */
static struct snapshot *find_redundant_snapshot(struct snapshot_list *sl)
{
	int i, interval;
	struct snapshot *s;
	unsigned missing = 0;

	DSS_DEBUG_LOG(("looking for intervals containing too many snapshots\n"));
	for (interval = conf.num_intervals_arg - 1; interval >= 0; interval--) {
		unsigned keep = desired_number_of_snapshots(interval, conf.num_intervals_arg);
		unsigned num = sl->interval_count[interval];
		struct snapshot *victim = NULL, *prev = NULL;
		int64_t score = LONG_MAX;

		if (keep >= num)
			missing += keep - num;
		if (keep + missing >= num)
			continue;
		/* redundant snapshot in this interval, pick snapshot with lowest score */
		FOR_EACH_SNAPSHOT(s, i, sl) {
			int64_t this_score;

			if (snapshot_is_being_created(s))
				continue;
			if (is_reference_snapshot(s))
				continue;
			if (s->interval > interval) {
				prev = s;
				continue;
			}
			if (s->interval < interval)
				break;
			if (!victim) {
				victim = s;
				prev = s;
				continue;
			}
			assert(prev);
			/* check if s is a better victim */
			this_score = s->creation_time - prev->creation_time;
			assert(this_score >= 0);
			if (this_score < score) {
				score = this_score;
				victim = s;
			}
			prev = s;
		}
		assert(victim);
		return victim;
	}
	return NULL;
}

static struct snapshot *find_outdated_snapshot(struct snapshot_list *sl)
{
	int i;
	struct snapshot *s;

	DSS_DEBUG_LOG(("looking for snapshots belonging to intervals >= %d\n",
		conf.num_intervals_arg));
	FOR_EACH_SNAPSHOT(s, i, sl) {
		if (snapshot_is_being_created(s))
			continue;
		if (is_reference_snapshot(s))
			continue;
		if (s->interval < conf.num_intervals_arg)
			continue;
		return s;
	}
	return NULL;
}

static struct snapshot *find_oldest_removable_snapshot(struct snapshot_list *sl)
{
	int i;
	struct snapshot *s;
	FOR_EACH_SNAPSHOT(s, i, sl) {
		if (snapshot_is_being_created(s))
			continue;
		if (is_reference_snapshot(s))
			continue;
		DSS_INFO_LOG(("oldest removable snapshot: %s\n", s->name));
		return s;
	}
	return NULL;
}

static int rename_incomplete_snapshot(int64_t start)
{
	char *old_name;
	int ret;
	int64_t now;

	/*
	 * We don't want the dss_rename() below to fail with EEXIST because the
	 * last complete snapshot was created (and completed) in the same
	 * second as this one.
	 */
	while ((now = get_current_time()) == start)
		sleep(1);
	free(path_to_last_complete_snapshot);
	ret = complete_name(start, now, &path_to_last_complete_snapshot);
	if (ret < 0)
		return ret;
	old_name = incomplete_name(start);
	ret = dss_rename(old_name, path_to_last_complete_snapshot);
	if (ret >= 0)
		DSS_NOTICE_LOG(("%s -> %s\n", old_name,
			path_to_last_complete_snapshot));
	free(old_name);
	return ret;
}

static int try_to_free_disk_space(void)
{
	int ret;
	struct snapshot_list sl;
	struct snapshot *victim;
	struct timeval now;
	const char *why;
	int low_disk_space;

	ret = disk_space_low(NULL);
	if (ret < 0)
		return ret;
	low_disk_space = ret;
	gettimeofday(&now, NULL);
	if (tv_diff(&next_removal_check, &now, NULL) > 0)
		return 0;
	if (!low_disk_space) {
		if (conf.keep_redundant_given)
			return 0;
		if (snapshot_creation_status != HS_READY)
			return 0;
		if (next_snapshot_is_due())
			return 0;
	}
	dss_get_snapshot_list(&sl);
	ret = 0;
	if (!low_disk_space && sl.num_snapshots <= 1)
		goto out;
	why = "outdated";
	victim = find_outdated_snapshot(&sl);
	if (victim)
		goto remove;
	why = "redundant";
	victim = find_redundant_snapshot(&sl);
	if (victim)
		goto remove;
	/* try harder only if disk space is low */
	if (!low_disk_space)
		goto out;
	why = "orphaned";
	victim = find_orphaned_snapshot(&sl);
	if (victim)
		goto remove;
	DSS_WARNING_LOG(("disk space low and nothing obvious to remove\n"));
	victim = find_oldest_removable_snapshot(&sl);
	if (victim)
		goto remove;
	DSS_CRIT_LOG(("uhuhu: disk space low and nothing to remove\n"));
	ret = -ERRNO_TO_DSS_ERROR(ENOSPC);
	goto out;
remove:
	pre_remove_hook(victim, why);
out:
	free_snapshot_list(&sl);
	return ret;
}

static void post_create_hook(void)
{
	char *cmd = make_message("%s %s/%s", conf.post_create_hook_arg,
		conf.dest_dir_arg, path_to_last_complete_snapshot);
	DSS_NOTICE_LOG(("executing %s\n", cmd));
	dss_exec_cmdline_pid(&create_pid, cmd);
	free(cmd);
	snapshot_creation_status = HS_POST_RUNNING;
}

static void post_remove_hook(void)
{
	char *cmd;
	struct snapshot *s = snapshot_currently_being_removed;

	assert(s);

	cmd = make_message("%s %s/%s", conf.post_remove_hook_arg,
		conf.dest_dir_arg, s->name);
	DSS_NOTICE_LOG(("executing %s\n", cmd));
	dss_exec_cmdline_pid(&remove_pid, cmd);
	free(cmd);
	snapshot_removal_status = HS_POST_RUNNING;
}

static void dss_kill(pid_t pid, int sig, const char *msg)
{
	const char *signame, *process_name;

	if (pid == 0)
		return;
	switch (sig) {
	case SIGTERM: signame = "TERM"; break;
	case SIGSTOP: signame = "STOP"; break;
	case SIGCONT: signame = "CONT"; break;
	default: signame = "????";
	}

	if (pid == create_pid)
		process_name = "create";
	else if (pid == remove_pid)
		process_name = "remove";
	else process_name = "??????";

	if (msg)
		DSS_INFO_LOG(("%s\n", msg));
	DSS_DEBUG_LOG(("sending signal %d (%s) to pid %d (%s process)\n",
		sig, signame, (int)pid, process_name));
	if (kill(pid, sig) >= 0)
		return;
	DSS_INFO_LOG(("failed to send signal %d (%s) to pid %d (%s process)\n",
		sig, signame, (int)pid, process_name));
}

static void stop_create_process(void)
{
	if (create_process_stopped)
		return;
	dss_kill(create_pid, SIGSTOP, "suspending create process");
	create_process_stopped = 1;
}

static void restart_create_process(void)
{
	if (!create_process_stopped)
		return;
	dss_kill(create_pid, SIGCONT, "resuming create process");
	create_process_stopped = 0;
}

/**
 * Print a log message about the exit status of a child.
 */
static void log_termination_msg(pid_t pid, int status)
{
	if (WIFEXITED(status))
		DSS_INFO_LOG(("child %i exited. Exit status: %i\n", (int)pid,
			WEXITSTATUS(status)));
	else if (WIFSIGNALED(status))
		DSS_NOTICE_LOG(("child %i was killed by signal %i\n", (int)pid,
			WTERMSIG(status)));
	else
		DSS_WARNING_LOG(("child %i terminated abormally\n", (int)pid));
}

static int wait_for_process(pid_t pid, int *status)
{
	int ret;

	DSS_DEBUG_LOG(("Waiting for process %d to terminate\n", (int)pid));
	for (;;) {
		fd_set rfds;

		FD_ZERO(&rfds);
		FD_SET(signal_pipe, &rfds);
		ret = dss_select(signal_pipe + 1, &rfds, NULL, NULL);
		if (ret < 0)
			break;
		ret = next_signal();
		if (!ret)
			continue;
		if (ret == SIGCHLD) {
			ret = waitpid(pid, status, 0);
			if (ret >= 0)
				break;
			if (errno != EINTR) { /* error */
				ret = -ERRNO_TO_DSS_ERROR(errno);
				break;
			}
		}
		/* SIGINT or SIGTERM */
		dss_kill(pid, SIGTERM, "killing child process");
	}
	if (ret < 0)
		DSS_ERROR_LOG(("failed to wait for process %d\n", (int)pid));
	else
		log_termination_msg(pid, *status);
	return ret;
}

static void handle_pre_remove_exit(int status)
{
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		snapshot_removal_status = HS_READY;
		gettimeofday(&next_removal_check, NULL);
		next_removal_check.tv_sec += 60;
		return;
	}
	snapshot_removal_status = HS_PRE_SUCCESS;
}

static int handle_rm_exit(int status)
{
	if (!WIFEXITED(status)) {
		snapshot_removal_status = HS_READY;
		return -E_INVOLUNTARY_EXIT;
	}
	if (WEXITSTATUS(status)) {
		snapshot_removal_status = HS_READY;
		return -E_BAD_EXIT_CODE;
	}
	snapshot_removal_status = HS_SUCCESS;
	return 1;
}

static void handle_post_remove_exit(void)
{
	snapshot_removal_status = HS_READY;
}

static int handle_remove_exit(int status)
{
	int ret;
	struct snapshot *s = snapshot_currently_being_removed;

	assert(s);
	switch (snapshot_removal_status) {
	case HS_PRE_RUNNING:
		handle_pre_remove_exit(status);
		ret = 1;
		break;
	case HS_RUNNING:
		ret = handle_rm_exit(status);
		break;
	case HS_POST_RUNNING:
		handle_post_remove_exit();
		ret = 1;
		break;
	default:
		ret = -E_BUG;
	}
	if (snapshot_removal_status == HS_READY) {
		free(s->name);
		free(s);
		snapshot_currently_being_removed = NULL;
	}
	remove_pid = 0;
	return ret;
}

static int wait_for_remove_process(void)
{
	int status, ret;

	assert(remove_pid);
	assert(
		snapshot_removal_status == HS_PRE_RUNNING ||
		snapshot_removal_status == HS_RUNNING ||
		snapshot_removal_status == HS_POST_RUNNING
	);
	ret = wait_for_process(remove_pid, &status);
	if (ret < 0)
		return ret;
	return handle_remove_exit(status);
}

static int handle_rsync_exit(int status)
{
	int es, ret;

	if (!WIFEXITED(status)) {
		DSS_ERROR_LOG(("rsync process %d died involuntary\n", (int)create_pid));
		ret = -E_INVOLUNTARY_EXIT;
		snapshot_creation_status = HS_READY;
		goto out;
	}
	es = WEXITSTATUS(status);
	/*
	 * Restart rsync on non-fatal errors:
	 * 12: Error in rsync protocol data stream
	 * 13: Errors with program diagnostics
	 */
	if (es == 12 || es == 13) {
		DSS_WARNING_LOG(("rsync process %d returned %d -- restarting\n",
			(int)create_pid, es));
		snapshot_creation_status = HS_NEEDS_RESTART;
		next_snapshot_time = get_current_time() + 60;
		ret = 1;
		goto out;
	}
	if (es != 0 && es != 23 && es != 24) {
		DSS_ERROR_LOG(("rsync process %d returned %d\n", (int)create_pid, es));
		ret = -E_BAD_EXIT_CODE;
		snapshot_creation_status = HS_READY;
		goto out;
	}
	ret = rename_incomplete_snapshot(current_snapshot_creation_time);
	if (ret < 0)
		goto out;
	snapshot_creation_status = HS_SUCCESS;
	free(name_of_reference_snapshot);
	name_of_reference_snapshot = NULL;
out:
	create_process_stopped = 0;
	return ret;
}

static int handle_pre_create_hook_exit(int status)
{
	int es, ret;
	static int warn_count;

	if (!WIFEXITED(status)) {
		snapshot_creation_status = HS_READY;
		ret = -E_INVOLUNTARY_EXIT;
		goto out;
	}
	es = WEXITSTATUS(status);
	if (es) {
		if (!warn_count--) {
			DSS_NOTICE_LOG(("pre_create_hook %s returned %d\n",
				conf.pre_create_hook_arg, es));
			DSS_NOTICE_LOG(("deferring snapshot creation...\n"));
			warn_count = 60; /* warn only once per hour */
		}
		next_snapshot_time = get_current_time() + 60;
		snapshot_creation_status = HS_READY;
		ret = 0;
		goto out;
	}
	warn_count = 0;
	snapshot_creation_status = HS_PRE_SUCCESS;
	ret = 1;
out:
	return ret;
}

static int handle_sigchld(void)
{
	pid_t pid;
	int status, ret = reap_child(&pid, &status);

	if (ret <= 0)
		return ret;

	if (pid == create_pid) {
		switch (snapshot_creation_status) {
		case HS_PRE_RUNNING:
			ret = handle_pre_create_hook_exit(status);
			break;
		case HS_RUNNING:
			ret = handle_rsync_exit(status);
			break;
		case HS_POST_RUNNING:
			snapshot_creation_status = HS_READY;
			ret = 1;
			break;
		default:
			DSS_EMERG_LOG(("BUG: create can't die in status %d\n",
				snapshot_creation_status));
			return -E_BUG;
		}
		create_pid = 0;
		return ret;
	}
	if (pid == remove_pid) {
		ret = handle_remove_exit(status);
		if (ret < 0)
			return ret;
		return ret;
	}
	DSS_EMERG_LOG(("BUG: unknown process %d died\n", (int)pid));
	return -E_BUG;
}

static int check_config(void)
{
	if (conf.unit_interval_arg <= 0) {
		DSS_ERROR_LOG(("bad unit interval: %i\n", conf.unit_interval_arg));
		return -E_INVALID_NUMBER;
	}
	DSS_DEBUG_LOG(("unit interval: %i day(s)\n", conf.unit_interval_arg));
	if (conf.num_intervals_arg <= 0 || conf.num_intervals_arg > 30) {
		DSS_ERROR_LOG(("bad number of intervals: %i\n",
			conf.num_intervals_arg));
		return -E_INVALID_NUMBER;
	}
	DSS_DEBUG_LOG(("number of intervals: %i\n", conf.num_intervals_arg));
	return 1;
}

/*
 * Returns < 0 on errors, 0 if no config file is given and > 0 if the config
 * file was read successfully.
 */
static int parse_config_file(int override)
{
	int ret, config_file_exists;
	char *config_file = get_config_file_name();
	struct stat statbuf;
	char *old_logfile_arg = NULL;
	int old_daemon_given = 0;

	if (override) { /* SIGHUP */
		if (conf.logfile_given)
			old_logfile_arg = dss_strdup(conf.logfile_arg);
		old_daemon_given = conf.daemon_given;
	}

	config_file_exists = !stat(config_file, &statbuf);
	if (!config_file_exists && conf.config_file_given) {
		ret = -ERRNO_TO_DSS_ERROR(errno);
		DSS_ERROR_LOG(("failed to stat config file %s\n", config_file));
		goto out;
	}
	if (config_file_exists) {
		struct cmdline_parser_params params;
		params.override = override;
		params.initialize = 0;
		params.check_required = 1;
		params.check_ambiguity = 0;
		params.print_errors = 1;
		if (override) { /* invalidate all rsync options */
			int i;

			for (i = 0; i < conf.rsync_option_given; i++) {
				free(conf.rsync_option_arg[i]);
				conf.rsync_option_arg[i] = NULL;
			}
			conf.rsync_option_given = 0;
		}
		cmdline_parser_config_file(config_file, &conf, &params);
	}
	ret = check_config();
	if (ret < 0)
		goto out;
	if (override) {
		/* don't change daemon mode on SIGHUP */
		conf.daemon_given = old_daemon_given;
		close_log(logfile);
		logfile = NULL;
		if (conf.logfile_given)
			free(old_logfile_arg);
		else if (conf.daemon_given) { /* re-use old logfile */
			conf.logfile_arg = old_logfile_arg;
			conf.logfile_given = 1;
		}
	}
	if (conf.logfile_given && conf.run_given && conf.daemon_given) {
		logfile = open_log(conf.logfile_arg);
		log_welcome(conf.loglevel_arg);
	}
	DSS_DEBUG_LOG(("loglevel: %d\n", conf.loglevel_arg));
	ret = config_file_exists;
out:
	free(config_file);
	if (ret < 0)
		DSS_EMERG_LOG(("%s\n", dss_strerror(-ret)));
	return ret;
}

static int change_to_dest_dir(void)
{
	DSS_INFO_LOG(("changing cwd to %s\n", conf.dest_dir_arg));
	return dss_chdir(conf.dest_dir_arg);
}

static int handle_sighup(void)
{
	int ret;

	DSS_NOTICE_LOG(("SIGHUP, re-reading config\n"));
	dump_dss_config("old");
	ret = parse_config_file(1);
	if (ret < 0)
		return ret;
	dump_dss_config("reloaded");
	invalidate_next_snapshot_time();
	return change_to_dest_dir();
}

static int handle_signal(void)
{
	int sig, ret = next_signal();

	if (ret <= 0)
		goto out;
	sig = ret;
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		restart_create_process();
		dss_kill(create_pid, SIGTERM, NULL);
		dss_kill(remove_pid, SIGTERM, NULL);
		ret = -E_SIGNAL;
		break;
	case SIGHUP:
		ret = handle_sighup();
		break;
	case SIGCHLD:
		ret = handle_sigchld();
		break;
	}
out:
	if (ret < 0)
		DSS_ERROR_LOG(("%s\n", dss_strerror(-ret)));
	return ret;
}

/*
 * We can not use rsync locally if the local user is different from the remote
 * user or if the src dir is not on the local host (or both).
 */
static int use_rsync_locally(char *logname)
{
	char *h = conf.remote_host_arg;

	if (strcmp(h, "localhost") && strcmp(h, "127.0.0.1"))
		return 0;
	if (conf.remote_user_given && strcmp(conf.remote_user_arg, logname))
		return 0;
	return 1;
}

static int rename_resume_snap(int64_t creation_time)
{
	struct snapshot_list sl;
	struct snapshot *s = NULL;
	char *new_name = incomplete_name(creation_time);
	int ret;
	const char *why;

	sl.num_snapshots = 0;

	ret = 0;
	if (conf.no_resume_given)
		goto out;
	dss_get_snapshot_list(&sl);
	/*
	 * Snapshot recycling: We first look at the newest snapshot. If this
	 * snapshot happens to be incomplete, the last rsync process was
	 * aborted and we reuse this one. Otherwise we look at snapshots which
	 * could be removed (outdated and redundant snapshots) as candidates
	 * for recycling. If no outdated/redundant snapshot exists, we check if
	 * there is an orphaned snapshot, which likely is useless anyway.
	 *
	 * Only if no existing snapshot is suitable for recycling, we bite the
	 * bullet and create a new one.
	 */
	s = get_newest_snapshot(&sl);
	if (!s) /* no snapshots at all */
		goto out;
	/* re-use last snapshot if it is incomplete */
	why = "aborted";
	if ((s->flags & SS_COMPLETE) == 0)
		goto out;
	why = "outdated";
	s = find_outdated_snapshot(&sl);
	if (s)
		goto out;
	why = "redundant";
	s = find_redundant_snapshot(&sl);
	if (s)
		goto out;
	why = "orphaned";
	s = find_orphaned_snapshot(&sl);
out:
	if (s) {
		DSS_INFO_LOG(("reusing %s snapshot %s\n", why, s->name));
		ret = dss_rename(s->name, new_name);
	}
	if (ret >= 0)
		DSS_NOTICE_LOG(("creating new snapshot %s\n", new_name));
	free(new_name);
	free_snapshot_list(&sl);
	return ret;
}

static void create_rsync_argv(char ***argv, int64_t *num)
{
	char *logname;
	int i = 0, j;
	struct snapshot_list sl;

	dss_get_snapshot_list(&sl);
	assert(!name_of_reference_snapshot);
	name_of_reference_snapshot = name_of_newest_complete_snapshot(&sl);
	free_snapshot_list(&sl);

	*argv = dss_malloc((15 + conf.rsync_option_given) * sizeof(char *));
	(*argv)[i++] = dss_strdup("rsync");
	(*argv)[i++] = dss_strdup("-aq");
	(*argv)[i++] = dss_strdup("--delete");
	for (j = 0; j < conf.rsync_option_given; j++)
		(*argv)[i++] = dss_strdup(conf.rsync_option_arg[j]);
	if (name_of_reference_snapshot) {
		DSS_INFO_LOG(("using %s as reference\n", name_of_reference_snapshot));
		(*argv)[i++] = make_message("--link-dest=../%s",
			name_of_reference_snapshot);
	} else
		DSS_INFO_LOG(("no suitable reference snapshot found\n"));
	logname = dss_logname();
	if (use_rsync_locally(logname))
		(*argv)[i++] = dss_strdup(conf.source_dir_arg);
	else
		(*argv)[i++] = make_message("%s@%s:%s/", conf.remote_user_given?
			conf.remote_user_arg : logname,
			conf.remote_host_arg, conf.source_dir_arg);
	free(logname);
	*num = get_current_time();
	(*argv)[i++] = incomplete_name(*num);
	(*argv)[i++] = NULL;
	for (j = 0; j < i; j++)
		DSS_DEBUG_LOG(("argv[%d] = %s\n", j, (*argv)[j]));
}

static void free_rsync_argv(char **argv)
{
	int i;

	if (!argv)
		return;
	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

static int create_snapshot(char **argv)
{
	int ret;

	ret = rename_resume_snap(current_snapshot_creation_time);
	if (ret < 0)
		return ret;
	dss_exec(&create_pid, argv[0], argv);
	snapshot_creation_status = HS_RUNNING;
	return ret;
}

static int select_loop(void)
{
	int ret;
	/* check every 60 seconds for free disk space */
	struct timeval tv;
	char **rsync_argv = NULL;

	for (;;) {
		fd_set rfds;
		struct timeval *tvp;

		if (remove_pid)
			tvp = NULL; /* sleep until rm hook/process dies */
		else { /* sleep one minute */
			tv.tv_sec = 60;
			tv.tv_usec = 0;
			tvp = &tv;
		}
		FD_ZERO(&rfds);
		FD_SET(signal_pipe, &rfds);
		ret = dss_select(signal_pipe + 1, &rfds, NULL, tvp);
		if (ret < 0)
			goto out;
		if (FD_ISSET(signal_pipe, &rfds)) {
			ret = handle_signal();
			if (ret < 0)
				goto out;
		}
		if (remove_pid)
			continue;
		if (snapshot_removal_status == HS_PRE_SUCCESS) {
			ret = exec_rm();
			if (ret < 0)
				goto out;
			continue;
		}
		if (snapshot_removal_status == HS_SUCCESS) {
			post_remove_hook();
			continue;
		}
		ret = try_to_free_disk_space();
		if (ret < 0)
			goto out;
		if (snapshot_removal_status != HS_READY) {
			stop_create_process();
			continue;
		}
		restart_create_process();
		switch (snapshot_creation_status) {
		case HS_READY:
			if (!next_snapshot_is_due())
				continue;
			pre_create_hook();
			continue;
		case HS_PRE_RUNNING:
		case HS_RUNNING:
		case HS_POST_RUNNING:
			continue;
		case HS_PRE_SUCCESS:
			if (!name_of_reference_snapshot) {
				free_rsync_argv(rsync_argv);
				create_rsync_argv(&rsync_argv, &current_snapshot_creation_time);
			}
			ret = create_snapshot(rsync_argv);
			if (ret < 0)
				goto out;
			continue;
		case HS_NEEDS_RESTART:
			if (!next_snapshot_is_due())
				continue;
			ret = create_snapshot(rsync_argv);
			if (ret < 0)
				goto out;
			continue;
		case HS_SUCCESS:
			post_create_hook();
			continue;
		}
	}
out:
	return ret;
}

static void exit_hook(int exit_code)
{
	char *argv[3];
	pid_t pid;

	argv[0] = conf.exit_hook_arg;
	argv[1] = dss_strerror(-exit_code);
	argv[2] = NULL;

	DSS_NOTICE_LOG(("executing %s %s\n", argv[0], argv[1]));
	dss_exec(&pid, conf.exit_hook_arg, argv);
}

static void lock_dss_or_die(void)
{
	char *config_file = get_config_file_name();
	int ret = lock_dss(config_file);

	free(config_file);
	if (ret < 0) {
		DSS_EMERG_LOG(("failed to lock: %s\n", dss_strerror(-ret)));
		exit(EXIT_FAILURE);
	}
}

static int com_run(void)
{
	int ret;

	lock_dss_or_die();
	if (conf.dry_run_given) {
		DSS_ERROR_LOG(("dry_run not supported by this command\n"));
		return -E_SYNTAX;
	}
	ret = install_sighandler(SIGHUP);
	if (ret < 0)
		return ret;
	ret = select_loop();
	if (ret >= 0) /* impossible */
		ret = -E_BUG;
	exit_hook(ret);
	return ret;
}

static int com_prune(void)
{
	int ret;
	struct snapshot_list sl;
	struct snapshot *victim;
	struct disk_space ds;
	const char *why;

	lock_dss_or_die();
	ret = get_disk_space(".", &ds);
	if (ret < 0)
		return ret;
	log_disk_space(&ds);
	dss_get_snapshot_list(&sl);
	why = "outdated";
	victim = find_outdated_snapshot(&sl);
	if (victim)
		goto rm;
	why = "redundant";
	victim = find_redundant_snapshot(&sl);
	if (victim)
		goto rm;
	ret = 0;
	goto out;
rm:
	if (conf.dry_run_given) {
		dss_msg("%s snapshot %s (interval = %i)\n",
			why, victim->name, victim->interval);
		ret = 0;
		goto out;
	}
	pre_remove_hook(victim, why);
	if (snapshot_removal_status == HS_PRE_RUNNING) {
		ret = wait_for_remove_process();
		if (ret < 0)
			goto out;
		if (snapshot_removal_status != HS_PRE_SUCCESS)
			goto out;
	}
	ret = exec_rm();
	if (ret < 0)
		goto out;
	ret = wait_for_remove_process();
	if (ret < 0)
		goto out;
	if (snapshot_removal_status != HS_SUCCESS)
		goto out;
	post_remove_hook();
	if (snapshot_removal_status != HS_POST_RUNNING)
		goto out;
	ret = wait_for_remove_process();
	if (ret < 0)
		goto out;
	ret = 1;
out:
	free_snapshot_list(&sl);
	return ret;
}

static int com_create(void)
{
	int ret, status;
	char **rsync_argv;

	lock_dss_or_die();
	if (conf.dry_run_given) {
		int i;
		char *msg = NULL;
		create_rsync_argv(&rsync_argv, &current_snapshot_creation_time);
		for (i = 0; rsync_argv[i]; i++) {
			char *tmp = msg;
			msg = make_message("%s%s%s", tmp? tmp : "",
				tmp? " " : "", rsync_argv[i]);
			free(tmp);
		}
		free_rsync_argv(rsync_argv);
		dss_msg("%s\n", msg);
		free(msg);
		return 1;
	}
	pre_create_hook();
	if (create_pid) {
		ret = wait_for_process(create_pid, &status);
		if (ret < 0)
			return ret;
		ret = handle_pre_create_hook_exit(status);
		if (ret <= 0) /* error, or pre-create failed */
			return ret;
	}
	create_rsync_argv(&rsync_argv, &current_snapshot_creation_time);
	ret = create_snapshot(rsync_argv);
	if (ret < 0)
		goto out;
	ret = wait_for_process(create_pid, &status);
	if (ret < 0)
		goto out;
	ret = handle_rsync_exit(status);
	if (ret < 0)
		goto out;
	post_create_hook();
	if (create_pid)
		ret = wait_for_process(create_pid, &status);
out:
	free_rsync_argv(rsync_argv);
	return ret;
}

static int com_ls(void)
{
	int i;
	struct snapshot_list sl;
	struct snapshot *s;

	dss_get_snapshot_list(&sl);
	FOR_EACH_SNAPSHOT(s, i, &sl) {
		int64_t d = 0;
		if (s->flags & SS_COMPLETE)
			d = (s->completion_time - s->creation_time) / 60;
		dss_msg("%u\t%s\t%3" PRId64 ":%02" PRId64 "\n", s->interval, s->name, d/60, d%60);
	};
	free_snapshot_list(&sl);
	return 1;
}

static int setup_signal_handling(void)
{
	int ret;

	DSS_INFO_LOG(("setting up signal handlers\n"));
	signal_pipe = signal_init(); /* always successful */
	ret = install_sighandler(SIGINT);
	if (ret < 0)
		return ret;
	ret = install_sighandler(SIGTERM);
	if (ret < 0)
		return ret;
	return install_sighandler(SIGCHLD);
}

/**
 * The main function of dss.
 *
 * \param argc Usual argument count.
 * \param argv Usual argument vector.
 */
int main(int argc, char **argv)
{
	int ret;
	struct cmdline_parser_params params;

	params.override = 0;
	params.initialize = 1;
	params.check_required = 0;
	params.check_ambiguity = 0;
	params.print_errors = 1;

	cmdline_parser_ext(argc, argv, &conf, &params); /* aborts on errors */
	ret = parse_config_file(0);
	if (ret < 0)
		goto out;
	if (ret == 0) { /* no config file given */
		/*
		 * Parse the command line options again, but this time check
		 * that all required options are given.
		 */
		struct cmdline_parser_params params;
		params.override = 1;
		params.initialize = 1;
		params.check_required = 1;
		params.check_ambiguity = 1;
		params.print_errors = 1;
		cmdline_parser_ext(argc, argv, &conf, &params); /* aborts on errors */
	}
	if (conf.daemon_given)
		daemon_init();
	ret = change_to_dest_dir();
	if (ret < 0)
		goto out;
	dump_dss_config("startup");
	ret = setup_signal_handling();
	if (ret < 0)
		goto out;
	ret = call_command_handler();
out:
	if (ret < 0)
		DSS_EMERG_LOG(("%s\n", dss_strerror(-ret)));
	exit(ret >= 0? EXIT_SUCCESS : EXIT_FAILURE);
}
