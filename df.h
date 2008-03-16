struct disk_space {
	unsigned total_mb;
	unsigned free_mb;
	unsigned percent_free;
	unsigned percent_free_inodes;
};

int get_disk_space(const char *path, struct disk_space *result);

