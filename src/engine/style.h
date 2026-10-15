#pragma once

#include "doc/doc.h"
#include "engine/session.h"

namespace engine
{

    bool effect_from_params (const Value& p, doc::Effect* e, const char* cmd, Error* err);
    Value effect_json (const doc::Effect& e);
    Value style_json (const doc::LayerStyle& st);

} // namespace engine
