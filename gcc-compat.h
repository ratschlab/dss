#define HAVE_GNUC(maj, min) \
	defined(__GNUC__) && \
	(__GNUC__ > maj || (__GNUC__ == maj && __GNUC_MINOR__ >= min))

#if HAVE_GNUC(2,5)
# define __noreturn	__attribute__ ((noreturn))
#else
# define __noreturn
#endif

#if HAVE_GNUC(3,0)
# define __malloc	__attribute__ ((malloc))
#else
# define __malloc
#endif

#if HAVE_GNUC(2,7)
# define __a_unused	__attribute__ ((unused))
#else
# define __a_unused
#endif

/* 
 * p is the number of the "format string" parameter, and q is 
 * the number of the first variadic parameter 
 */
#if HAVE_GNUC(2,3)
# define __printf(p,q) __attribute__ ((format (printf, p, q)))
#else
# define __printf(p,q)
#endif

/*
 * as direct use of __printf(p,q) confuses doxygen, here are two extra macros
 * for those values p,q that are actually used by paraslash.
 */
#define  __printf_1_2 __printf(1,2)
#define  __printf_2_3 __printf(2,3)

#if HAVE_GNUC(3,3)
# define __must_check	__attribute__ ((warn_unused_result))
#else
# define __must_check	/* no warn_unused_result */
#endif

#define _static_inline_ static inline
