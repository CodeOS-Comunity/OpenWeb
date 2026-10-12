#ifndef LITEHTML_KERNEL_COMPAT_H
#define LITEHTML_KERNEL_COMPAT_H

#include <cxx_compat.h>

#ifdef assert
#undef assert
#endif
#ifndef __ASSERT_H
#define assert(expr) ((void)0)
#else
#define assert(expr) ((void)0)
#endif

#ifndef EOF
#define EOF (-1)
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

#ifndef WCHAR_MAX
#define WCHAR_MAX 0x7fffffff
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN 0
#endif

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif

#ifndef FILENAME_MAX
#define FILENAME_MAX 4096
#endif

#ifndef FOPEN_MAX
#define FOPEN_MAX 16
#endif

#endif
