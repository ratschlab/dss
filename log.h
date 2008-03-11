
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

/** \cond */
#if DEBUG > COMPILE_TIME_LOGLEVEL
#define DSS_DEBUG_LOG(f,...) dss_log(DEBUG, "%s: " f, __FUNCTION__, ## __VA_ARGS__)
#else
#define DSS_DEBUG_LOG(...) do {;} while (0)
#endif

#if INFO > COMPILE_TIME_LOGLEVEL
#define DSS_INFO_LOG(f,...) dss_log(INFO, "%s: " f, __FUNCTION__, ## __VA_ARGS__)
#else
#define DSS_INFO_LOG(...) do {;} while (0)
#endif

#if NOTICE > COMPILE_TIME_LOGLEVEL
#define DSS_NOTICE_LOG(f,...) dss_log(NOTICE, "%s: " f, __FUNCTION__, ## __VA_ARGS__)
#else
#define DSS_NOTICE_LOG(...) do {;} while (0)
#endif

#if WARNING > COMPILE_TIME_LOGLEVEL
#define DSS_WARNING_LOG(f,...) dss_log(WARNING, "%s: " f, __FUNCTION__, ##  __VA_ARGS__)
#else
#define DSS_WARNING_LOG(...) do {;} while (0)
#endif

#if ERROR > COMPILE_TIME_LOGLEVEL
#define DSS_ERROR_LOG(f,...) dss_log(ERROR, "%s: " f, __FUNCTION__, ## __VA_ARGS__)
#else
#define DSS_ERROR_LOG(...) do {;} while (0)
#endif

#if CRIT > COMPILE_TIME_LOGLEVEL
#define DSS_CRIT_LOG(f,...) dss_log(CRIT, "%s: " f, __FUNCTION__, ## __VA_ARGS__)
#else
#define DSS_CRIT_LOG(...) do {;} while (0)
#endif

#if EMERG > COMPILE_TIME_LOGLEVEL
#define DSS_EMERG_LOG(f,...) dss_log(EMERG, "%s: " f, __FUNCTION__, ## __VA_ARGS__)
#else
#define DSS_EMERG_LOG(...)
#endif
