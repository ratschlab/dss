/*
 * Copyright (C) 1997-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

/** debug loglevel, gets really noisy */
#define DEBUG 1
/** still noisy, but won't fill your disk */
#define INFO  2
/** normal, but significant event */
#define NOTICE 3
/** unexpected event that can be handled */
#define WARNING 4
/** unhandled error condition */
#define ERROR 5
/** system might be unreliable */
#define CRIT 6
/** last message before exit */
#define EMERG 7

/** Log messages with lower priority than that will not be compiled in. */
#define COMPILE_TIME_LOGLEVEL 0

/** Not all compilers support __func__ or an equivalent. */
#if (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L) && !defined(__GNUC__)
# if defined(_MSC_VER) && _MSC_VER >= 1300
#  define __func__ __FUNCTION__
# else
#  define DSS_NO_FUNC_NAMES
#  define __func__ "<unknown>"
# endif
#endif

/** \cond */
#if DEBUG > COMPILE_TIME_LOGLEVEL
#define DSS_DEBUG_LOG(args) \
	do { \
		dss_log_set_params(DEBUG, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_DEBUG_LOG(args) do {;} while (0)
#endif

#if INFO > COMPILE_TIME_LOGLEVEL
#define DSS_INFO_LOG(args) \
	do { \
		dss_log_set_params(INFO, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_INFO_LOG(args) do {;} while (0)
#endif

#if NOTICE > COMPILE_TIME_LOGLEVEL
#define DSS_NOTICE_LOG(args) \
	do { \
		dss_log_set_params(NOTICE, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_NOTICE_LOG(args) do {;} while (0)
#endif

#if WARNING > COMPILE_TIME_LOGLEVEL
#define DSS_WARNING_LOG(args) \
	do { \
		dss_log_set_params(WARNING, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_WARNING_LOG(args) do {;} while (0)
#endif

#if ERROR > COMPILE_TIME_LOGLEVEL
#define DSS_ERROR_LOG(args) \
	do { \
		dss_log_set_params(ERROR, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_ERROR_LOG(args) do {;} while (0)
#endif

#if CRIT > COMPILE_TIME_LOGLEVEL
#define DSS_CRIT_LOG(args) \
	do { \
		dss_log_set_params(CRIT, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_CRIT_LOG(args) do {;} while (0)
#endif

#if EMERG > COMPILE_TIME_LOGLEVEL
#define DSS_EMERG_LOG(args) \
	do { \
		dss_log_set_params(EMERG, __FILE__, __LINE__, __func__); \
		dss_log args ; \
	} while (0)
#else
#define DSS_EMERG_LOG(args)
#endif
