#ifndef CHECKED_ARITH_H
#define CHECKED_ARITH_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

static inline bool checked_mul_size(size_t a, size_t b, size_t *out)
{
	if (a == 0 || b <= SIZE_MAX / a) {
		*out = a * b;
		return true;
	}
	return false;
}

static inline bool checked_add_size(size_t a, size_t b, size_t *out)
{
	if (a <= SIZE_MAX - b) {
		*out = a + b;
		return true;
	}
	return false;
}

#endif  // CHECKED_ARITH_H
