#pragma once

#include <errno.h>
#include <sys/types.h>

#ifdef _WIN32
typedef int status_t;
#else
typedef int32_t status_t;
#endif

#ifdef _WIN32
#undef NO_ERROR
#endif

enum {
    OK                 = 0, // Everything's swell.
    NO_ERROR           = 0, // No errors.
    UNKNOWN_ERROR      = (-2147483647 - 1), // INT32_MIN value
    NO_MEMORY          = -ENOMEM,
    INVALID_OPERATION  = -ENOSYS,
    BAD_VALUE          = -EINVAL,
    BAD_TYPE           = (UNKNOWN_ERROR + 1),
    NAME_NOT_FOUND     = -ENOENT,
    PERMISSION_DENIED  = -EPERM,
    NO_INIT            = -ENODEV,
    ALREADY_EXISTS     = -EEXIST,
    DEAD_OBJECT        = -EPIPE,
    FAILED_TRANSACTION = (UNKNOWN_ERROR + 2),
#if !defined(_WIN32)
    BAD_INDEX           = -EOVERFLOW,
    NOT_ENOUGH_DATA     = -ENODATA,
    WOULD_BLOCK         = -EWOULDBLOCK,
    TIMED_OUT           = -ETIMEDOUT,
    UNKNOWN_TRANSACTION = -EBADMSG,
#else
    BAD_INDEX           = -E2BIG,
    NOT_ENOUGH_DATA     = (UNKNOWN_ERROR + 3),
    WOULD_BLOCK         = (UNKNOWN_ERROR + 4),
    TIMED_OUT           = (UNKNOWN_ERROR + 5),
    UNKNOWN_TRANSACTION = (UNKNOWN_ERROR + 6),
#endif
    FDS_NOT_ALLOWED = (UNKNOWN_ERROR + 7),
    UNEXPECTED_NULL = (UNKNOWN_ERROR + 8),
};

#ifdef _WIN32
#define NO_ERROR 0L
#endif