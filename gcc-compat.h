# define inline		inline __attribute__ ((always_inline))
# define __noreturn	__attribute__ ((noreturn))
# define __malloc	__attribute__ ((malloc))
# define __a_unused	__attribute__ ((unused))
# define likely(x)	__builtin_expect (!!(x), 1)
# define unlikely(x)	__builtin_expect (!!(x), 0)
/* 
 * p is the number of the "format string" parameter, and q is 
 * the number of the first variadic parameter 
 */
# define __printf(p,q) __attribute__ ((format (printf, p, q)))
/*
 * as direct use of __printf(p,q) confuses doxygen, here are two extra macros
 * for those values p,q that are actually used by paraslash.
 */
#define  __printf_1_2 __printf(1,2)
#define  __printf_2_3 __printf(2,3)

# if __GNUC__ > 3  || (__GNUC__ == 3 && __GNUC_MINOR__ > 3)
# define __must_check	__attribute__ ((warn_unused_result))
# else
# define __must_check	/* no warn_unused_result */
# endif

#define _static_inline_ static inline
