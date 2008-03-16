
/** \file daemon.h exported symbols from daemon.c */

void daemon_init(void);
FILE *open_log(const char *logfile_name);
void close_log(FILE* logfile);
void log_welcome(int loglevel);
