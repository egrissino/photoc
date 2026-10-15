#pragma once

#include <stddef.h>

namespace ui
{

    struct IconSource
    {
        const char* name;
        const char* svg;
    };

    extern const IconSource ICON_SOURCES[];
    extern const size_t ICON_SOURCE_COUNT;

} // namespace ui
