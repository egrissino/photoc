#include "engine/error.h"

#include <stdarg.h>
#include <stdio.h>

namespace engine
{

    /* Fills *err (when non-NULL) and returns false, so callers can write `return fail(err, ...)`. */
    bool fail (Error* err, ErrorKind kind, const char* fmt, ...)
    {
        if (err == NULL)
        {
            return false;
        }
        char buf[1024];
        va_list ap;
        va_start (ap, fmt);
        vsnprintf (buf, sizeof buf, fmt, ap);
        va_end (ap);
        err->kind    = kind;
        err->message = buf;
        return false;
    }

    bool bad_params (Error* err, const char* cmd, const char* fmt, ...)
    {
        if (err == NULL)
        {
            return false;
        }
        char buf[1024];
        va_list ap;
        va_start (ap, fmt);
        vsnprintf (buf, sizeof buf, fmt, ap);
        va_end (ap);
        err->kind    = ErrorKind::BadParams;
        err->message = std::string ("invalid parameters for `") + cmd + "`: " + buf;
        return false;
    }

} // namespace engine
