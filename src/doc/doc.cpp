#include "doc/doc.h"

#include <stdio.h>
#include <string.h>
#include <unordered_set>

namespace doc
{

    using color::PixelFormat;
    using geom::Rect;

    IdSource::IdSource () : next_ (1) {}

    uint64_t IdSource::fresh ()
    {
        return next_.fetch_add (1, std::memory_order_relaxed);
    }

    /* Ids loaded from a file must never collide with ids minted later. */
    void IdSource::ensure_above (uint64_t max)
    {
        uint64_t want = max == UINT64_MAX ? UINT64_MAX : max + 1;
        uint64_t cur  = next_.load (std::memory_order_relaxed);
        while (cur < want && !next_.compare_exchange_weak (cur, want, std::memory_order_relaxed))
        {
        }
    }

    Locks Locks::unite (Locks o) const
    {
        Locks l = {transparency || o.transparency, pixels || o.pixels, position || o.position, artboard || o.artboard,
            all || o.all};
        return l;
    }

    const LabelColor LABEL_COLORS[LABEL_COLOR_COUNT] = {
        LabelColor::None,
        LabelColor::Red,
        LabelColor::Orange,
        LabelColor::Yellow,
        LabelColor::Green,
        LabelColor::Seafoam,
        LabelColor::Blue,
        LabelColor::Indigo,
        LabelColor::Magenta,
        LabelColor::Fuchsia,
        LabelColor::Violet,
        LabelColor::Gray,
    };

    const char* label_color_id (LabelColor c)
    {
        switch (c)
        {
        case LabelColor::None:
            return "none";
        case LabelColor::Red:
            return "red";
        case LabelColor::Orange:
            return "orange";
        case LabelColor::Yellow:
            return "yellow";
        case LabelColor::Green:
            return "green";
        case LabelColor::Seafoam:
            return "seafoam";
        case LabelColor::Blue:
            return "blue";
        case LabelColor::Indigo:
            return "indigo";
        case LabelColor::Magenta:
            return "magenta";
        case LabelColor::Fuchsia:
            return "fuchsia";
        case LabelColor::Violet:
            return "violet";
        case LabelColor::Gray:
            return "gray";
        }
        return "none";
    }

    const char* label_color_label (LabelColor c)
    {
        switch (c)
        {
        case LabelColor::None:
            return "No Color";
        case LabelColor::Red:
            return "Red";
        case LabelColor::Orange:
            return "Orange";
        case LabelColor::Yellow:
            return "Yellow";
        case LabelColor::Green:
            return "Green";
        case LabelColor::Seafoam:
            return "Seafoam";
        case LabelColor::Blue:
            return "Blue";
        case LabelColor::Indigo:
            return "Indigo";
        case LabelColor::Magenta:
            return "Magenta";
        case LabelColor::Fuchsia:
            return "Fuchsia";
        case LabelColor::Violet:
            return "Violet";
        case LabelColor::Gray:
            return "Gray";
        }
        return "No Color";
    }

    bool label_color_from_id (const char* id, LabelColor* out)
    {
        for (size_t i = 0; i < LABEL_COLOR_COUNT; i++)
        {
            if (strcmp (id, label_color_id (LABEL_COLORS[i])) == 0)
            {
                *out = LABEL_COLORS[i];
                return true;
            }
        }
        return false;
    }

    LayerMask LayerMask::reveal_all ()
    {
        float one = 1.0f;
        LayerMask m;
        m.surface = raster::Surface::with_default (PixelFormat::gray8 (), &one);
        m.enabled = true;
        m.linked  = true;
        m.density = 1.0f;
        m.feather = 0.0f;
        return m;
    }

    LayerMask LayerMask::hide_all ()
    {
        float zero  = 0.0f;
        LayerMask m = reveal_all ();
        m.surface   = raster::Surface::with_default (PixelFormat::gray8 (), &zero);
        return m;
    }

    float LayerMask::value (int32_t x, int32_t y) const
    {
        if (!enabled)
        {
            return 1.0f;
        }
        float v = surface.sample_channel (x, y, 0);
        return 1.0f - density * (1.0f - v);
    }

    /* Effective mask values over r (row-major), including density; all ones when disabled. */
    bool LayerMask::values (Rect r, std::vector<float>* out) const
    {
        size_t n;
        if (!raster::region_len (r, 1, &n))
        {
            return false;
        }
        if (!enabled)
        {
            out->assign (n, 1.0f);
            return true;
        }
        if (!surface.read_region (r, out))
        {
            return false;
        }
        size_t ch = surface.channels ();
        if (ch > 1)
        {
            for (size_t i = 0; i < n; i++)
            {
                (*out)[i] = (*out)[i * ch];
            }
            out->resize (n);
        }
        if (density < 1.0f)
        {
            for (size_t i = 0; i < n; i++)
            {
                (*out)[i] = 1.0f - density * (1.0f - (*out)[i]);
            }
        }
        return true;
    }

    /* A vector mask as a pixel mask: its rendered path with its own density and feather. */
    LayerMask vector_mask_view (const VectorMask& v)
    {
        LayerMask m;
        m.surface = v.surface;
        m.enabled = v.enabled;
        m.linked  = v.linked;
        m.density = v.density;
        m.feather = v.feather;
        return m;
    }

    const char* layer_kind_name (LayerKind k)
    {
        switch (k)
        {
        case LayerKind::Raster:
            return "Pixel";
        case LayerKind::Group:
            return "Group";
        case LayerKind::Adjustment:
            return "Adjustment";
        case LayerKind::Fill:
            return "Fill";
        case LayerKind::Text:
            return "Type";
        case LayerKind::Shape:
            return "Shape";
        case LayerKind::Smart:
            return "Smart Object";
        }
        return "Pixel";
    }

    const char* layer_kind_article (LayerKind k)
    {
        return k == LayerKind::Adjustment ? "an" : "a";
    }

    /* A fill layer's contents: a solid colour, a gradient (stops, style, angle, scale, laid out over
       the layer's frame with Align) or a tiled pattern (its own pixels, scale, angle, Link with
       Layer and phase). */
    FillLayer FillLayer::make ()
    {
        FillLayer f;
        f.kind     = FillKind::Solid;
        f.color[0] = f.color[1] = f.color[2] = 0.0f;
        GradientStop a = {0.0f, {0.0f, 0.0f, 0.0f}}, b = {1.0f, {1.0f, 1.0f, 1.0f}};
        f.stops.push_back (a);
        f.stops.push_back (b);
        f.style     = GradientStyle::Linear;
        f.angle     = 90.0f;
        f.scale     = 1.0f;
        f.reverse   = false;
        f.dither    = false;
        f.align     = true;
        f.pattern_w = f.pattern_h = 0;
        f.pattern_scale           = 1.0f;
        f.pattern_angle           = 0.0f;
        f.link                    = true;
        f.phase[0] = f.phase[1] = 0.0f;
        return f;
    }

    /* Blend If: per channel (Gray, then R G B) the levels of this layer and of the layers under it
       that let this one show, each end split in two for a soft ramp (0..255). */
    BlendIf BlendIf::make ()
    {
        BlendIf b;
        BlendRange r = {{0.0f, 0.0f}, {255.0f, 255.0f}, {0.0f, 0.0f}, {255.0f, 255.0f}};
        b.gray       = r;
        for (int i = 0; i < 3; i++)
        {
            b.channels[i] = r;
        }
        return b;
    }

    bool BlendIf::is_default () const
    {
        BlendIf d = make ();
        return memcmp (this, &d, sizeof d) == 0;
    }

    Layer Layer::make (IdSource& ids, const std::string& name, LayerKind kind)
    {
        Layer l;
        l.id      = ids.fresh ();
        l.name    = name;
        l.visible = true;
        memset (&l.locks, 0, sizeof l.locks);
        l.blend         = kind == LayerKind::Group ? color::BlendMode::PassThrough : color::BlendMode::Normal;
        l.opacity       = 1.0f;
        l.fill_opacity  = 1.0f;
        l.clipped       = false;
        l.has_mask      = false;
        l.label         = LabelColor::None;
        l.kind          = kind;
        l.adjustment    = Adjustment::make (AdjustmentKind::Invert);
        l.style.enabled = true;
        l.text          = TextLayer::make ();
        l.shape         = ShapeLayer::make ();
        memset (&l.smart.quad, 0, sizeof l.smart.quad);
        memset (&l.smart.rendered, 0, sizeof l.smart.rendered);
        l.fill                  = FillLayer::make ();
        l.blend_if              = BlendIf::make ();
        l.smart.filters_enabled = true;
        l.smart.warped          = false;
        memset (&l.smart.warp, 0, sizeof l.smart.warp);
        l.has_vector_mask     = false;
        l.vector_mask.path    = Path::make ();
        l.vector_mask.enabled = true;
        l.vector_mask.linked  = true;
        l.vector_mask.density = 1.0f;
        l.vector_mask.feather = 0.0f;
        l.expanded            = true;
        l.has_psd_id          = false;
        l.psd_id              = 0;
        l.link_group          = 0;
        l.excluded_channels   = 0;
        return l;
    }

    Layer Layer::raster (IdSource& ids, const std::string& name, PixelFormat format)
    {
        Layer l  = make (ids, name, LayerKind::Raster);
        l.pixels = raster::Surface (format);
        return l;
    }

    Layer Layer::group (IdSource& ids, const std::string& name, std::vector<Layer> children)
    {
        Layer l    = make (ids, name, LayerKind::Group);
        l.children = children;
        return l;
    }

    /* Whether the layer has the Background's properties. Only the bottom layer of a document with
       them is the Background. */
    bool Layer::has_background_traits () const
    {
        return locks.transparency && locks.position && !clipped && kind == LayerKind::Raster;
    }

    bool Layer::is_group () const
    {
        return kind == LayerKind::Group;
    }

    /* Pixels to composite: the raster itself, or the rendered cache of type, shape and smart object
       layers (read only: tools ask to rasterize them first). */
    const raster::Surface* Layer::surface () const
    {
        if (kind == LayerKind::Raster || kind == LayerKind::Text || kind == LayerKind::Shape
            || kind == LayerKind::Smart)
        {
            return &pixels;
        }
        return NULL;
    }

    raster::Surface* Layer::surface_mut ()
    {
        if (kind == LayerKind::Raster)
        {
            return &pixels;
        }
        return NULL;
    }

    Layer Layer::duplicate (IdSource& ids) const
    {
        Layer l = *this;
        l.reassign_ids (ids);
        return l;
    }

    void Layer::reassign_ids (IdSource& ids)
    {
        id         = ids.fresh ();
        has_psd_id = false;
        psd_id     = 0;
        for (size_t i = 0; i < children.size (); i++)
        {
            children[i].reassign_ids (ids);
        }
    }

    /* An Indexed Color image's palette: RGB entries, and the entry shown transparent (-1: none). */
    size_t ColorTable::count () const
    {
        return rgb.size () / 3;
    }

    size_t ColorTable::nearest (const float c[3]) const
    {
        size_t best = 0;
        float bd    = 1e30f;
        for (size_t i = 0; i < count (); i++)
        {
            float d = 0.0f;
            for (int k = 0; k < 3; k++)
            {
                float e = c[k] - (float) rgb[i * 3 + (size_t) k] / 255.0f;
                d += e * e;
            }
            if (d < bd)
            {
                bd   = d;
                best = i;
            }
        }
        return best;
    }

    void ColorTable::color (size_t i, float out[3]) const
    {
        for (int k = 0; k < 3; k++)
        {
            out[k] = i < count () ? (float) rgb[i * 3 + (size_t) k] / 255.0f : 0.0f;
        }
    }

    /* A Duotone ink: its colour and the curve from the image's density to the ink's. */
    DuotoneInk DuotoneInk::make (const std::string& name, float r, float g, float b)
    {
        DuotoneInk k;
        k.name       = name;
        k.color[0]   = r;
        k.color[1]   = g;
        k.color[2]   = b;
        CurvePoint a = {0.0f, 0.0f}, z = {1.0f, 1.0f};
        k.curve.push_back (a);
        k.curve.push_back (z);
        return k;
    }

    float DuotoneInk::density (float d) const
    {
        d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
        if (curve.empty ())
        {
            return d;
        }
        if (curve.size () == 1)
        {
            return curve[0].output;
        }
        float v = curve.back ().output;
        if (d <= curve[0].input)
        {
            v = curve[0].output;
        }
        else
        {
            for (size_t i = 0; i + 1 < curve.size (); i++)
            {
                if (d <= curve[i + 1].input)
                {
                    float span = curve[i + 1].input - curve[i].input;
                    if (span < 1e-6f)
                    {
                        span = 1e-6f;
                    }
                    v = curve[i].output + (curve[i + 1].output - curve[i].output) * (d - curve[i].input) / span;
                    break;
                }
            }
        }
        return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }

    /* How a gray value prints: each ink laid down at its curve's density, multiplied together. */
    void Duotone::render (float gray, float out[3]) const
    {
        float density = 1.0f - (gray < 0.0f ? 0.0f : gray > 1.0f ? 1.0f : gray);
        out[0] = out[1] = out[2] = 1.0f;
        for (size_t i = 0; i < inks.size (); i++)
        {
            float d = inks[i].density (density);
            for (int c = 0; c < 3; c++)
            {
                out[c] *= 1.0f - d * (1.0f - inks[i].color[c]);
            }
        }
    }

    Document Document::make (
        IdSource& ids, const std::string& name, geom::Size size, color::ColorMode mode, color::SampleType depth)
    {
        Document d;
        d.id             = ids.fresh ();
        d.name           = name;
        d.size           = size;
        d.resolution_dpi = 72.0f;
        d.mode           = mode;
        d.depth          = depth;
        d.has_selection  = false;
        d.has_work_path  = false;
        d.work_path      = Path::make ();
        d.light_angle    = 120.0f;
        d.light_altitude = 30.0f;
        return d;
    }

    const SmartContents* Document::smart_contents (const std::string& uuid) const
    {
        for (size_t i = 0; i < smart.size (); i++)
        {
            if (smart[i].uuid == uuid)
            {
                return &smart[i];
            }
        }
        return NULL;
    }

    SmartContents* Document::smart_contents_mut (const std::string& uuid)
    {
        for (size_t i = 0; i < smart.size (); i++)
        {
            if (smart[i].uuid == uuid)
            {
                return &smart[i];
            }
        }
        return NULL;
    }

    /* New document with a filled, locked "Background" layer (File > New). */
    Document Document::with_background (IdSource& ids, const std::string& name, geom::Size size, color::ColorMode mode,
        color::SampleType depth, color::Color fill, const color::CmykSpace* cmyk)
    {
        Document d            = make (ids, name, size, mode, depth);
        Layer bg              = Layer::raster (ids, "Background", d.pixel_format ());
        bg.locks.transparency = true;
        bg.locks.position     = true;
        float rgb[3];
        fill.to_rgb (cmyk, rgb);
        float rgba[4] = {rgb[0], rgb[1], rgb[2], fill.alpha};
        float px[raster::MAX_CHANNELS];
        raster::from_rgba (bg.pixels.format (), rgba, cmyk, px);
        bg.pixels.fill_rect (d.bounds (), px);
        d.layers.push_back (bg);
        return d;
    }

    PixelFormat Document::pixel_format () const
    {
        color::ColorMode m = mode;
        if (m == color::ColorMode::Indexed || m == color::ColorMode::Multichannel)
        {
            m = color::ColorMode::Rgb;
        }
        if (m == color::ColorMode::Bitmap || m == color::ColorMode::Duotone)
        {
            m = color::ColorMode::Grayscale;
        }
        PixelFormat f = {m, depth, true};
        return f;
    }

    Rect Document::bounds () const
    {
        return Rect::from_size (size);
    }

    const Layer* find_layer (const std::vector<Layer>& layers, LayerId id)
    {
        for (size_t i = 0; i < layers.size (); i++)
        {
            if (layers[i].id == id)
            {
                return &layers[i];
            }
            const Layer* found = find_layer (layers[i].children, id);
            if (found != NULL)
            {
                return found;
            }
        }
        return NULL;
    }

    const Layer* Document::layer (LayerId id) const
    {
        return find_layer (layers, id);
    }

    Layer* Document::layer_mut (LayerId id)
    {
        LayerPath path;
        if (!path_of (id, &path))
        {
            return NULL;
        }
        return layer_at_mut (path);
    }

    bool find_path (const std::vector<Layer>& layers, LayerId id, LayerPath* path)
    {
        for (size_t i = 0; i < layers.size (); i++)
        {
            path->push_back (i);
            if (layers[i].id == id || find_path (layers[i].children, id, path))
            {
                return true;
            }
            path->pop_back ();
        }
        return false;
    }

    bool Document::path_of (LayerId id, LayerPath* out) const
    {
        out->clear ();
        return find_path (layers, id, out);
    }

    const Layer* Document::layer_at (const LayerPath& path) const
    {
        const std::vector<Layer>* level = &layers;
        const Layer* cur                = NULL;
        for (size_t i = 0; i < path.size (); i++)
        {
            if (path[i] >= level->size ())
            {
                return NULL;
            }
            cur   = &(*level)[path[i]];
            level = &cur->children;
        }
        return cur;
    }

    Layer* Document::layer_at_mut (const LayerPath& path)
    {
        std::vector<Layer>* level = &layers;
        Layer* cur                = NULL;
        for (size_t i = 0; i < path.size (); i++)
        {
            if (path[i] >= level->size ())
            {
                return NULL;
            }
            cur   = &(*level)[path[i]];
            level = &cur->children;
        }
        return cur;
    }

    /* The list holding the layer at `path`, and its index in that list. */
    std::vector<Layer>* Document::siblings_mut (const LayerPath& path, size_t* index)
    {
        if (path.empty ())
        {
            return NULL;
        }
        std::vector<Layer>* level = &layers;
        for (size_t i = 0; i + 1 < path.size (); i++)
        {
            if (path[i] >= level->size ())
            {
                return NULL;
            }
            level = &(*level)[path[i]].children;
        }
        if (path.back () >= level->size ())
        {
            return NULL;
        }
        *index = path.back ();
        return level;
    }

    bool Document::remove (LayerId id, Layer* out)
    {
        LayerPath path;
        if (!path_of (id, &path))
        {
            return false;
        }
        size_t idx;
        std::vector<Layer>* sib = siblings_mut (path, &idx);
        if (sib == NULL)
        {
            return false;
        }
        if (out != NULL)
        {
            *out = (*sib)[idx];
        }
        sib->erase (sib->begin () + (ptrdiff_t) idx);
        return true;
    }

    /* Inserts `layer` just above `above` (in its group), or at the top when `above` is NO_LAYER or
       not found. */
    LayerId Document::insert_above (LayerId above, Layer layer)
    {
        LayerId id = layer.id;
        LayerPath path;
        size_t idx;
        std::vector<Layer>* sib = NULL;
        if (above != NO_LAYER && path_of (above, &path))
        {
            sib = siblings_mut (path, &idx);
        }
        if (sib != NULL)
        {
            sib->insert (sib->begin () + (ptrdiff_t) idx + 1, layer);
        }
        else
        {
            layers.push_back (layer);
        }
        return id;
    }

    void walk_layers (const std::vector<Layer>& layers, LayerPath* prefix, std::vector<WalkEntry>* out)
    {
        for (size_t i = 0; i < layers.size (); i++)
        {
            prefix->push_back (i);
            WalkEntry e = {*prefix, prefix->size () - 1, &layers[i]};
            out->push_back (e);
            walk_layers (layers[i].children, prefix, out);
            prefix->pop_back ();
        }
    }

    /* Every layer, depth first, bottom to top within each level. */
    std::vector<WalkEntry> Document::walk () const
    {
        std::vector<WalkEntry> out;
        LayerPath prefix;
        walk_layers (layers, &prefix, &out);
        return out;
    }

    /* "<base> N" with the smallest N no layer uses. */
    std::string Document::next_layer_name (const char* base) const
    {
        std::vector<WalkEntry> all = walk ();
        std::unordered_set<std::string> names;
        for (size_t i = 0; i < all.size (); i++)
        {
            names.insert (all[i].layer->name);
        }
        char buf[256];
        for (size_t n = 1; n <= names.size () + 1; n++)
        {
            snprintf (buf, sizeof buf, "%s %zu", base, n);
            if (names.count (buf) == 0)
            {
                return buf;
            }
        }
        return base;
    }

    /* "<name> copy", then "<name> copy 2", 3, ... An existing " copy" or " copy N" suffix is
       replaced, not stacked. */
    std::string Document::copy_name (const std::string& name) const
    {
        std::string root = name;
        size_t at        = name.rfind (" copy");
        if (at != std::string::npos)
        {
            std::string rest = name.substr (at + 5);
            bool numbered =
                rest.size () > 1 && rest[0] == ' ' && rest.find_first_not_of ("0123456789", 1) == std::string::npos;
            if (rest.empty () || numbered)
            {
                root = name.substr (0, at);
            }
        }
        std::vector<WalkEntry> all = walk ();
        std::unordered_set<std::string> names;
        for (size_t i = 0; i < all.size (); i++)
        {
            names.insert (all[i].layer->name);
        }
        std::string first = root + " copy";
        if (names.count (first) == 0)
        {
            return first;
        }
        for (size_t n = 2; n <= names.size () + 2; n++)
        {
            std::string candidate = root + " copy " + std::to_string (n);
            if (names.count (candidate) == 0)
            {
                return candidate;
            }
        }
        return first;
    }

    /* The bottom layer, when it has the Background's properties. */
    const Layer* Document::background () const
    {
        if (layers.empty () || !layers[0].has_background_traits ())
        {
            return NULL;
        }
        return &layers[0];
    }

    bool Document::is_background (LayerId id) const
    {
        const Layer* b = background ();
        return b != NULL && b->id == id;
    }

    /* A layer's locks with those of every group it sits in. */
    Locks Document::effective_locks (LayerId id) const
    {
        Locks out = {false, false, false, false, false};
        LayerPath path;
        if (!path_of (id, &path))
        {
            return out;
        }
        for (size_t n = 1; n <= path.size (); n++)
        {
            LayerPath prefix (path.begin (), path.begin () + (ptrdiff_t) n);
            const Layer* l = layer_at (prefix);
            if (l != NULL)
            {
                out = out.unite (l->locks);
            }
        }
        return out;
    }

    LayerId Document::top_layer () const
    {
        return layers.empty () ? NO_LAYER : layers.back ().id;
    }

    /* Layer styles: one Effect holds the settings of any kind (the ones a kind doesn't use stay at
       their defaults). Defaults are Photoshop's. */
    const char* const FX_IDS[FX_KIND_COUNT]    = {"dropShadow", "innerShadow", "outerGlow", "innerGlow", "satin",
           "colorOverlay", "gradientOverlay", "stroke", "bevelEmboss", "patternOverlay"};
    const char* const FX_LABELS[FX_KIND_COUNT] = {"Drop Shadow", "Inner Shadow", "Outer Glow", "Inner Glow", "Satin",
        "Color Overlay", "Gradient Overlay", "Stroke", "Bevel & Emboss", "Pattern Overlay"};

    /* The order of Photoshop's Layers panel and Layer Style list: Bevel & Emboss, Stroke, Inner
       Shadow, Inner Glow, Satin, Color, Gradient and Pattern Overlay, Outer Glow, Drop Shadow. */
    int fx_panel_rank (FxKind k)
    {
        const int RANK[FX_KIND_COUNT] = {9, 2, 8, 3, 4, 5, 6, 1, 0, 7};
        return RANK[(size_t) k];
    }

    /* Where a new effect of kind k goes: on top of its kind, in panel order. */
    size_t fx_insert_at (const std::vector<Effect>& items, FxKind k)
    {
        for (size_t i = 0; i < items.size (); i++)
        {
            if (fx_panel_rank (items[i].kind) >= fx_panel_rank (k))
            {
                return i;
            }
        }
        return items.size ();
    }

    const char* fx_kind_id (FxKind k)
    {
        return FX_IDS[(size_t) k];
    }

    const char* fx_kind_label (FxKind k)
    {
        return FX_LABELS[(size_t) k];
    }

    bool fx_kind_from_id (const char* id, FxKind* out)
    {
        for (size_t i = 0; i < FX_KIND_COUNT; i++)
        {
            if (strcmp (FX_IDS[i], id) == 0)
            {
                *out = (FxKind) i;
                return true;
            }
        }
        return false;
    }

    Effect Effect::make (FxKind kind)
    {
        Effect e;
        memset (&e, 0, sizeof e);
        e.kind         = kind;
        e.enabled      = true;
        e.blend        = color::BlendMode::Normal;
        e.opacity      = 1.0f;
        e.angle        = 120.0f;
        e.global_light = true;
        e.range        = 0.5f;
        e.knocks_out   = true;
        e.position     = StrokePosition::Outside;
        e.gradient     = GradientStyle::Linear;
        e.scale        = 1.0f;
        e.bevel        = BevelStyle::InnerBevel;
        e.depth        = 1.0f;
        e.up           = true;
        e.altitude     = 30.0f;
        e.blend2       = color::BlendMode::Multiply;
        e.opacity2     = 0.75f;
        e.color2[0] = e.color2[1] = e.color2[2] = 1.0f;
        switch (kind)
        {
        case FxKind::DropShadow:
            e.blend    = color::BlendMode::Multiply;
            e.opacity  = 0.75f;
            e.distance = 5.0f;
            e.size     = 5.0f;
            break;
        case FxKind::InnerShadow:
            e.blend    = color::BlendMode::Multiply;
            e.opacity  = 0.75f;
            e.distance = 5.0f;
            e.size     = 5.0f;
            break;
        case FxKind::OuterGlow:
        case FxKind::InnerGlow:
            e.blend    = color::BlendMode::Screen;
            e.opacity  = 0.75f;
            e.size     = 5.0f;
            e.color[0] = 1.0f;
            e.color[1] = 1.0f;
            e.color[2] = 190.0f / 255.0f;
            break;
        case FxKind::Satin:
            e.blend        = color::BlendMode::Multiply;
            e.opacity      = 0.5f;
            e.angle        = 19.0f;
            e.global_light = false;
            e.distance     = 11.0f;
            e.size         = 14.0f;
            e.invert       = true;
            break;
        case FxKind::ColorOverlay:
            e.color[0] = 1.0f;
            break;
        case FxKind::GradientOverlay:
            e.angle        = 90.0f;
            e.global_light = false;
            break;
        case FxKind::Stroke:
            e.size     = 3.0f;
            e.color[0] = 1.0f;
            break;
        case FxKind::BevelEmboss:
            e.blend    = color::BlendMode::Screen;
            e.size     = 5.0f;
            e.color[0] = e.color[1] = e.color[2] = 1.0f;
            e.color2[0] = e.color2[1] = e.color2[2] = 0.0f;
            break;
        case FxKind::PatternOverlay:
            e.link = true;
            break;
        }
        return e;
    }

    /* The document's pattern called `name` (Pattern Overlay draws from these), or NULL. */
    const DocPattern* Document::pattern (const std::string& n) const
    {
        for (size_t i = 0; i < patterns.size (); i++)
        {
            if (patterns[i].name == n)
            {
                return &patterns[i];
            }
        }
        return NULL;
    }

    bool same_rgb (const float a[3], const float b[3])
    {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    }

    bool same_effect (const Effect& a, const Effect& b)
    {
        return a.kind == b.kind && a.enabled == b.enabled && a.blend == b.blend && a.opacity == b.opacity
            && same_rgb (a.color, b.color) && a.angle == b.angle && a.global_light == b.global_light
            && a.distance == b.distance && a.spread == b.spread && a.size == b.size && a.range == b.range
            && a.precise == b.precise && a.center == b.center && a.knocks_out == b.knocks_out && a.invert == b.invert
            && a.position == b.position && same_rgb (a.color2, b.color2) && a.gradient == b.gradient
            && a.reverse == b.reverse && a.scale == b.scale && a.bevel == b.bevel && a.depth == b.depth && a.up == b.up
            && a.soften == b.soften && a.altitude == b.altitude && a.blend2 == b.blend2 && a.opacity2 == b.opacity2
            && strcmp (a.pattern, b.pattern) == 0 && a.link == b.link && a.phase[0] == b.phase[0]
            && a.phase[1] == b.phase[1];
    }

    bool same_style (const LayerStyle& a, const LayerStyle& b)
    {
        if (a.enabled != b.enabled || a.items.size () != b.items.size ())
        {
            return false;
        }
        for (size_t i = 0; i < a.items.size (); i++)
        {
            if (!same_effect (a.items[i], b.items[i]))
            {
                return false;
            }
        }
        return true;
    }

} // namespace doc
