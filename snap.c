/*
 * Copyright (C) 2008-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "gcc-compat.h"
#include "error.h"
#include "snap.h"
#include "string.h"
#include "time.h"
#include "fd.h"

/**
 * Wrapper for isdigit.
 * NetBSD needs this.
 *
 * The values should be cast to an unsigned char first, then to int.
 * Why? Because the isdigit (as do all other is/to functions/macros)
 * expect a number from 0 upto and including 255 as their (int) argument.
 * Because char is signed on most systems, casting it to int immediately
 * gives the functions an argument between -128 and 127 (inclusive),
 * which they will use as an array index, and which will thus fail
 * horribly for characters which have their most significant bit set.
 */
#define dss_isdigit(c) isdigit((int)(unsigned char)(c))


/**
 * Return the desired number of snapshots of an interval.
 */
unsigned desired_number_of_snapshots(int interval_num, int num_intervals)
{
	unsigned n;

	assert(interval_num >= 0);

	if (interval_num >= num_intervals)
		return 0;
	n = num_intervals - interval_num - 1;
	return 1 << n;
}

/* return: Whether dirname is a snapshot directory (0: no, 1: yes) */
static int is_snapshot(const char *dirname, int64_t now, int unit_interval,
		struct snapshot *s)
{
	int i, ret;
	char *dash, *dot, *tmp;
	int64_t num;

	assert(dirname);
	dash = strchr(dirname, '-');
	if (!dash || !dash[1] || dash == dirname)
		return 0;
	for (i = 0; dirname[i] != '-'; i++)
		if (!dss_isdigit(dirname[i]))
			return 0;
	tmp = dss_strdup(dirname);
	tmp[i] = '\0';
	ret = dss_atoi64(tmp, &num);
	free(tmp);
	if (ret < 0)
		return 0;
	assert(num >= 0);
	if (num > now)
		return 0;
	s->creation_time = num;
	//DSS_DEBUG_LOG("%s start time: %lli\n", dirname, (long long)s->creation_time);
	s->interval = (long long) ((now - s->creation_time)
		/ unit_interval / 24 / 3600);
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
		if (!dss_isdigit(tmp[i]))
			return 0;
	tmp = dss_strdup(dash + 1);
	tmp[i] = '\0';
	ret = dss_atoi64(tmp, &num);
	free(tmp);
	if (ret < 0)
		return 0;
	if (num > now || num < s->creation_time)
		return 0;
	s->completion_time = num;
	s->flags = SS_COMPLETE;
	if (!strcmp(dot + 1, "being_deleted"))
		s->flags |= SS_BEING_DELETED;
success:
	s->name = dss_strdup(dirname);
	return 1;
}

struct add_snapshot_data {
	int unit_interval;
	int num_intervals;
	struct snapshot_list *sl;
};

/** Compute the minimum of \a a and \a b. */
#define DSS_MIN(a,b) ((a) < (b) ? (a) : (b))

static int add_snapshot(const char *dirname, void *private)
{
	struct add_snapshot_data *asd = private;
	struct snapshot_list *sl = asd->sl;
	struct snapshot s;
	int ret = is_snapshot(dirname, sl->now, asd->unit_interval, &s);

	if (!ret)
		return 1;
	if (sl->num_snapshots >= sl->array_size) {
		sl->array_size = 2 * sl->array_size + 1;
		sl->snapshots = dss_realloc(sl->snapshots,
			sl->array_size * sizeof(struct snapshot *));
	}
	sl->snapshots[sl->num_snapshots] = dss_malloc(sizeof(struct snapshot));
	*(sl->snapshots[sl->num_snapshots]) = s;
	sl->interval_count[DSS_MIN(s.interval, asd->num_intervals)]++;
	sl->num_snapshots++;
	return 1;
}

#define NUM_COMPARE(x, y) ((int)((x) < (y)) - (int)((x) > (y)))

static int compare_snapshots(const void *a, const void *b)
{
	struct snapshot *s1 = *(struct snapshot **)a;
	struct snapshot *s2 = *(struct snapshot **)b;
	return NUM_COMPARE(s2->creation_time, s1->creation_time);
}


void get_snapshot_list(struct snapshot_list *sl, int unit_interval,
		int num_intervals)
{
	struct add_snapshot_data asd = {
		.unit_interval = unit_interval,
		.num_intervals = num_intervals,
		.sl = sl
	};
	sl->now = get_current_time();
	sl->num_snapshots = 0;
	sl->array_size = 0;
	sl->snapshots = NULL;
	sl->interval_count = dss_calloc((num_intervals + 1) * sizeof(unsigned));
	for_each_subdir(add_snapshot, &asd);
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

__malloc char *incomplete_name(int64_t start)
{
	return make_message("%" PRId64 "-incomplete", start);
}

__malloc char *being_deleted_name(struct snapshot *s)
{
	if (s->flags & SS_COMPLETE)
		return make_message("%" PRId64 "-%" PRId64 ".being_deleted",
			s->creation_time, s->completion_time);
	return make_message("%" PRId64 "-incomplete.being_deleted", s->creation_time);
}

int complete_name(int64_t start, int64_t end, char **result)
{
	struct tm start_tm, end_tm;
	time_t *start_seconds = (time_t *) (uint64_t *)&start; /* STFU, gcc */
	time_t *end_seconds = (time_t *) (uint64_t *)&end; /* STFU, gcc */
	char start_str[200], end_str[200];

	if (!localtime_r(start_seconds, &start_tm))
		return -E_LOCALTIME;
	if (!localtime_r(end_seconds, &end_tm))
		return -E_LOCALTIME;
	if (!strftime(start_str, sizeof(start_str), "%a_%b_%d_%Y_%H_%M_%S", &start_tm))
		return -E_STRFTIME;
	if (!strftime(end_str, sizeof(end_str), "%a_%b_%d_%Y_%H_%M_%S", &end_tm))
		return -E_STRFTIME;
	*result = make_message("%" PRId64 "-%" PRId64 ".%s-%s", start, end,
		start_str, end_str);
	return 1;
}

__malloc char *name_of_newest_complete_snapshot(struct snapshot_list *sl)
{
	struct snapshot *s;
	int i;
	char *name = NULL;

	FOR_EACH_SNAPSHOT_REVERSE(s, i, sl) {
		if (s->flags != SS_COMPLETE) /* incomplete or being deleted */
			continue;
		name = dss_strdup(s->name);
		break;
	}
	return name;
}

