/*
 * Copyright (C) 2008 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

/** The possible states for snapshot creation/removal. */
enum hook_status {
	/** We are ready to take the next snapshot. */
	HS_READY,
	/** The pre-create/pre-remove hook has been started. */
	HS_PRE_RUNNING,
	/** The pre-create/pre-remove hook exited successfully. */
	HS_PRE_SUCCESS,
	/** The rsync/rm process is running. */
	HS_RUNNING,
	/** The rsync/rm process exited successfully. */
	HS_SUCCESS,
	/** The rsync/rm process needs to be restarted. */
	HS_NEEDS_RESTART,
	/** The post-create/post-remove hook has been started. */
	HS_POST_RUNNING,
};

/**
 * The status of a snapshot.
 *
 * The snapshot directories come in four different flavours, depending
 * on how the two staus flags are set. Examples:
 *
 * Complete, not being deleted: 1204565370-1204565371.Sun_Mar_02_2008_14_33-Sun_Mar_02_2008_14_43.
 * Complete, being deleted: 1204565370-1204565371.being_deleted.
 * Incomplete, not being deleted: 1204565370-incomplete.
 * incomplete, being deleted: 1204565370-incomplete.being_deleted.
 */
enum snapshot_status_flags {
	/** The rsync process terminated successfully. */
	SS_COMPLETE = 1,
	/** The rm process is running to remove this snapshot. */
	SS_BEING_DELETED = 2,
};

/** Describes one snapshot. */
struct snapshot {
	/** The name of the directory, relative to the destination dir. */
	char *name;
	/** Seconds after the epoch when this snapshot was created. */
	int64_t creation_time;
	/**
	 * Seconds after the epoch when creation of this snapshot completed.
	 * Only meaningful if the SS_COMPLTE bit is set.
	 */
	int64_t completion_time;
	/** See \ref snapshot_status_flags. */
	enum snapshot_status_flags flags;
	/** The interval this snapshot belongs to. */
	unsigned interval;
};

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

/** Iterate over all snapshots in a snapshot list. */
#define FOR_EACH_SNAPSHOT(s, i, sl) \
	for ((i) = 0; (i) < (sl)->num_snapshots && ((s) = (sl)->snapshots[(i)]); (i)++)

/** Iterate backwards over all snapshots in a snapshot list. */
#define FOR_EACH_SNAPSHOT_REVERSE(s, i, sl) \
	for ((i) = (sl)->num_snapshots; (i) > 0 && ((s) = (sl)->snapshots[(i - 1)]); (i)--)


unsigned desired_number_of_snapshots(int interval_num, int num_intervals);
void get_snapshot_list(struct snapshot_list *sl, int unit_interval,
		int num_intervals);
void free_snapshot_list(struct snapshot_list *sl);
__malloc char *incomplete_name(int64_t start);
__malloc char *being_deleted_name(struct snapshot *s);
int complete_name(int64_t start, int64_t end, char **result);
__malloc char *name_of_newest_complete_snapshot(struct snapshot_list *sl);

/**
 * Get the oldest snapshot in a snapshot list.
 */
_static_inline_ struct snapshot *get_oldest_snapshot(struct snapshot_list *sl)
{
	if (!sl->num_snapshots)
		return NULL;
	return sl->snapshots[0];
}
