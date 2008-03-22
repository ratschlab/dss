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

static inline struct snapshot *get_oldest_snapshot(struct snapshot_list *sl)
{
	if (!sl->num_snapshots)
		return NULL;
	return sl->snapshots[0];
}
