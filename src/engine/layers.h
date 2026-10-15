#pragma once

#include "doc/doc.h"
#include "engine/session.h"
#include "raster/raster.h"
#include <string>
#include <utility>
#include <vector>

namespace engine
{

    struct ClipBefore
    {
        std::vector<doc::LayerId> moving;
        std::vector<doc::LayerId> clipped;
        std::vector<std::pair<doc::LayerId, std::vector<doc::LayerId>>> groups;
        std::vector<doc::LayerId> left;
    };

    struct FillBlend
    {
        color::BlendMode mode;
        float opacity;
        bool keep_alpha;
        float color[4];
    };

    bool layer_param (const Session& s, const Value& p, doc::LayerId* out, Error* err);
    bool no_layer (Error* err, doc::LayerId id);
    bool multi_target (const Session& s, const Value& p);
    bool layer_targets (const Session& s, const Value& p, std::vector<doc::LayerId>* out, Error* err);
    std::vector<doc::LayerId> top_level (const doc::Document& d, const std::vector<doc::LayerId>& ids);
    bool set_layer_selection (
        Session& s, const std::vector<doc::LayerId>& ids, doc::LayerId active, doc::LayerId anchor, Error* err);
    doc::Layer layer_copy (doc::IdSource& ids, const doc::Document& d, const doc::Layer& src);
    std::vector<doc::LayerId> deletion_neighbours (const doc::Document& d, doc::LayerId target);
    raster::Surface composite_layers (
        const doc::Document& solo, color::PixelFormat format, const color::CmykSpace* cmyk);
    ClipBefore clip_before (const doc::Document& d, const std::vector<doc::LayerId>& moving);
    void clip_settle (doc::Document* d, const std::vector<doc::LayerId>& placed, const ClipBefore& before, bool force);
    raster::Surface lift (const raster::Surface& src, const raster::Surface* sel, geom::Rect canvas);
    void background_to_layer (doc::Document* d, doc::LayerId id);
    void apply_selection (doc::Document* d, const raster::Surface& shape, const std::string& mode);
    bool check_pixels_unlocked (const doc::Document& d, doc::LayerId id, Error* err);
    bool clear_selected (
        doc::Document* d, doc::LayerId id, const float background[4], const color::CmykSpace* cmyk, Error* err);
    void color_param (const Value& p, const char* key, const float fallback[4], float out[4]);
    void fill_surface (raster::Surface* surf, geom::Rect area, const raster::Surface* sel, const FillBlend& b,
        const color::CmykSpace* cmyk);
    raster::Surface* pixel_target (doc::Document* d, doc::LayerId id, Error* err);

} // namespace engine
