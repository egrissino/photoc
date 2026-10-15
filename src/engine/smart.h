#pragma once

#include "algo/transform.h"
#include "doc/doc.h"
#include "engine/session.h"

namespace engine
{

    void render_smart (Session& s, const doc::Document& d, doc::Layer* l);
    void render_smart_changed (Session& s, doc::Document* d, bool all);
    void map_quad (double quad[4][2], const algo::Homography& h);
    void map_smart (doc::SmartLayer* s, const algo::Homography& h);

} // namespace engine
