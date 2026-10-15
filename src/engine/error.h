#pragma once

#include <string>

namespace engine
{

    enum class ErrorKind
    {
        None,
        UnknownCommand,
        Disabled,
        BadParams,
        NoDocument,
        NoLayer,
        Other,
        Cancelled,
    };

    struct Error
    {
        ErrorKind kind;
        std::string message;
    };

    bool fail (Error* err, ErrorKind kind, const char* fmt, ...) __attribute__ ((format (printf, 3, 4)));
    bool bad_params (Error* err, const char* cmd, const char* fmt, ...) __attribute__ ((format (printf, 3, 4)));

} // namespace engine
