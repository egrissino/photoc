#pragma once

#include "algo/retouch.h"
#include "engine/session.h"
#include "paint/paint.h"
#include "paint/retouch.h"
#include <memory>
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace engine
{

    enum class SampleLayers
    {
        Current,
        CurrentAndBelow,
        All,
    };

    enum class DabKind
    {
        Tone,
        Sponge,
        Focus,
        Smudge,
        BackgroundErase,
        ColorReplace,
        Mixer,
    };

    struct ToneCell
    {
        std::vector<float> cov;
        std::vector<float> orig;
    };

    struct DabTool
    {
        DabKind kind;
        const char* label;
        color::PixelFormat fmt;
        const color::CmykSpace* cmyk;
        float spacing;
        bool burn;
        algo::ToneRange range;
        float exposure;
        bool protect;
        bool saturate;
        bool vibrance;
        bool sharpen;
        float strength;
        float sigma;
        paint::Smudge smudge;
        std::unordered_map<int64_t, ToneCell> cells;
        bool all_layers;
        std::shared_ptr<const doc::Document> pre;
        bool has_visible;
        paint::Region visible;
        int sampling;
        bool contiguous;
        float tolerance;
        bool protect_fg;
        bool has_sample;
        float sample[4];
        float fg[4];
        color::BlendMode replace_mode;
        float wet;
        float load;
        float mix;
        float level;
        float reservoir[4];

        int32_t halo () const;
    };

    /* The stroke's target and the document before it, inside an edit. */
    struct StrokeTarget
    {
        std::shared_ptr<const doc::Document> pre;
        raster::Surface* surf;
        bool lock;
        const raster::Surface* sel;
    };

    bool begin_target (Edit* e, doc::LayerId id, const Value& p, StrokeTarget* t, Error* err);
    paint::Region composite_region (const doc::Document& pre, doc::LayerId id, SampleLayers which, geom::Rect rect,
        color::PixelFormat fmt, const color::CmykSpace* cmyk);
    paint::Region heal_region (
        color::PixelFormat fmt, const paint::Region& src, const paint::Region& dst, const std::vector<bool>& mask);
    std::vector<bool> dilate (size_t w, size_t h, const std::vector<bool>& m, size_t r);
    geom::Rect sampling_window (geom::Rect hole, geom::Rect canvas);
    void clamp_samples (color::PixelFormat fmt, std::vector<float>* data);
    float pnum (const Value& p, const char* key, float fallback);
    bool pflag (const Value& p, const char* key, bool fallback);
    bool make_brush_tool (
        const Session& s, const char* cmd, const Value& p, color::PixelFormat fmt, DabTool* t, Error* err);
    void brush_tool_dab (DabTool* t, paint::Region* work, const paint::Footprint& fp);
    bool remove_stroke (Session& s, const Value& p, const char* cmd, const char* label, Value* result, Error* err);
    bool fill_content_aware (Session& s, Edit* e, doc::LayerId id, float opacity, Error* err);
    bool fill_pattern (
        Session& s, Edit* e, doc::LayerId id, const Value& p, float opacity, color::BlendMode mode, Error* err);
    bool content_aware_region (
        const paint::Region& img, const std::vector<bool>& hole, uint64_t seed, paint::Region* out);
    bool pattern_source (Session& s, const Value& p, const char* cmd, color::PixelFormat fmt, geom::Rect area,
        double ox, double oy, raster::Surface* out, Error* err);

    bool retouch_brush (const Session& s, const Value& p, const char* cmd, paint::BrushSettings* brush,
        std::vector<paint::StrokePoint>* pts, doc::LayerId* id, Error* err);
    bool make_dab_tool (const Session& s, const char* cmd, const Value& p, const paint::BrushSettings& brush,
        color::PixelFormat fmt, DabTool* out, Error* err);
    void dab_tool_effect (paint::Region* work, const paint::Footprint& fp, void* user);
    bool sample_layers_param (const Value& p, const char* cmd, SampleLayers* out, Error* err);
    paint::Region sample_region (const doc::Document& pre, doc::LayerId id, const raster::Surface& surf,
        SampleLayers which, geom::Rect rect, const color::CmykSpace* cmyk);
    bool history_source (
        const Session& s, doc::LayerId id, const Value& p, const char* cmd, raster::Surface* out, Error* err);

    class LiveRetouch
    {
    public:
        bool begin (Session& s, const char* cmd, const Value& p, Error* err);
        geom::Rect push (const paint::StrokePoint* pts, size_t n);
        std::shared_ptr<doc::Document> doc;

    private:
        Session* session_;
        std::unique_ptr<paint::StrokeRenderer> renderer_;
        std::shared_ptr<const doc::Document> pre_;
        raster::Surface pre_surf_;
        raster::Surface history_;
        bool history_kind_;
        doc::LayerId id_;
        bool mask_;
        SampleLayers which_;
        int32_t dx_;
        int32_t dy_;
        color::BlendMode mode_;
        float opacity_;
        bool lock_;
        raster::Surface* surface ();
    };

    class LiveDab
    {
    public:
        bool begin (Session& s, const char* cmd, const Value& p, Error* err);
        geom::Rect push (const paint::StrokePoint* pts, size_t n);
        std::shared_ptr<doc::Document> doc;

    private:
        Session* session_;
        std::unique_ptr<paint::DabGenerator> gen_;
        std::unique_ptr<paint::StrokeRenderer> ctx_;
        std::unique_ptr<DabTool> tool_;
        raster::Surface pre_surf_;
        raster::Surface work_;
        doc::LayerId id_;
        bool mask_;
        float opacity_;
        bool lock_;
        size_t done_;
        raster::Surface* surface ();
    };

} // namespace engine
