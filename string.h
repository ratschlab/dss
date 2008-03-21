__must_check __malloc void *dss_realloc(void *p, size_t size);
__must_check __malloc void *dss_malloc(size_t size);
__must_check __malloc void *dss_calloc(size_t size);
__must_check __printf_1_2 __malloc char *make_message(const char *fmt, ...);
__must_check __malloc char *dss_strdup(const char *s);
__printf_1_2 void make_err_msg(const char* fmt,...);
__must_check __malloc char *get_homedir(void);
int dss_atoi64(const char *str, int64_t *value);
__must_check __malloc char *dss_logname(void);
__must_check unsigned split_args(char *args, char *** const argv_ptr, const char *delim);


/** \cond LLONG_MAX and LLONG_LIN might not be defined. */
#ifndef LLONG_MAX
#define LLONG_MAX (1 << (sizeof(long) - 1))
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#endif
/** \endcond */

