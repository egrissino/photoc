#pragma once

#include "doc/doc.h"
#include "engine/session.h"

namespace engine
{

    Value char_style_json (const doc::CharStyle& s);
    bool char_style_from (const Value& v, doc::CharStyle* s, const char* cmd, Error* err);
    Value para_style_json (const doc::ParaStyle& p);
    bool para_style_from (const Value& v, doc::ParaStyle* p, const char* cmd, Error* err);
    Value text_json (const doc::TextLayer& t);
    bool text_from_json (const Value& v, doc::TextLayer* t, const char* cmd, Error* err);
    void render_type (Session& s, const doc::Document& d, doc::Layer* l);
    void render_all_type (Session& s, doc::Document* d);
    void follow_document_transform (Session& s, doc::Document* d, const double m[6]);
    std::string type_layer_name (const std::string& text);

} // namespace engine
