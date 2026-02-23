#ifndef STDIO_HELPERS_H
#define STDIO_HELPERS_H

#include <stdio.h>
#include <stdarg.h>

#define GETC() fgetc_unlocked(stdin)
#define UNGETC(c) ungetc(c, stdin)

#define READ_FILE(buf, len, stream) fread_unlocked(buf, 1, len, stream)
#define WRITE_FILE(buf, len, stream) fwrite_unlocked(buf, 1, len, stream)
#define GETC_FILE(stream) fgetc_unlocked(stream)
#define FLUSH_FILE(stream) fflush_unlocked(stream)

#define WRITE(s, len) fwrite_unlocked(s, 1, len, stdout)
#define PUTS(s) fputs_unlocked(s, stdout)
#define PUTC(c) fputc_unlocked(c, stdout)
#define FLUSH() fflush_unlocked(stdout)

#if defined(__GNUC__) || defined(__clang__)
#define STDIO_HELPERS_UNUSED __attribute__((unused))
#else
#define STDIO_HELPERS_UNUSED
#endif

static char stdio_printf_buf[256] STDIO_HELPERS_UNUSED;

#define PRINTF(format, ...) \
	do { \
		int n = snprintf(stdio_printf_buf, sizeof(stdio_printf_buf), format, __VA_ARGS__); \
		size_t len = 0; \
		if (n > 0) { \
			len = (size_t)n; \
			if (len >= sizeof(stdio_printf_buf)) len = sizeof(stdio_printf_buf) - 1; \
			fwrite_unlocked(stdio_printf_buf, 1, len, stdout); \
		} \
	} while (0)

#define WRITE_ERR(s, len) fwrite_unlocked(s, 1, len, stderr)
#define PUTS_ERR(s) fputs_unlocked(s, stderr)
#define PUTC_ERR(c) fputc_unlocked(c, stderr)

#define PRINTF_ERR(format, ...) \
	do { \
		int n = snprintf(stdio_printf_buf, sizeof(stdio_printf_buf), format, __VA_ARGS__); \
		size_t len = 0; \
		if (n > 0) { \
			len = (size_t)n; \
			if (len >= sizeof(stdio_printf_buf)) len = sizeof(stdio_printf_buf) - 1; \
			fwrite_unlocked(stdio_printf_buf, 1, len, stderr); \
		} \
	} while (0)

#endif  // STDIO_HELPERS_H
