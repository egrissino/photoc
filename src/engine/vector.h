#pragma once

#include "algo/transform.h"
#include "doc/doc.h"
#include "engine/session.h"

namespace engine
{

    Value path_json (const doc::Path& p);
    bool path_from_json (const Value& v, doc::Path* out, const char* cmd, Error* err);
    void render_shape_layer (Session& s, const doc::Document& d, doc::Layer* l);
    void render_vector_mask (const doc::Document& d, doc::Layer* l);
    void render_all_live (Session& s, doc::Document* d);
    void map_path (doc::Path* p, const algo::Homography& h);
    void follow_vectors (Session& s, doc::Document* d, const double m[6]);
    void translate_vectors (doc::Layer* l, int32_t dx, int32_t dy);

} // namespace engine
