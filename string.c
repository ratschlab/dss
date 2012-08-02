/*
 * Copyright (C) 2004-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>


#include "gcc-compat.h"
#include "log.h"
#include "error.h"
#include "string.h"

/**
 * Write a message to a dynamically allocated string.
 *
 * \param fmt Usual format string.
 * \param p Result pointer.
 *
 * \sa printf(3). */
#define VSPRINTF(fmt, p) \
{ \
	int n; \
	size_t size = 100; \
	p = dss_malloc(size); \
	while (1) { \
		va_list ap; \
		/* Try to print in the allocated space. */ \
		va_start(ap, fmt); \
		n = vsnprintf(p, size, fmt, ap); \
		va_end(ap); \
		/* If that worked, return the string. */ \
		if (n > -1 && n < size) \
			break; \
		/* Else try again with more space. */ \
		if (n > -1) /* glibc 2.1 */ \
			size = n + 1; /* precisely what is needed */ \
		else /* glibc 2.0 */ \
			size *= 2; /* twice the old size */ \
		p = dss_realloc(p, size); \
	} \
}

/**
 * dss' version of realloc().
 *
 * \param p Pointer to the memory block, may be \p NULL.
 * \param size The desired new size.
 *
 * A wrapper for realloc(3). It calls \p exit(\p EXIT_FAILURE) on errors,
 * i.e. there is no need to check the return value in the caller.
 *
 * \return A pointer to  the newly allocated memory, which is suitably aligned
 * for any kind of variable and may be different from \a p.
 *
 * \sa realloc(3).
 */
__must_check __malloc void *dss_realloc(void *p, size_t size)
{
	/*
	 * No need to check for NULL pointers: If p is NULL, the  call
	 * to realloc is equivalent to malloc(size)
	 */
	assert(size);
	if (!(p = realloc(p, size))) {
		DSS_EMERG_LOG("realloc failed (size = %zu), aborting\n",
			size);
		exit(EXIT_FAILURE);
	}
	return p;
}

/**
 * dss' version of malloc().
 *
 * \param size The desired new size.
 *
 * A wrapper for malloc(3) which exits on errors.
 *
 * \return A pointer to the allocated memory, which is suitably aligned for any
 * kind of variable.
 *
 * \sa malloc(3).
 */
__must_check __malloc void *dss_malloc(size_t size)
{
	void *p;
	assert(size);
	p = malloc(size);

	if (!p) {
		DSS_EMERG_LOG("malloc failed (size = %zu),  aborting\n",
			size);
		exit(EXIT_FAILURE);
	}
	return p;
}

/**
 * dss' version of calloc().
 *
 * \param size The desired new size.
 *
 * A wrapper for calloc(3) which exits on errors.
 *
 * \return A pointer to the allocated and zeroed-out memory, which is suitably
 * aligned for any kind of variable.
 *
 * \sa calloc(3)
 */
__must_check __malloc void *dss_calloc(size_t size)
{
	void *ret = dss_malloc(size);

	memset(ret, 0, size);
	return ret;
}

/**
 * dss' version of strdup().
 *
 * \param s The string to be duplicated.
 *
 * A wrapper for strdup(3). It calls \p exit(EXIT_FAILURE) on errors, i.e.
 * there is no need to check the return value in the caller.
 *
 * \return A pointer to the duplicated string. If \p s was the NULL pointer,
 * an pointer to an empty string is returned.
 *
 * \sa strdup(3)
 */

__must_check __malloc char *dss_strdup(const char *s)
{
	char *ret;

	if ((ret = strdup(s? s: "")))
		return ret;
	DSS_EMERG_LOG("strdup failed, aborting\n");
	exit(EXIT_FAILURE);
}

/**
 * Allocate a sufficiently large string and print into it.
 *
 * \param fmt A usual format string.
 *
 * Produce output according to \p fmt. No artificial bound on the length of the
 * resulting string is imposed.
 *
 * \return This function either returns a pointer to a string that must be
 * freed by the caller or aborts without returning.
 *
 * \sa printf(3).
 */
__must_check __printf_1_2 __malloc char *make_message(const char *fmt, ...)
{
	char *msg;

	VSPRINTF(fmt, msg);
	return msg;
}

/**
 * Get the home directory of the current user.
 *
 * \return A dynammically allocated string that must be freed by the caller. If
 * the home directory could not be found, this function returns "/tmp".
 */
__must_check __malloc char *get_homedir(void)
{
	struct passwd *pw = getpwuid(getuid());
	return dss_strdup(pw? pw->pw_dir : "/tmp");
}

/**
 * Convert a string to a 64-bit signed integer value.
 *
 * \param str The string to be converted.
 * \param value Result pointer.
 *
 * \return Standard.
 *
 * \sa strtol(3), atoi(3).
 */
int dss_atoi64(const char *str, int64_t *value)
{
	char *endptr;
	long long tmp;

	errno = 0; /* To distinguish success/failure after call */
	tmp = strtoll(str, &endptr, 10);
	if (errno == ERANGE && (tmp == LLONG_MAX || tmp == LLONG_MIN))
		return -E_ATOI_OVERFLOW;
	if (errno != 0 && tmp == 0) /* other error */
		return -E_STRTOLL;
	if (endptr == str)
		return -E_ATOI_NO_DIGITS;
	if (*endptr != '\0') /* Further characters after number */
		return -E_ATOI_JUNK_AT_END;
	*value = tmp;
	return 1;
}

/**
 * Get the logname of the current user.
 *
 * \return A dynammically allocated string that must be freed by the caller. On
 * errors, the string "unknown user" is returned, i.e. this function never
 * returns \p NULL.
 *
 * \sa getpwuid(3).
 */
__must_check __malloc char *dss_logname(void)
{
	struct passwd *pw = getpwuid(getuid());
	return dss_strdup(pw? pw->pw_name : "unknown_user");
}

/**
 * Split string and return pointers to its parts.
 *
 * \param args The string to be split.
 * \param argv_ptr Pointer to the list of substrings.
 * \param delim Delimiter.
 *
 * This function modifies \a args by replacing each occurance of \a delim by
 * zero. A \p NULL-terminated array of pointers to char* is allocated dynamically
 * and these pointers are initialized to point to the broken-up substrings
 * within \a args. A pointer to this array is returned via \a argv_ptr.
 *
 * \return The number of substrings found in \a args.
 */
unsigned split_args(char *args, char *** const argv_ptr, const char *delim)
{
	char *p = args;
	char **argv;
	size_t n = 0, i, j;

	p = args + strspn(args, delim);
	for (;;) {
		i = strcspn(p, delim);
		if (!i)
			break;
		p += i;
		n++;
		p += strspn(p, delim);
	}
	*argv_ptr = dss_malloc((n + 1) * sizeof(char *));
	argv = *argv_ptr;
	i = 0;
	p = args + strspn(args, delim);
	while (p) {
		argv[i] = p;
		j = strcspn(p, delim);
		if (!j)
			break;
		p += strcspn(p, delim);
		if (*p) {
			*p = '\0';
			p++;
			p += strspn(p, delim);
		}
		i++;
	}
	argv[n] = NULL;
	return n;
}
