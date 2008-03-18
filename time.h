int tv_diff(const struct timeval *b, const struct timeval *a, struct timeval *diff);
long unsigned tv2ms(const struct timeval*);
void d2tv(double, struct timeval*);
void tv_add(const struct timeval*, const struct timeval *, struct timeval *);
void tv_scale(const unsigned long, const struct timeval *, struct timeval *);
void tv_divide(const unsigned long divisor, const struct timeval *tv,
	struct timeval *result);
void ms2tv(const long unsigned n, struct timeval *tv);
