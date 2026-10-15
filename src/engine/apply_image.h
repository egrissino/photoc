#pragma once

#include "color/blend.h"
#include "doc/doc.h"
#include "engine/session.h"
#include <stddef.h>
#include <string>
#include <vector>

namespace engine
{

    enum class ChanKind
    {
        Composite,
        Color,
        Alpha,
        Selection,
        Transparency,
        LayerMask,
    };

    struct ChanRef
    {
        ChanKind kind;
        size_t index;
    };

    struct Blending
    {
        int kind;
        color::BlendMode mode;
        float scale;
        float offset;
    };

    size_t color_channel_names (color::ColorMode mode, const char* names[4], const char** composite);
    bool parse_chan_ref (const Value& v, const doc::Document& d, ChanRef* out);
    std::string next_channel_name (const doc::Document& d);
    bool apply_image_to (Session& s, doc::Document* d, doc::LayerId id, const Value& p, Error* err);
    bool calculations_plane (Session& s, const Value& p, std::vector<float>* out, Error* err);

} // namespace engine
