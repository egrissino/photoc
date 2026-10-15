#pragma once

#include "color/blend.h"
#include "color/color.h"
#include "color/convert.h"
#include "doc/adjust.h"
#include "doc/text.h"
#include "doc/vector.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace doc
{

    typedef uint64_t LayerId;
    typedef uint64_t DocId;
    typedef uint64_t SmartContentsId;

    const LayerId NO_LAYER       = 0;
    const size_t MAX_GROUP_DEPTH = 100;

    typedef std::vector<size_t> LayerPath;

    class IdSource
    {
    public:
        IdSource ();
        uint64_t fresh ();
        void ensure_above (uint64_t max);

    private:
        std::atomic<uint64_t> next_;
    };

    struct Locks
    {
        bool transparency;
        bool pixels;
        bool position;
        bool artboard;
        bool all;

        Locks unite (Locks o) const;
    };

    enum class LabelColor : uint8_t
    {
        None,
        Red,
        Orange,
        Yellow,
        Green,
        Blue,
        Violet,
        Gray,
        Seafoam,
        Indigo,
        Magenta,
        Fuchsia,
    };

    const size_t LABEL_COLOR_COUNT = 12;
    extern const LabelColor LABEL_COLORS[LABEL_COLOR_COUNT];

    const char* label_color_id (LabelColor c);
    const char* label_color_label (LabelColor c);
    bool label_color_from_id (const char* id, LabelColor* out);

    struct LayerMask
    {
        raster::Surface surface;
        bool enabled;
        bool linked;
        float density;
        float feather;

        static LayerMask reveal_all ();
        static LayerMask hide_all ();
        float value (int32_t x, int32_t y) const;
        bool values (geom::Rect r, std::vector<float>* out) const;
    };

    LayerMask vector_mask_view (const VectorMask& v);

    enum class LayerKind : uint8_t
    {
        Raster,
        Group,
        Adjustment,
        Fill,
        Text,
        Shape,
        Smart,
    };

    const char* layer_kind_name (LayerKind k);
    const char* layer_kind_article (LayerKind k);

    enum class FxKind : uint8_t
    {
        DropShadow,
        InnerShadow,
        OuterGlow,
        InnerGlow,
        Satin,
        ColorOverlay,
        GradientOverlay,
        Stroke,
        BevelEmboss,
        PatternOverlay,
    };

    const size_t FX_KIND_COUNT = 10;

    enum class StrokePosition : uint8_t
    {
        Outside,
        Inside,
        Center,
    };

    enum class BevelStyle : uint8_t
    {
        OuterBevel,
        InnerBevel,
        Emboss,
        PillowEmboss,
    };

    enum class GradientStyle : uint8_t
    {
        Linear,
        Radial,
        Angle,
        Reflected,
        Diamond,
    };

    struct Effect
    {
        FxKind kind;
        bool enabled;
        color::BlendMode blend;
        float opacity;
        float color[3];
        float angle;
        bool global_light;
        float distance;
        float spread;
        float size;
        float range;
        bool precise;
        bool center;
        bool knocks_out;
        bool invert;
        StrokePosition position;
        float color2[3];
        GradientStyle gradient;
        bool reverse;
        float scale;
        BevelStyle bevel;
        float depth;
        bool up;
        float soften;
        float altitude;
        color::BlendMode blend2;
        float opacity2;
        char pattern[64];
        bool link;
        float phase[2];

        static Effect make (FxKind kind);
    };

    int fx_panel_rank (FxKind k);
    size_t fx_insert_at (const std::vector<Effect>& items, FxKind k);
    const char* fx_kind_id (FxKind k);
    const char* fx_kind_label (FxKind k);
    bool fx_kind_from_id (const char* id, FxKind* out);

    enum class FillKind : uint8_t
    {
        Solid,
        Gradient,
        Pattern,
    };

    struct FillLayer
    {
        FillKind kind;
        float color[3];
        std::vector<GradientStop> stops;
        std::vector<CurvePoint> opacity_stops;
        GradientStyle style;
        float angle;
        float scale;
        bool reverse;
        bool dither;
        bool align;
        std::string pattern_name;
        uint32_t pattern_w;
        uint32_t pattern_h;
        std::vector<float> pattern_rgba;
        float pattern_scale;
        float pattern_angle;
        bool link;
        float phase[2];

        static FillLayer make ();
    };

    struct BlendRange
    {
        float this_black[2];
        float this_white[2];
        float under_black[2];
        float under_white[2];
    };

    struct BlendIf
    {
        BlendRange gray;
        BlendRange channels[3];

        static BlendIf make ();
        bool is_default () const;
    };

    struct DocPattern
    {
        std::string name;
        uint32_t width;
        uint32_t height;
        std::vector<float> rgba;
    };

    struct LayerStyle
    {
        bool enabled;
        std::vector<Effect> items;
    };

    bool same_effect (const Effect& a, const Effect& b);
    bool same_style (const LayerStyle& a, const LayerStyle& b);

    struct Document;

    /* A smart object's embedded contents: the original file (kept to write back unchanged), the
       document it opens as, and its size in pixels. */
    struct SmartContents
    {
        std::string uuid;
        std::string filename;
        char filetype[4];
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        std::shared_ptr<const Document> doc;
        uint32_t width;
        uint32_t height;
    };

    /* A smart object layer: its contents and where their corners land (top left, top right, bottom
       right, bottom left, document pixels). Its pixels are the contents rendered there. */
    /* A smart filter: the filter command and its settings (JSON), how it blends with what is under
       it in the stack. An empty id is a Photoshop filter this build can't run (kept, skipped). */
    struct SmartFilter
    {
        std::string id;
        std::string name;
        std::string params;
        bool enabled;
        color::BlendMode blend;
        float opacity;
    };

    struct SmartLayer
    {
        std::string uuid;
        double quad[4][2];
        double rendered[4][2];
        std::vector<SmartFilter> filters;
        bool filters_enabled;
        bool warped;
        double warp[16][2];
    };

    struct PsdBlock
    {
        char key[4];
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    struct Layer
    {
        LayerId id;
        std::string name;
        bool visible;
        Locks locks;
        color::BlendMode blend;
        float opacity;
        float fill_opacity;
        bool clipped;
        bool has_mask;
        LayerMask mask;
        LabelColor label;
        LayerKind kind;

        raster::Surface pixels;

        Adjustment adjustment;
        LayerStyle style;
        TextLayer text;
        ShapeLayer shape;
        SmartLayer smart;
        FillLayer fill;
        BlendIf blend_if;
        bool has_vector_mask;
        VectorMask vector_mask;

        std::vector<Layer> children;
        bool expanded;

        std::vector<PsdBlock> psd_blocks;
        bool has_psd_id;
        uint32_t psd_id;
        uint64_t link_group;
        uint32_t excluded_channels;

        static Layer make (IdSource& ids, const std::string& name, LayerKind kind);
        static Layer raster (IdSource& ids, const std::string& name, color::PixelFormat format);
        static Layer group (IdSource& ids, const std::string& name, std::vector<Layer> children);

        bool has_background_traits () const;
        bool is_group () const;
        const raster::Surface* surface () const;
        raster::Surface* surface_mut ();
        Layer duplicate (IdSource& ids) const;
        void reassign_ids (IdSource& ids);
    };

    struct WalkEntry
    {
        LayerPath path;
        size_t depth;
        const Layer* layer;
    };

    struct AlphaChannel
    {
        std::string name;
        raster::Surface surface;
    };

    struct Guide
    {
        bool vertical;
        double position;
    };

    struct ColorTable
    {
        std::vector<uint8_t> rgb;
        int transparent = -1;

        size_t count () const;
        size_t nearest (const float c[3]) const;
        void color (size_t i, float out[3]) const;
    };

    struct DuotoneInk
    {
        std::string name;
        float color[3];
        std::vector<CurvePoint> curve;

        static DuotoneInk make (const std::string& name, float r, float g, float b);
        float density (float gray_density) const;
    };

    struct Duotone
    {
        std::vector<DuotoneInk> inks;
        std::vector<uint8_t> psd_raw;

        void render (float gray, float out[3]) const;
    };

    /* A layer comp: per layer its visibility, position (content's top left) and appearance (blend,
       opacity, fill, style), and which of the three applying it restores. */
    struct CompLayerState
    {
        LayerId layer;
        bool has_visible;
        bool visible;
        bool has_position;
        int32_t x;
        int32_t y;
        bool has_appearance;
        color::BlendMode blend;
        float opacity;
        float fill;
        LayerStyle style;
    };

    struct LayerComp
    {
        uint32_t id;
        std::string name;
        std::string comment;
        bool apply_visibility;
        bool apply_position;
        bool apply_appearance;
        std::vector<CompLayerState> states;
    };

    struct Document
    {
        DocId id;
        std::string name;
        geom::Size size;
        float resolution_dpi;
        color::ColorMode mode;
        color::SampleType depth;
        std::shared_ptr<const std::vector<uint8_t>> icc_profile;
        std::vector<Layer> layers;
        bool has_selection;
        raster::Surface selection;
        std::vector<AlphaChannel> channels;
        std::vector<Guide> guides;
        std::vector<NamedPath> paths;
        bool has_work_path;
        Path work_path;
        std::vector<SmartContents> smart;
        float light_angle;
        float light_altitude;
        bool has_color_table = false;
        ColorTable color_table;
        bool has_duotone = false;
        Duotone duotone;
        std::vector<DocPattern> patterns;
        std::vector<LayerComp> layer_comps;
        uint32_t last_applied_comp = 0;
        bool has_last_state        = false;
        LayerComp last_document_state;

        const DocPattern* pattern (const std::string& name) const;

        static Document make (
            IdSource& ids, const std::string& name, geom::Size size, color::ColorMode mode, color::SampleType depth);
        static Document with_background (IdSource& ids, const std::string& name, geom::Size size, color::ColorMode mode,
            color::SampleType depth, color::Color fill, const color::CmykSpace* cmyk);

        color::PixelFormat pixel_format () const;
        geom::Rect bounds () const;

        const Layer* layer (LayerId id) const;
        Layer* layer_mut (LayerId id);
        bool path_of (LayerId id, LayerPath* out) const;
        const Layer* layer_at (const LayerPath& path) const;
        Layer* layer_at_mut (const LayerPath& path);
        std::vector<Layer>* siblings_mut (const LayerPath& path, size_t* index);
        bool remove (LayerId id, Layer* out);
        LayerId insert_above (LayerId above, Layer layer);
        std::vector<WalkEntry> walk () const;
        std::string next_layer_name (const char* base) const;
        std::string copy_name (const std::string& name) const;
        const Layer* background () const;
        bool is_background (LayerId id) const;
        Locks effective_locks (LayerId id) const;
        LayerId top_layer () const;
        const SmartContents* smart_contents (const std::string& uuid) const;
        SmartContents* smart_contents_mut (const std::string& uuid);
    };

} // namespace doc
