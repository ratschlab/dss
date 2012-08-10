/*
 * Copyright (C) 2005-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

/** \file time.c Helper functions for dealing with time values. */

#include <sys/time.h>
#include <time.h>
#include <inttypes.h>
#include <assert.h>
#include <string.h>

#include "gcc-compat.h"
#include "error.h"
#include "string.h"
#include "log.h"
#include "time.h"

/**
 * Convert struct timeval to milliseconds.
 *
 * \param tv The time value value to convert.
 *
 * \return The number off milliseconds in \a tv.
 */
long unsigned tv2ms(const struct timeval *tv)
{
	return tv->tv_sec * 1000 + (tv->tv_usec + 500)/ 1000;
}

/**
 * Convert milliseconds to a struct timeval.
 *
 * \param n The number of milliseconds.
 * \param tv Result pointer.
 */
void ms2tv(long unsigned n, struct timeval *tv)
{
	tv->tv_sec = n / 1000;
	tv->tv_usec = (n % 1000) * 1000;
}

/**
 * Convert a double to a struct timeval.
 *
 * \param x The value to convert.
 * \param tv Result pointer.
 */
void d2tv(double x, struct timeval *tv)
{
	tv->tv_sec = x;
	tv->tv_usec = (x - (double)tv->tv_sec) * 1000.0 * 1000.0 + 0.5;
}

/**
 * Compute the difference of two time values.
 *
 * \param b Minuend.
 * \param a Subtrahend.
 * \param diff Result pointer.
 *
 * If \a diff is not \p NULL, it contains the absolute value |\a b - \a a| on
 * return.
 *
 * \return If \a b < \a a, this function returns -1, otherwise it returns 1.
 */
int tv_diff(const struct timeval *b, const struct timeval *a, struct timeval *diff)
{
	int ret = 1;

	if ((b->tv_sec < a->tv_sec) ||
		((b->tv_sec == a->tv_sec) && (b->tv_usec < a->tv_usec))) {
		const struct timeval *tmp = a;
		a = b;
		b = tmp;
		ret = -1;
	}
	if (!diff)
		return ret;
	diff->tv_sec = b->tv_sec - a->tv_sec;
	if (b->tv_usec < a->tv_usec) {
		diff->tv_sec--;
		diff->tv_usec = 1000 * 1000 - a->tv_usec + b->tv_usec;
	} else
		diff->tv_usec = b->tv_usec - a->tv_usec;
	return ret;
}

/**
 * Add two time values.
 *
 * \param a First addend.
 * \param b Second addend.
 * \param sum Contains the sum \a + \a b on return.
 */
void tv_add(const struct timeval *a, const struct timeval *b,
	struct timeval *sum)
{
	sum->tv_sec = a->tv_sec + b->tv_sec;
	if (a->tv_usec + b->tv_usec >= 1000 * 1000) {
		sum->tv_sec++;
		sum->tv_usec = a->tv_usec + b->tv_usec - 1000 * 1000;
	} else
		sum->tv_usec = a->tv_usec + b->tv_usec;
}

/**
 * Compute integer multiple of given struct timeval.
 *
 * \param mult The integer value to multiply with.
 * \param tv The timevalue to multiply.
 *
 * \param result Contains \a mult * \a tv on return.
 */
void tv_scale(const unsigned long mult, const struct timeval *tv,
	struct timeval *result)
{
	result->tv_sec = mult * tv->tv_sec;
	result->tv_sec += tv->tv_usec * mult / 1000 / 1000;
	result->tv_usec = tv->tv_usec * mult % (1000 * 1000);
}

/**
 * Compute a fraction of given struct timeval.
 *
 * \param divisor The integer value to divide by.
 * \param tv The timevalue to divide.
 * \param result Contains (1 / mult) * tv on return.
 */
void tv_divide(const unsigned long divisor, const struct timeval *tv,
	struct timeval *result)
{
	uint64_t x = ((uint64_t)tv->tv_sec * 1000 * 1000 + tv->tv_usec) / divisor;

	result->tv_sec = x / 1000 / 1000;
	result->tv_usec = x % (1000 * 1000);
}

int64_t get_current_time(void)
{
	time_t now;
	time(&now);
	DSS_DEBUG_LOG(("now: %jd\n", (intmax_t)now));
	return (int64_t)now;
}
