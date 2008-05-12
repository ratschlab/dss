/*
 * Copyright (C) 2006-2008 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */
extern char *dss_errlist[];
extern char *dss_error_txt;

__printf_2_3 void dss_log(int ll, const char* fmt,...);

/**
 * This bit indicates whether a number is considered a system error number
 * If yes, the system errno is just the result of clearing this bit from
 * the given number.
 */
#define SYSTEM_ERROR_BIT 30

/** Check whether the system error bit is set. */
#define IS_SYSTEM_ERROR(num) (!!((num) & (1 << SYSTEM_ERROR_BIT)))

/** Set the system error bit for the given number. */
#define ERRNO_TO_DSS_ERROR(num) ((num) | (1 << SYSTEM_ERROR_BIT))

/** Check whether a given number is a system error number.
 *
 * \param num The value to be checked.
 * \param _errno The system error number.
 *
 * \return True if \a num is dss' representation of the system
 * error identified by \a _errno.
 */
static inline int is_errno(int num, int _errno)
{
	assert(num > 0 && _errno > 0);
	return ERRNO_TO_DSS_ERROR(_errno) == num;
}

/**
 * dss' version of strerror(3).
 *
 * \param num The error number.
 *
 * \return The error text of \a num.
 */
static inline char *dss_strerror(int num)
{
	assert(num > 0);
	if (IS_SYSTEM_ERROR(num))
		return strerror((num) & ((1 << SYSTEM_ERROR_BIT) - 1));
	else
		return dss_errlist[num];
}

#define DSS_ERRORS \
	DSS_ERROR(SUCCESS, "success") \
	DSS_ERROR(SYNTAX, "syntax error") \
	DSS_ERROR(ATOI_OVERFLOW, "value too large") \
	DSS_ERROR(STRTOLL, "unknown strtoll error") \
	DSS_ERROR(ATOI_NO_DIGITS, "no digits found in string") \
	DSS_ERROR(ATOI_JUNK_AT_END, "further characters after number") \
	DSS_ERROR(INVALID_NUMBER, "invalid number") \
	DSS_ERROR(STRFTIME, "strftime() failed") \
	DSS_ERROR(LOCALTIME, "localtime() failed") \
	DSS_ERROR(NULL_OPEN, "can not open /dev/null") \
	DSS_ERROR(DUP_PIPE, "exec error: can not create pipe") \
	DSS_ERROR(INVOLUNTARY_EXIT, "unexpected termination cause") \
	DSS_ERROR(BAD_EXIT_CODE, "unexpected exit code") \
	DSS_ERROR(SIGNAL_SIG_ERR, "signal() returned SIG_ERR")

/**
 * This is temporarily defined to expand to its first argument (prefixed by
 * 'E_') and gets later redefined to expand to the error text only
 */
#define DSS_ERROR(err, msg) E_ ## err,

enum dss_error_codes {
	DSS_ERRORS
};
#undef DSS_ERROR
#define DSS_ERROR(err, msg) msg,
#define DEFINE_DSS_ERRLIST char *dss_errlist[] = {DSS_ERRORS}
