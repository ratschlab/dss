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
#include "string.h"
#include "error.h"
#include "fd.h"
#include "exec.h"
#include "daemon.h"
#include "signal.h"
#include "df.h"
#include "time.h"


struct gengetopt_args_info conf;
char *dss_error_txt = NULL;
static FILE *logfile;
static int signal_pipe;

/** Process id of current rsync process. */
static pid_t rsync_pid;
/** Whether the rsync process is currently stopped */
static int rsync_stopped;
/** Process id of current rm process. */
static pid_t rm_pid;
/** When the next snapshot is due. */
struct timeval next_snapshot_time;
/** The pid of the pre-create hook. */
pid_t pre_create_hook_pid;
/** The pid of the post-create hook. */
pid_t post_create_hook_pid;

/* Creation time of the snapshot currently being created. */
int64_t current_snapshot_creation_time;

static char *path_to_last_complete_snapshot;

enum {
	/** We are ready to take the next snapshot. */
	SCS_READY,
	/** The pre-creation hook has been started. */
	SCS_PRE_HOOK_RUNNING,
	/** The pre-creation hook exited successfully. */
	SCS_PRE_HOOK_SUCCESS,
	/** The rsync process is running. */
	SCS_RSYNC_RUNNING,
	/** The rsync process exited successfully. */
	SCS_RSYNC_SUCCESS,
	/** The post-create hook has been started- */
	SCS_POST_HOOK_RUNNING,
};

static unsigned snapshot_creation_status;


DEFINE_DSS_ERRLIST;


/* a litte cpp magic helps to DRY */
#define COMMANDS \
	COMMAND(ls) \
	COMMAND(create) \
	COMMAND(prune) \
	COMMAND(run)
#define COMMAND(x) int com_ ##x(void);
COMMANDS
#undef COMMAND
#define COMMAND(x) if (conf.x ##_given) return com_ ##x();
int call_command_handler(void)
{
	COMMANDS
	DSS_EMERG_LOG("BUG: did not find command handler\n");
	exit(EXIT_FAILURE);
}
#undef COMMAND
#undef COMMANDS

/*
 * complete, not being deleted: 1204565370-1204565371.Sun_Mar_02_2008_14_33-Sun_Mar_02_2008_14_43
 * complete, being deleted: 1204565370-1204565371.being_deleted
 * incomplete, not being deleted: 1204565370-incomplete
 * incomplete, being deleted: 1204565370-incomplete.being_deleted
 */
enum snapshot_status_flags {
	/** The rsync process terminated successfully. */
	SS_COMPLETE = 1,
	/** The rm process is running to remove this snapshot. */
	SS_BEING_DELETED = 2,
};

struct snapshot {
	char *name;
	int64_t creation_time;
	int64_t completion_time;
	enum snapshot_status_flags flags;
	unsigned interval;
};

/*
 * An edge snapshot is either the oldest one or the newest one.
 *
 * We need to find either of them occasionally: The create code
 * needs to know the newest snapshot because that is the one
 * used as the link destination dir. The pruning code needs to
 * find the oldest one in case disk space becomes low.
 */
struct edge_snapshot_data {
	int64_t now;
	struct snapshot snap;
};

__printf_2_3 void dss_log(int ll, const char* fmt,...)
{
	va_list argp;
	FILE *outfd;
	struct tm *tm;
	time_t t1;
	char str[255] = "";

	if (ll < conf.loglevel_arg)
		return;
	outfd = logfile? logfile : stderr;
	time(&t1);
	tm = localtime(&t1);
	strftime(str, sizeof(str), "%b %d %H:%M:%S", tm);
	fprintf(outfd, "%s ", str);
	if (conf.loglevel_arg <= INFO)
		fprintf(outfd, "%i: ", ll);
	va_start(argp, fmt);
	vfprintf(outfd, fmt, argp);
	va_end(argp);
}

/**
 * Print a message either to stdout or to the log file.
 */
__printf_1_2 void dss_msg(const char* fmt,...)
{
	FILE *outfd = conf.daemon_given? logfile : stdout;
	va_list argp;
	va_start(argp, fmt);
	vfprintf(outfd, fmt, argp);
	va_end(argp);
}

/**
 * Return the desired number of snapshots of an interval.
 */
unsigned num_snapshots(int interval)
{
	unsigned n;

	assert(interval >= 0);

	if (interval >= conf.num_intervals_arg)
		return 0;
	n = conf.num_intervals_arg - interval - 1;
	return 1 << n;
}

/* return: Whether dirname is a snapshot directory (0: no, 1: yes) */
int is_snapshot(const char *dirname, int64_t now, struct snapshot *s)
{
	int i, ret;
	char *dash, *dot, *tmp;
	int64_t num;

	assert(dirname);
	dash = strchr(dirname, '-');
	if (!dash || !dash[1] || dash == dirname)
		return 0;
	for (i = 0; dirname[i] != '-'; i++)
		if (!isdigit(dirname[i]))
			return 0;
	tmp = dss_strdup(dirname);
	tmp[i] = '\0';
	ret = dss_atoi64(tmp, &num);
	free(tmp);
	if (ret < 0) {
		free(dss_error_txt);
		return 0;
	}
	assert(num >= 0);
	if (num > now)
		return 0;
	s->creation_time = num;
	//DSS_DEBUG_LOG("%s start time: %lli\n", dirname, (long long)s->creation_time);
	s->interval = (long long) ((now - s->creation_time)
		/ conf.unit_interval_arg / 24 / 3600);
	if (!strcmp(dash + 1, "incomplete")) {
		s->completion_time = -1;
		s->flags = 0; /* neither complete, nor being deleted */
		goto success;
	}
	if (!strcmp(dash + 1, "incomplete.being_deleted")) {
		s->completion_time = -1;
		s->flags = SS_BEING_DELETED; /* mot cpmplete, being deleted */
		goto success;
	}
	tmp = dash + 1;
	dot = strchr(tmp, '.');
	if (!dot || !dot[1] || dot == tmp)
		return 0;
	for (i = 0; tmp[i] != '.'; i++)
		if (!isdigit(tmp[i]))
			return 0;
	tmp = dss_strdup(dash + 1);
	tmp[i] = '\0';
	ret = dss_atoi64(tmp, &num);
	free(tmp);
	if (ret < 0) {
		free(dss_error_txt);
		return 0;
	}
	if (num > now)
		return 0;
	s->completion_time = num;
	s->flags = SS_COMPLETE;
	if (!strcmp(dot + 1, "being_deleted"))
		s->flags |= SS_BEING_DELETED;
success:
	s->name = dss_strdup(dirname);
	return 1;
}

int64_t get_current_time(void)
{
	time_t now;
	time(&now);
	DSS_DEBUG_LOG("now: %lli\n", (long long) now);
	return (int64_t)now;
}

char *incomplete_name(int64_t start)
{
	return make_message("%lli-incomplete", (long long)start);
}

char *being_deleted_name(struct snapshot *s)
{
	if (s->flags & SS_COMPLETE)
		return make_message("%lli-%lli.being_deleted",
			(long long)s->creation_time,
			(long long)s->completion_time);
	return make_message("%lli-incomplete.being_deleted",
		(long long)s->creation_time);
}

int complete_name(int64_t start, int64_t end, char **result)
{
	struct tm start_tm, end_tm;
	time_t *start_seconds = (time_t *) (uint64_t *)&start; /* STFU, gcc */
	time_t *end_seconds = (time_t *) (uint64_t *)&end; /* STFU, gcc */
	char start_str[200], end_str[200];

	if (!localtime_r(start_seconds, &start_tm)) {
		make_err_msg("%lli", (long long)start);
		return -E_LOCALTIME;
	}
	if (!localtime_r(end_seconds, &end_tm)) {
		make_err_msg("%lli", (long long)end);
		return -E_LOCALTIME;
	}
	if (!strftime(start_str, sizeof(start_str), "%a_%b_%d_%Y_%H_%M_%S", &start_tm)) {
		make_err_msg("%lli", (long long)start);
		return -E_STRFTIME;
	}
	if (!strftime(end_str, sizeof(end_str), "%a_%b_%d_%Y_%H_%M_%S", &end_tm)) {
		make_err_msg("%lli", (long long)end);
		return -E_STRFTIME;
	}
	*result = make_message("%lli-%lli.%s-%s", (long long) start, (long long) end,
		start_str, end_str);
	return 1;
}

struct snapshot_list {
	int64_t now;
	unsigned num_snapshots;
	unsigned array_size;
	struct snapshot **snapshots;
	/**
	 * Array of size num_intervals + 1
	 *
	 * It contains the number of snapshots in each interval. interval_count[num_intervals]
	 * is the number of snapshots which belong to any interval greater than num_intervals.
	 */
	unsigned *interval_count;
};

#define FOR_EACH_SNAPSHOT(s, i, sl) \
	for ((i) = 0; (i) < (sl)->num_snapshots && ((s) = (sl)->snapshots[(i)]); (i)++)

#define NUM_COMPARE(x, y) ((int)((x) < (y)) - (int)((x) > (y)))

static int compare_snapshots(const void *a, const void *b)
{
	struct snapshot *s1 = *(struct snapshot **)a;
	struct snapshot *s2 = *(struct snapshot **)b;
	return NUM_COMPARE(s2->creation_time, s1->creation_time);
}

/** Compute the minimum of \a a and \a b. */
#define DSS_MIN(a,b) ((a) < (b) ? (a) : (b))

int add_snapshot(const char *dirname, void *private)
{
	struct snapshot_list *sl = private;
	struct snapshot s;
	int ret = is_snapshot(dirname, sl->now, &s);

	if (!ret)
		return 1;
	if (sl->num_snapshots >= sl->array_size) {
		sl->array_size = 2 * sl->array_size + 1;
		sl->snapshots = dss_realloc(sl->snapshots,
			sl->array_size * sizeof(struct snapshot *));
	}
	sl->snapshots[sl->num_snapshots] = dss_malloc(sizeof(struct snapshot));
	*(sl->snapshots[sl->num_snapshots]) = s;
	sl->interval_count[DSS_MIN(s.interval, conf.num_intervals_arg)]++;
	sl->num_snapshots++;
	return 1;
}

void get_snapshot_list(struct snapshot_list *sl)
{
	sl->now = get_current_time();
	sl->num_snapshots = 0;
	sl->array_size = 0;
	sl->snapshots = NULL;
	sl->interval_count = dss_calloc((conf.num_intervals_arg + 1) * sizeof(unsigned));
	for_each_subdir(add_snapshot, sl);
	qsort(sl->snapshots, sl->num_snapshots, sizeof(struct snapshot *),
		compare_snapshots);
}

void free_snapshot_list(struct snapshot_list *sl)
{
	int i;
	struct snapshot *s;

	FOR_EACH_SNAPSHOT(s, i, sl) {
		free(s->name);
		free(s);
	}
	free(sl->interval_count);
	sl->interval_count = NULL;
	free(sl->snapshots);
	sl->snapshots = NULL;
	sl->num_snapshots = 0;
}

void stop_rsync_process(void)
{
	if (!rsync_pid || rsync_stopped)
		return;
	kill(SIGSTOP, rsync_pid);
	rsync_stopped = 1;
}

void restart_rsync_process(void)
{
	if (!rsync_pid || !rsync_stopped)
		return;
	kill (SIGCONT, rsync_pid);
	rsync_stopped = 0;
}

/**
 * Print a log message about the exit status of a child.
 */
void log_termination_msg(pid_t pid, int status)
{
	if (WIFEXITED(status))
		DSS_INFO_LOG("child %i exited. Exit status: %i\n", (int)pid,
			WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		DSS_NOTICE_LOG("child %i was killed by signal %i\n", (int)pid,
			WTERMSIG(status));
	else
		DSS_WARNING_LOG("child %i terminated abormally\n", (int)pid);
}

int wait_for_process(pid_t pid, int *status)
{
	int ret;

	DSS_DEBUG_LOG("Waiting for process %d to terminate\n", (int)pid);
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
		DSS_WARNING_LOG("sending SIGTERM to pid %d\n", (int)pid);
		kill(pid, SIGTERM);
	}
	if (ret < 0)
		make_err_msg("failed to wait for process %d", (int)pid);
	else
		log_termination_msg(pid, *status);
	return ret;
}

int remove_snapshot(struct snapshot *s)
{
	int fds[3] = {0, 0, 0};
	assert(!rm_pid);
	char *new_name = being_deleted_name(s);
	int ret = dss_rename(s->name, new_name);
	char *argv[] = {"rm", "-rf", new_name, NULL};

	if (ret < 0)
		goto out;
	DSS_NOTICE_LOG("removing %s (interval = %i)\n", s->name, s->interval);
	stop_rsync_process();
	ret = dss_exec(&rm_pid, argv[0], argv, fds);
out:
	free(new_name);
	return ret;
}

/*
 * return: 0: no redundant snapshots, 1: rm process started, negative: error
 */
int remove_redundant_snapshot(struct snapshot_list *sl)
{
	int ret, i, interval;
	struct snapshot *s;
	unsigned missing = 0;

	DSS_INFO_LOG("looking for intervals containing too many snapshots\n");
	for (interval = conf.num_intervals_arg - 1; interval >= 0; interval--) {
		unsigned keep = num_snapshots(interval);
		unsigned num = sl->interval_count[interval];
		struct snapshot *victim = NULL, *prev = NULL;
		int64_t score = LONG_MAX;

		if (keep >= num)
			missing += keep - num;
//		DSS_DEBUG_LOG("interval %i: keep: %u, have: %u, missing: %u\n",
//			interval, keep, num, missing);
		if (keep + missing >= num)
			continue;
		/* redundant snapshot in this interval, pick snapshot with lowest score */
		FOR_EACH_SNAPSHOT(s, i, sl) {
			int64_t this_score;

			//DSS_DEBUG_LOG("checking %s\n", s->name);
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
			//DSS_DEBUG_LOG("%s: score %lli\n", s->name, (long long)score);
			if (this_score < score) {
				score = this_score;
				victim = s;
			}
			prev = s;
		}
		assert(victim);
		if (conf.dry_run_given) {
			dss_msg("%s would be removed (interval = %i)\n",
				victim->name, victim->interval);
			continue;
		}
		ret = remove_snapshot(victim);
		return ret < 0? ret : 1;
	}
	return 0;
}

int remove_outdated_snapshot(struct snapshot_list *sl)
{
	int i, ret;
	struct snapshot *s;

	DSS_INFO_LOG("looking for snapshots belonging to intervals greater than %d\n",
		conf.num_intervals_arg);
	FOR_EACH_SNAPSHOT(s, i, sl) {
		if (s->interval <= conf.num_intervals_arg)
			continue;
		if (conf.dry_run_given) {
			dss_msg("%s would be removed (interval = %i)\n",
				s->name, s->interval);
			continue;
		}
		ret = remove_snapshot(s);
		if (ret < 0)
			return ret;
		return 1;
	}
	return 0;
}

int handle_rm_exit(int status)
{
	int es, ret;

	if (!WIFEXITED(status)) {
		make_err_msg("rm process %d died involuntary", (int)rm_pid);
		ret = -E_INVOLUNTARY_EXIT;
		goto out;
	}
	es = WEXITSTATUS(status);
	if (es) {
		make_err_msg("rm process %d returned %d", (int)rm_pid, es);
		ret = -E_BAD_EXIT_CODE;
		goto out;
	}
	ret = 1;
	rm_pid = 0;
out:
	return ret;
}

int wait_for_rm_process(void)
{
	int status, ret = wait_for_process(rm_pid, &status);

	if (ret < 0)
		return ret;
	return handle_rm_exit(status);
}

void kill_process(pid_t pid)
{
	if (!pid)
		return;
	DSS_WARNING_LOG("sending SIGTERM to pid %d\n", (int)pid);
	kill(pid, SIGTERM);
}

int check_config(void)
{
	if (conf.unit_interval_arg <= 0) {
		make_err_msg("bad unit interval: %i", conf.unit_interval_arg);
		return -E_INVALID_NUMBER;
	}
	DSS_DEBUG_LOG("unit interval: %i day(s)\n", conf.unit_interval_arg);
	if (conf.num_intervals_arg <= 0) {
		make_err_msg("bad number of intervals  %i", conf.num_intervals_arg);
		return -E_INVALID_NUMBER;
	}
	DSS_DEBUG_LOG("number of intervals: %i\n", conf.num_intervals_arg);
	return 1;
}

/* exits on errors */
void parse_config_file(int override)
{
	int ret;
	char *config_file;
	struct stat statbuf;
	char *old_logfile_arg = NULL;
	int old_daemon_given = 0;

	if (conf.config_file_given)
		config_file = dss_strdup(conf.config_file_arg);
	else {
		char *home = get_homedir();
		config_file = make_message("%s/.dssrc", home);
		free(home);
	}
	if (override) { /* SIGHUP */
		if (conf.logfile_given)
			old_logfile_arg = dss_strdup(conf.logfile_arg);
		old_daemon_given = conf.daemon_given;
	}

	ret = stat(config_file, &statbuf);
	if (ret && conf.config_file_given) {
		ret = -ERRNO_TO_DSS_ERROR(errno);
		make_err_msg("failed to stat config file %s", config_file);
		goto out;
	}
	if (!ret) {
		struct cmdline_parser_params params = {
			.override = override,
			.initialize = 0,
			.check_required = 1,
			.check_ambiguity = 0
		};
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
	if (conf.logfile_given) {
		logfile = open_log(conf.logfile_arg);
		log_welcome(conf.loglevel_arg);
	}
	DSS_EMERG_LOG("loglevel: %d\n", conf.loglevel_arg);
//	cmdline_parser_dump(logfile? logfile : stdout, &conf);
	ret = dss_chdir(conf.dest_dir_arg);
out:
	free(config_file);
	if (ret >= 0)
		return;
	log_err_msg(EMERG, -ret);
	exit(EXIT_FAILURE);
}

void handle_sighup(void)
{
	DSS_NOTICE_LOG("SIGHUP\n");
	parse_config_file(1);
}

int rename_incomplete_snapshot(int64_t start)
{
	char *old_name;
	int ret;

	free(path_to_last_complete_snapshot);
	ret = complete_name(start, get_current_time(),
		&path_to_last_complete_snapshot);
	if (ret < 0)
		return ret;
	old_name = incomplete_name(start);
	ret = dss_rename(old_name, path_to_last_complete_snapshot);
	if (ret >= 0)
		DSS_NOTICE_LOG("%s -> %s\n", old_name,
			path_to_last_complete_snapshot);
	free(old_name);
	return ret;
}

void compute_next_snapshot_time(void)
{
	struct timeval now, unit_interval = {.tv_sec = 24 * 3600 * conf.unit_interval_arg},
		tmp, diff;
	int64_t x = 0;
	unsigned wanted = num_snapshots(0), num_complete_snapshots = 0;
	int i, ret;
	struct snapshot *s = NULL;
	struct snapshot_list sl;

	assert(snapshot_creation_status == SCS_READY);
	current_snapshot_creation_time = 0;
	get_snapshot_list(&sl);
	FOR_EACH_SNAPSHOT(s, i, &sl) {
		if (!(s->flags & SS_COMPLETE))
			continue;
		num_complete_snapshots++;
		x += s->completion_time - s->creation_time;
	}
	assert(x >= 0);
	if (num_complete_snapshots)
		x /= num_complete_snapshots; /* avg time to create one snapshot */
	x *= wanted; /* time to create all snapshots in interval 0 */
	tmp.tv_sec = x;
	tmp.tv_usec = 0;
	ret = tv_diff(&unit_interval, &tmp, &diff); /* total sleep time per unit interval */
	gettimeofday(&now, NULL);
	if (ret < 0 || !s)
		goto min_sleep;
	tv_divide(wanted, &diff, &tmp); /* sleep time betweeen two snapshots */
	diff.tv_sec = s->completion_time;
	diff.tv_usec = 0;
	tv_add(&diff, &tmp, &next_snapshot_time);
	if (tv_diff(&now, &next_snapshot_time, NULL) < 0)
		goto out;
min_sleep:
	next_snapshot_time = now;
	next_snapshot_time.tv_sec += 60;
out:
	free_snapshot_list(&sl);
}

int handle_rsync_exit(int status)
{
	int es, ret;

	if (!WIFEXITED(status)) {
		make_err_msg("rsync process %d died involuntary", (int)rsync_pid);
		ret = -E_INVOLUNTARY_EXIT;
		snapshot_creation_status = SCS_READY;
		compute_next_snapshot_time();
		goto out;
	}
	es = WEXITSTATUS(status);
	if (es != 0 && es != 23 && es != 24) {
		make_err_msg("rsync process %d returned %d", (int)rsync_pid, es);
		ret = -E_BAD_EXIT_CODE;
		snapshot_creation_status = SCS_READY;
		compute_next_snapshot_time();
		goto out;
	}
	ret = rename_incomplete_snapshot(current_snapshot_creation_time);
	if (ret < 0)
		goto out;
	snapshot_creation_status = SCS_RSYNC_SUCCESS;
out:
	rsync_pid = 0;
	rsync_stopped = 0;
	return ret;
}

int get_newest_complete(const char *dirname, void *private)
{
	struct edge_snapshot_data *esd = private;
	struct snapshot s;
	int ret = is_snapshot(dirname, esd->now, &s);

	if (ret <= 0)
		return 1;
	if (s.flags != SS_COMPLETE) /* incomplete or being deleted */
		return 1;
	if (s.creation_time < esd->snap.creation_time)
		return 1;
	free(esd->snap.name);
	esd->snap = s;
	return 1;
}

__malloc char *name_of_newest_complete_snapshot(void)
{
	struct edge_snapshot_data esd = {
		.now = get_current_time(),
		.snap = {.creation_time = -1}
	};
	for_each_subdir(get_newest_complete, &esd);
	return esd.snap.name;
}

void create_rsync_argv(char ***argv, int64_t *num)
{
	char *logname, *newest = name_of_newest_complete_snapshot();
	int i = 0, j;

	*argv = dss_malloc((15 + conf.rsync_option_given) * sizeof(char *));
	(*argv)[i++] = dss_strdup("rsync");
	(*argv)[i++] = dss_strdup("-aq");
	(*argv)[i++] = dss_strdup("--delete");
	for (j = 0; j < conf.rsync_option_given; j++)
		(*argv)[i++] = dss_strdup(conf.rsync_option_arg[j]);
	if (newest) {
		DSS_INFO_LOG("using %s as reference snapshot\n", newest);
		(*argv)[i++] = make_message("--link-dest=../%s", newest);
		free(newest);
	} else
		DSS_INFO_LOG("no previous snapshot found\n");
	if (conf.exclude_patterns_given) {
		(*argv)[i++] = dss_strdup("--exclude-from");
		(*argv)[i++] = dss_strdup(conf.exclude_patterns_arg);

	}
	logname = dss_logname();
	if (conf.remote_user_given && !strcmp(conf.remote_user_arg, logname))
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
		DSS_DEBUG_LOG("argv[%d] = %s\n", j, (*argv)[j]);
}

void free_rsync_argv(char **argv)
{
	int i;
	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

int pre_create_hook(void)
{
	int ret, fds[3] = {0, 0, 0};

	if (!conf.pre_create_hook_given) {
		snapshot_creation_status = SCS_PRE_HOOK_SUCCESS;
		return 0;
	}
	DSS_NOTICE_LOG("executing %s\n", conf.pre_create_hook_arg);
	ret = dss_exec_cmdline_pid(&pre_create_hook_pid,
		conf.pre_create_hook_arg, fds);
	if (ret < 0)
		return ret;
	snapshot_creation_status = SCS_PRE_HOOK_RUNNING;
	return ret;
}

int post_create_hook(void)
{
	int ret, fds[3] = {0, 0, 0};
	char *cmd;

	if (!conf.post_create_hook_given) {
		snapshot_creation_status = SCS_READY;
		compute_next_snapshot_time();
		return 0;
	}
	cmd = make_message("%s %s", conf.post_create_hook_arg,
		path_to_last_complete_snapshot);
	DSS_NOTICE_LOG("executing %s\n", cmd);
	ret = dss_exec_cmdline_pid(&post_create_hook_pid, cmd, fds);
	free(cmd);
	if (ret < 0)
		return ret;
	snapshot_creation_status = SCS_POST_HOOK_RUNNING;
	return ret;
}

int create_snapshot(char **argv)
{
	int ret, fds[3] = {0, 0, 0};
	char *name;

	name = incomplete_name(current_snapshot_creation_time);
	DSS_NOTICE_LOG("creating new snapshot %s\n", name);
	free(name);
	ret = dss_exec(&rsync_pid, argv[0], argv, fds);
	if (ret < 0)
		return ret;
	snapshot_creation_status = SCS_RSYNC_RUNNING;
	return ret;
}

int handle_pre_create_hook_exit(int status)
{
	int es, ret;

	if (!WIFEXITED(status)) {
		make_err_msg("pre-create-hook %d died involuntary",
			(int)pre_create_hook_pid);
		snapshot_creation_status = SCS_READY;
		compute_next_snapshot_time();
		ret = -E_INVOLUNTARY_EXIT;
		goto out;
	}
	es = WEXITSTATUS(status);
	if (es) {
		make_err_msg("pre-create-hook %d returned %d",
			(int)pre_create_hook_pid, es);
		snapshot_creation_status = SCS_READY;
		compute_next_snapshot_time();
		ret = -E_BAD_EXIT_CODE;
		goto out;
	}
	snapshot_creation_status = SCS_PRE_HOOK_SUCCESS;
	ret = 1;
out:
	pre_create_hook_pid = 0;
	return ret;
}

int handle_sigchld()
{
	pid_t pid;
	int status, ret = reap_child(&pid, &status);

	if (ret <= 0)
		return ret;
	if (pid == rsync_pid)
		return handle_rsync_exit(status);
	if (pid == rm_pid)
		return handle_rm_exit(status);
	if (pid == pre_create_hook_pid)
		return handle_pre_create_hook_exit(status);
	if (pid == post_create_hook_pid) {
		snapshot_creation_status = SCS_READY;
		compute_next_snapshot_time();
		return 1;
	}
	DSS_EMERG_LOG("BUG: unknown process %d died\n", (int)pid);
	exit(EXIT_FAILURE);
}

void handle_signal(void)
{
	int sig, ret = next_signal();

	if (ret <= 0)
		goto out;
	sig = ret;
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		restart_rsync_process();
		kill_process(rsync_pid);
		kill_process(rm_pid);
		exit(EXIT_FAILURE);
	case SIGHUP:
		handle_sighup();
		ret = 1;
		break;
	case SIGCHLD:
		ret = handle_sigchld();
		break;
	}
out:
	if (ret < 0)
		log_err_msg(ERROR, -ret);
}

int get_oldest(const char *dirname, void *private)
{
	struct edge_snapshot_data *esd = private;
	struct snapshot s;
	int ret = is_snapshot(dirname, esd->now, &s);

	if (ret <= 0)
		return 1;
	if (s.creation_time > esd->snap.creation_time)
		return 1;
	free(esd->snap.name);
	esd->snap = s;
	return 1;
}

int remove_oldest_snapshot()
{
	int ret;
	struct edge_snapshot_data esd = {
		.now = get_current_time(),
		.snap = {.creation_time = LLONG_MAX}
	};
	for_each_subdir(get_oldest, &esd);
	if (!esd.snap.name) /* no snapshot found */
		return 0;
	DSS_INFO_LOG("oldest snapshot: %s\n", esd.snap.name);
	ret = 0;
	if (esd.snap.creation_time == current_snapshot_creation_time)
		goto out; /* do not remove the snapshot currently being created */
	ret = remove_snapshot(&esd.snap);
out:
	free(esd.snap.name);
	return ret;
}

/* TODO: Also consider number of inodes. */
int disk_space_low(void)
{
	struct disk_space ds;
	int ret = get_disk_space(".", &ds);

	if (ret < 0)
		return ret;
	if (conf.min_free_mb_arg)
		if (ds.free_mb < conf.min_free_mb_arg)
			return 1;
	if (conf.min_free_percent_arg)
		if (ds.percent_free < conf.min_free_percent_arg)
			return 1;
	return 0;
}

int try_to_free_disk_space(int low_disk_space)
{
	int ret;
	struct snapshot_list sl;

	get_snapshot_list(&sl);
	ret = remove_outdated_snapshot(&sl);
	if (ret) /* error, or we are removing something */
		goto out;
	/* no outdated snapshot */
	ret = remove_redundant_snapshot(&sl);
	if (ret)
		goto out;
	ret = 0;
	if (!low_disk_space)
		goto out;
	DSS_WARNING_LOG("disk space low and nothing obvious to remove\n");
	ret = remove_oldest_snapshot();
	if (ret)
		goto out;
	make_err_msg("uhuhu: not enough disk space for a single snapshot");
	ret= -ENOSPC;
out:
	free_snapshot_list(&sl);
	return ret;
}

int select_loop(void)
{
	int ret;
	struct timeval tv = {.tv_sec = 0, .tv_usec = 0};

	for (;;) {
		fd_set rfds;
		int low_disk_space;
		char **rsync_argv;
		struct timeval now, *tvp = &tv;

		if (rsync_pid)
			tv.tv_sec = 60; /* check every 60 seconds for free disk space */
		else if (rm_pid)
			tvp = NULL; /* sleep until rm process dies */
		FD_ZERO(&rfds);
		FD_SET(signal_pipe, &rfds);
		DSS_DEBUG_LOG("tvp: %p, tv_sec: %lu\n", tvp, (long unsigned) tv.tv_sec);
		ret = dss_select(signal_pipe + 1, &rfds, NULL, tvp);
		if (ret < 0)
			return ret;
		if (FD_ISSET(signal_pipe, &rfds))
			handle_signal();
		if (rm_pid)
			continue;
		ret = disk_space_low();
		if (ret < 0)
			break;
		low_disk_space = ret;
		if (low_disk_space)
			stop_rsync_process();
		ret = try_to_free_disk_space(low_disk_space);
		if (ret < 0)
			break;
		if (rm_pid)
			continue;
		restart_rsync_process();
		gettimeofday(&now, NULL);
		if (tv_diff(&next_snapshot_time, &now, &tv) > 0)
			continue;
		switch (snapshot_creation_status) {
		case SCS_READY:
			ret = pre_create_hook();
			if (ret < 0)
				goto out;
			continue;
		case SCS_PRE_HOOK_RUNNING:
			continue;
		case SCS_PRE_HOOK_SUCCESS:
			create_rsync_argv(&rsync_argv, &current_snapshot_creation_time);
			ret = create_snapshot(rsync_argv);
			free_rsync_argv(rsync_argv);
			if (ret < 0)
				goto out;
			continue;
		case SCS_RSYNC_RUNNING:
			continue;
		case SCS_RSYNC_SUCCESS:
			ret = post_create_hook();
			if (ret < 0)
				goto out;
			continue;
		case SCS_POST_HOOK_RUNNING:
			continue;
		}
	}
out:
	return ret;
}

int com_run(void)
{
	int ret;

	if (conf.dry_run_given) {
		make_err_msg("dry_run not supported by this command");
		return -E_SYNTAX;
	}
	ret = install_sighandler(SIGHUP);
	if (ret < 0)
		return ret;
	compute_next_snapshot_time();
	return select_loop();
}

void log_disk_space(struct disk_space *ds)
{
	DSS_INFO_LOG("free: %uM/%uM (%u%%), %u%% inodes unused\n",
		ds->free_mb, ds->total_mb, ds->percent_free,
		ds->percent_free_inodes);
}

int com_prune(void)
{
	int ret;
	struct snapshot_list sl;
	struct disk_space ds;

	ret = get_disk_space(".", &ds);
	if (ret < 0)
		return ret;
	log_disk_space(&ds);
	for (;;) {
		get_snapshot_list(&sl);
		ret = remove_outdated_snapshot(&sl);
		free_snapshot_list(&sl);
		if (ret < 0)
			return ret;
		if (!ret)
			break;
		ret = wait_for_rm_process();
		if (ret < 0)
			goto out;
	}
	for (;;) {
		get_snapshot_list(&sl);
		ret = remove_redundant_snapshot(&sl);
		free_snapshot_list(&sl);
		if (ret < 0)
			return ret;
		if (!ret)
			break;
		ret = wait_for_rm_process();
		if (ret < 0)
			goto out;
	}
	return 1;
out:
	return ret;
}

int com_create(void)
{
	int ret, status;
	char **rsync_argv;

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
	ret = pre_create_hook();
	if (ret < 0)
		return ret;
	if (pre_create_hook_pid) {
		ret = wait_for_process(pre_create_hook_pid, &status);
		if (ret < 0)
			return ret;
		ret = handle_pre_create_hook_exit(status);
		if (ret < 0)
			return ret;
	}
	create_rsync_argv(&rsync_argv, &current_snapshot_creation_time);
	ret = create_snapshot(rsync_argv);
	if (ret < 0)
		goto out;
	ret = wait_for_process(rsync_pid, &status);
	if (ret < 0)
		goto out;
	ret = handle_rsync_exit(status);
	if (ret < 0)
		goto out;
	post_create_hook();
	if (post_create_hook_pid)
		ret = wait_for_process(post_create_hook_pid, &status);
out:
	free_rsync_argv(rsync_argv);
	return ret;
}

int com_ls(void)
{
	int i;
	struct snapshot_list sl;
	struct snapshot *s;
	get_snapshot_list(&sl);
	FOR_EACH_SNAPSHOT(s, i, &sl)
		dss_msg("%u\t%s\n", s->interval, s->name);
	free_snapshot_list(&sl);
	return 1;
}

__noreturn void clean_exit(int status)
{
	free(dss_error_txt);
	exit(status);
}
static void setup_signal_handling(void)
{
	int ret;

	DSS_INFO_LOG("setting up signal handlers\n");
	signal_pipe = signal_init(); /* always successful */
	ret = install_sighandler(SIGINT);
	if (ret < 0)
		goto err;
	ret = install_sighandler(SIGTERM);
	if (ret < 0)
		goto err;
	ret = install_sighandler(SIGCHLD);
	if (ret < 0)
		goto err;
	return;
err:
	DSS_EMERG_LOG("could not install signal handlers\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	int ret;
	struct cmdline_parser_params params = {
		.override = 0,
		.initialize = 1,
		.check_required = 0,
		.check_ambiguity = 0
	};

	cmdline_parser_ext(argc, argv, &conf, &params); /* aborts on errors */
	parse_config_file(0);

	if (conf.daemon_given)
		daemon_init();
	setup_signal_handling();
	ret = call_command_handler();
	if (ret < 0)
		log_err_msg(EMERG, -ret);
	clean_exit(ret >= 0? EXIT_SUCCESS : EXIT_FAILURE);
}
