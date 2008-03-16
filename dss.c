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


struct gengetopt_args_info conf;
char *dss_error_txt = NULL;

DEFINE_DSS_ERRLIST;


/* a litte cpp magic helps to DRY */
#define COMMANDS \
	COMMAND(ls) \
	COMMAND(create) \
	COMMAND(prune) \
	COMMAND(run)
#define COMMAND(x) int com_ ##x(int, char * const * const);
COMMANDS
#undef COMMAND
#define COMMAND(x) if (conf.x ##_given) return com_ ##x(argc, argv);
int call_command_handler(int argc, char * const * const argv)
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
	SS_COMPLETE = 1,
	SS_BEING_DELETED = 2,
};

struct snapshot {
	char *name;
	int64_t creation_time;
	int64_t completion_time;
	enum snapshot_status_flags flags;
	unsigned interval;
};

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
	if (strcmp(dot + 1, "being_deleted"))
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
	free(sl->snapshots);
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
		ret = waitpid(pid, status, 0);
		if (ret >= 0 || errno != EINTR)
			break;
	}
	if (ret < 0) {
		ret = -ERRNO_TO_DSS_ERROR(errno);
		make_err_msg("failed to wait for process %d", (int)pid);
	} else
		log_termination_msg(pid, *status);
	return ret;
}

int remove_snapshot(struct snapshot *s, pid_t *pid)
{
	int fds[3] = {0, 0, 0};
	char *new_name = being_deleted_name(s);
	int ret = dss_rename(s->name, new_name);
	char *argv[] = {"rm", "-rf", new_name, NULL};

	if (ret < 0)
		goto out;
	DSS_NOTICE_LOG("removing %s (interval = %i)\n", s->name, s->interval);
	ret = dss_exec(pid, argv[0], argv, fds);
out:
	free(new_name);
	return ret;
}

int remove_redundant_snapshot(struct snapshot_list *sl,
		int dry_run, pid_t *pid)
{
	int ret, i, interval;
	struct snapshot *s;
	unsigned missing = 0;

	DSS_INFO_LOG("looking for intervals containing too many snapshots\n");
	for (interval = conf.num_intervals_arg - 1; interval >= 0; interval--) {
		unsigned keep = 1<<(conf.num_intervals_arg - interval - 1);
		unsigned num = sl->interval_count[interval];
		struct snapshot *victim = NULL, *prev = NULL;
		int64_t score = LONG_MAX;

		if (keep >= num)
			missing += keep - num;
		DSS_DEBUG_LOG("interval %i: keep: %u, have: %u, missing: %u\n",
			interval, keep, num, missing);
		if (keep + missing >= num)
			continue;
		/* redundant snapshot in this interval, pick snapshot with lowest score */
		FOR_EACH_SNAPSHOT(s, i, sl) {
			int64_t this_score;

			DSS_DEBUG_LOG("checking %s\n", s->name);
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
			DSS_DEBUG_LOG("%s: score %lli\n", s->name, (long long)score);
			if (this_score < score) {
				score = this_score;
				victim = s;
			}
			prev = s;
		}
		assert(victim);
		if (dry_run) {
			printf("%s would be removed (interval = %i)\n",
				victim->name, victim->interval);
			continue;
		}
		ret = remove_snapshot(victim, pid);
		return ret < 0? ret : 1;
	}
	return 0;
}

int remove_old_snapshot(struct snapshot_list *sl, int dry_run, pid_t *pid)
{
	int i, ret;
	struct snapshot *s;

	DSS_INFO_LOG("looking for snapshots belonging to intervals greater than %d\n",
		conf.num_intervals_arg);
	FOR_EACH_SNAPSHOT(s, i, sl) {
		if (s->interval <= conf.num_intervals_arg)
			continue;
		if (dry_run) {
			printf("%s would be removed (interval = %i)\n",
				s->name, s->interval);
			continue;
		}
		ret = remove_snapshot(s, pid);
		if (ret < 0)
			return ret;
		return 1;
	}
	return 0;
}

int wait_for_rm_process(pid_t pid)
{
	int status, es, ret = wait_for_process(pid, &status);
	if (ret < 0)
		return ret;
	if (!WIFEXITED(status)) {
		ret = E_INVOLUNTARY_EXIT;
		make_err_msg("rm process %d died involuntary", (int)pid);
		return ret;
	}
	es = WEXITSTATUS(status);
	if (es) {
		ret = -E_BAD_EXIT_CODE;
		make_err_msg("rm process %d returned %d", (int)pid, es);
		return ret;
	}
	return 1;
}

int com_run(int argc, char * const * argv)
{
	return 42;
}

int com_prune(int argc, char * const * argv)
{
	int ret, dry_run = 0;
	struct snapshot_list sl;
	pid_t pid;

	if (argc > 2) {
		make_err_msg("too many arguments");
		return -E_SYNTAX;
	}
	if (argc == 2) {
		if (strcmp(argv[1], "-d")) {
			make_err_msg("%s", argv[1]);
			return -E_SYNTAX;
		}
		dry_run = 1;
	}
	for (;;) {
		get_snapshot_list(&sl);
		ret = remove_old_snapshot(&sl, dry_run, &pid);
		free_snapshot_list(&sl);
		if (ret < 0)
			return ret;
		if (!ret)
			break;
		ret = wait_for_rm_process(pid);
		if (ret < 0)
			goto out;
	}
	for (;;) {
		get_snapshot_list(&sl);
		ret = remove_redundant_snapshot(&sl, dry_run, &pid);
		free_snapshot_list(&sl);
		if (ret < 0)
			return ret;
		if (!ret)
			break;
		ret = wait_for_rm_process(pid);
		if (ret < 0)
			goto out;
	}
	return 1;
out:
	return ret;
}

struct newest_snapshot_data {
	char * newest_name;
	int64_t newest_creation_time;
	int64_t now;
};

int get_newest_complete(const char *dirname, void *private)
{
	struct newest_snapshot_data *nsd = private;
	struct snapshot s;
	int ret = is_snapshot(dirname, nsd->now, &s);

	if (ret <= 0)
		return 1;
	if (s.creation_time < nsd->newest_creation_time)
		return 1;
	nsd->newest_creation_time = s.creation_time;
	free(nsd->newest_name);
	nsd->newest_name = s.name;
	return 1;
}

__malloc char *name_of_newest_complete_snapshot(void)
{
	struct newest_snapshot_data nsd = {
		.now = get_current_time(),
		.newest_creation_time = -1
	};
	for_each_subdir(get_newest_complete, &nsd);
	return nsd.newest_name;
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
		DSS_INFO_LOG("no previous snapshot found");
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

int create_snapshot(char **argv, pid_t *pid)
{
	int fds[3] = {0, 0, 0};

	return dss_exec(pid, argv[0], argv, fds);
}

int rename_incomplete_snapshot(int64_t start)
{
	char *old_name, *new_name;
	int ret;

	ret = complete_name(start, get_current_time(), &new_name);
	if (ret < 0)
		return ret;
	old_name = incomplete_name(start);
	ret = dss_rename(old_name, new_name);
	if (ret >= 0)
		DSS_NOTICE_LOG("%s -> %s\n", old_name, new_name);
	free(old_name);
	free(new_name);
	return ret;
}

int com_create(int argc, __a_unused char * const * argv)
{
	int ret, status, es;
	char **rsync_argv;
	int64_t snapshot_num;
	pid_t pid;

	if (argc != 1) {
		ret = -E_SYNTAX;
		make_err_msg("create: no args expected, %d given", argc - 1);
		return ret;
	}
	create_rsync_argv(&rsync_argv, &snapshot_num);
	DSS_NOTICE_LOG("creating snapshot %lli\n", (long long)snapshot_num);
	ret = create_snapshot(rsync_argv, &pid);
	if (ret < 0)
		goto out;
	ret = wait_for_process(pid, &status);
	if (ret < 0)
		goto out;
	if (!WIFEXITED(status)) {
		ret = E_INVOLUNTARY_EXIT;
		make_err_msg("rsync process %d died involuntary", (int)pid);
		goto out;
	}
	es = WEXITSTATUS(status);
	if (es != 0 && es != 23 && es != 24) {
		ret = -E_BAD_EXIT_CODE;
		make_err_msg("rsync process %d returned %d", (int)pid, es);
		goto out;
	}
	ret = rename_incomplete_snapshot(snapshot_num);
out:
	free_rsync_argv(rsync_argv);
	return ret;
}

int com_ls(int argc, __a_unused char * const * argv)
{
	int i, ret;
	struct snapshot_list sl;
	struct snapshot *s;
	if (argc != 1) {
		ret = -E_SYNTAX;
		make_err_msg("ls: no args expected, %d given", argc - 1);
		return ret;
	}
	get_snapshot_list(&sl);
	FOR_EACH_SNAPSHOT(s, i, &sl)
		printf("%u\t%s\n", s->interval, s->name);
	free_snapshot_list(&sl);
	return 1;
}

/* TODO: Unlink pid file */
__noreturn void clean_exit(int status)
{
	//kill(0, SIGTERM);
	free(dss_error_txt);
	exit(status);
}

__printf_2_3 void dss_log(int ll, const char* fmt,...)
{
	va_list argp;
	if (ll < conf.loglevel_arg)
		return;
	va_start(argp, fmt);
	vfprintf(stderr, fmt, argp);
	va_end(argp);
}

int read_config_file(void)
{
	int ret;
	char *config_file;
	struct stat statbuf;

	if (conf.config_file_given)
		config_file = dss_strdup(conf.config_file_arg);
	else {
		char *home = get_homedir();
		config_file = make_message("%s/.dssrc", home);
		free(home);
	}
	ret = stat(config_file, &statbuf);
	if (ret && conf.config_file_given) {
		ret = -ERRNO_TO_DSS_ERROR(errno);
		make_err_msg("failed to stat config file %s", config_file);
		goto out;
	}
	if (!ret) {
		struct cmdline_parser_params params = {
			.override = 0,
			.initialize = 0,
			.check_required = 0,
			.check_ambiguity = 0
		};
		cmdline_parser_config_file(config_file, &conf, &params);
	}
	if (!conf.source_dir_given || !conf.dest_dir_given) {
		ret = -E_SYNTAX;
		make_err_msg("you need to specify both source_dir and dest_dir");
		goto out;
	}
	ret = 1;
out:
	free(config_file);
	return ret;
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

int main(int argc, char **argv)
{
	int ret;

	cmdline_parser(argc, argv, &conf); /* aborts on errors */
	if (conf.inputs_num) {
		ret = -E_SYNTAX;
		make_err_msg("additional non-options given");
		goto out;
	}
	ret = read_config_file();
	if (ret < 0)
		goto out;
	ret = check_config();
	if (ret < 0)
		goto out;
	ret = dss_chdir(conf.dest_dir_arg);
	if (ret < 0)
		goto out;
	ret = call_command_handler(conf.inputs_num, conf.inputs);
out:
	if (ret < 0)
		log_err_msg(EMERG, -ret);
	clean_exit(ret >= 0? EXIT_SUCCESS : EXIT_FAILURE);
}
