#pragma once

#include "doc/doc.h"
#include "engine/session.h"

namespace engine
{

    void render_smart_filtered (
        const doc::Document& d, doc::Layer* l, const color::CmykSpace* cmyk, const float fg[4], const float bg[4]);
    bool set_smart_filter (doc::Document* d, doc::LayerId id, int index, const Value& p, const color::CmykSpace* cmyk,
        const float fg[4], const float bg[4], Error* err);

    bool prepare_filter_params (Session& s, const char* cmd, Value* p, Error* err);
    bool apply_hdr_toning (Session& s, doc::Document* d, const Value& p, doc::LayerId* active, Error* err);
    bool apply_filter (doc::Document* d, doc::LayerId id, const char* cmd, const Value& p, const color::CmykSpace* cmyk,
        const float fg[4], const float bg[4], Error* err);

} // namespace engine
