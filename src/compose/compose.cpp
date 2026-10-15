#include "compose/compose.h"

#include "compose/adjust.h"
#include "compose/effects.h"
#include "compose/masks.h"
#include <math.h>
#include <string.h>

namespace compose
{

    /* CPU reference compositor: flattens a layer tree into straight-alpha RGBA floats for any
       rectangle. Ported so far: raster and group layers, blend modes, opacity x fill (with the
       special-eight fill rule), Dissolve, layer masks with density and feather, clipping groups, pass-through
       and isolated groups, adjustment layers. Fill, text, shape and smart-object layers, layer
       effects, knockouts, Blend If and channel restrictions arrive with their doc types. */

    using color::BlendMode;
    using geom::Rect;

    struct Ctx
    {
        const doc::Document* doc;
        Rect canvas;
        color::ColorMode mode;
        color::SampleType depth;
        BlendSpace space;
        const color::CmykSpace* cmyk;
        float text_gamma;
        Transfer transfer;
        float light_angle;
        float light_altitude;
        std::vector<std::pair<doc::LayerId, raster::Surface>> feathered;
        std::vector<std::pair<doc::LayerId, raster::Surface>> vfeathered;
    };

    AdjustContext adjust_context (const Ctx& cx)
    {
        AdjustContext a = {cx.transfer, true, cx.depth, cx.mode, cx.cmyk};
        return a;
    }

    void composite_stack_at (
        const std::vector<doc::Layer>& layers, size_t begin, size_t end, Buffer* backdrop, const Ctx& cx);

    Buffer Buffer::transparent (Rect rect)
    {
        Buffer b;
        b.rect = rect;
        b.px.assign ((size_t) rect.width () * (size_t) rect.height () * 4, 0.0f);
        return b;
    }

    Buffer Buffer::filled (Rect rect, const float c[4])
    {
        Buffer b = transparent (rect);
        for (size_t i = 0; i < b.pixel_count (); i++)
        {
            memcpy (&b.px[i * 4], c, sizeof (float) * 4);
        }
        return b;
    }

    size_t Buffer::pixel_count () const
    {
        return px.size () / 4;
    }

    float* Buffer::at (int32_t x, int32_t y)
    {
        return &px[(((size_t) (y - rect.y0)) * rect.width () + (size_t) (x - rect.x0)) * 4];
    }

    const float* Buffer::at (int32_t x, int32_t y) const
    {
        return &px[(((size_t) (y - rect.y0)) * rect.width () + (size_t) (x - rect.x0)) * 4];
    }

    raster::Rgba8Image Buffer::to_rgba8 () const
    {
        raster::Rgba8Image img = raster::rgba8_image_new (rect.width (), rect.height ());
        for (size_t i = 0; i < px.size (); i++)
        {
            color::write_sample (img.pixels.data (), color::SampleType::U8, i, px[i]);
        }
        return img;
    }

    /* Flatten over an opaque background colour. */
    Buffer Buffer::over_background (const float bg[3]) const
    {
        Buffer out = *this;
        for (size_t i = 0; i < out.pixel_count (); i++)
        {
            float* p = &out.px[i * 4];
            float a  = p[3];
            for (int c = 0; c < 3; c++)
            {
                p[c] = p[c] * a + bg[c] * (1.0f - a);
            }
            p[3] = 1.0f;
        }
        return out;
    }

    RenderSettings default_settings ()
    {
        RenderSettings s = {NULL, TEXT_GAMMA, NULL, false};
        return s;
    }

    Buffer surface_to_buffer (const raster::Surface& s, Rect rect, const color::CmykSpace* cmyk)
    {
        Buffer b = Buffer::transparent (rect);
        s.read_rgba (rect, cmyk, b.px.data ());
        return b;
    }

    /* Deterministic hash for Dissolve, in document coordinates so it is stable under tiling. */
    float dissolve_noise (int32_t x, int32_t y)
    {
        uint32_t h = ((uint32_t) x * 0x8da6b343u) ^ ((uint32_t) y * 0xd8163841u) ^ 0x9e3779b9u;
        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        return (float) (h & 0xffff) / 65536.0f;
    }

    float layer_text_gamma (const doc::Layer& layer, const Ctx& cx)
    {
        return layer.kind == doc::LayerKind::Text ? cx.text_gamma : 1.0f;
    }

    /* Bounds of a layer's own pixels (union over visible group children; the canvas for fills). */
    Rect layer_bounds (const doc::Layer& layer, Rect canvas)
    {
        switch (layer.kind)
        {
        case doc::LayerKind::Group:
            {
                Rect acc;
                for (size_t i = 0; i < layer.children.size (); i++)
                {
                    if (layer.children[i].visible)
                    {
                        acc = acc.unite (layer_bounds (layer.children[i], canvas));
                    }
                }
                return acc;
            }
        case doc::LayerKind::Fill:
            return canvas;
        default:
            {
                const raster::Surface* s = layer.surface ();
                return s == NULL ? Rect () : s->content_bounds ();
            }
        }
    }

    /* True when the layer cannot contribute anything inside rect. */
    bool empty_in (const doc::Layer& layer, Rect rect)
    {
        if (layer.kind == doc::LayerKind::Group || layer.kind == doc::LayerKind::Adjustment
            || layer.kind == doc::LayerKind::Fill)
        {
            return false;
        }
        const raster::Surface* s = layer.surface ();
        if (s == NULL)
        {
            return true;
        }
        if (s->has_tiles_in (rect) || !s->format ().alpha)
        {
            return false;
        }
        float dp[raster::MAX_CHANNELS];
        s->default_pixel (dp);
        return dp[s->channels () - 1] <= 0.0f;
    }

    /* The layer's mask values over rect (feathered masks from the render's precomputed blur): the
       pixel mask times the vector mask, or false when it has neither. */
    bool mask_vals (const doc::Layer& layer, Rect rect, const Ctx& cx, std::vector<float>* out)
    {
        bool any = false;
        if (layer.has_mask)
        {
            bool found = false;
            for (size_t i = 0; i < cx.feathered.size () && !found; i++)
            {
                if (cx.feathered[i].first == layer.id)
                {
                    found = cx.feathered[i].second.read_region (rect, out);
                }
            }
            if (!found && !layer.mask.values (rect, out))
            {
                return false;
            }
            any = true;
        }
        if (layer.has_vector_mask)
        {
            std::vector<float> v;
            bool found = false;
            for (size_t i = 0; i < cx.vfeathered.size () && !found; i++)
            {
                if (cx.vfeathered[i].first == layer.id)
                {
                    found = cx.vfeathered[i].second.read_region (rect, &v);
                }
            }
            if (!found && !doc::vector_mask_view (layer.vector_mask).values (rect, &v))
            {
                return any;
            }
            if (!any)
            {
                *out = v;
            }
            else
            {
                for (size_t i = 0; i < out->size () && i < v.size (); i++)
                {
                    (*out)[i] *= v[i];
                }
            }
            any = true;
        }
        return any;
    }

    void composite_stack (const std::vector<doc::Layer>& layers, Buffer* backdrop, const Ctx& cx)
    {
        composite_stack_at (layers, 0, layers.size (), backdrop, cx);
    }

    /* A fill layer's frame: its mask's extent when the mask hides everything else (a fill made
       from a selection), else the canvas. Aligned gradients and linked patterns follow it. */
    Rect fill_frame (const doc::Layer& layer, Rect canvas)
    {
        if (layer.has_mask)
        {
            float dp[raster::MAX_CHANNELS];
            layer.mask.surface.default_pixel (dp);
            Rect b = layer.mask.surface.content_bounds ().intersect (canvas);
            if (dp[0] <= 0.0f && !b.is_empty ())
            {
                return b;
            }
        }
        return canvas;
    }

    /* A gradient's colour at t: colour stops, times the opacity stops (if any). */
    void fill_gradient_color (const doc::FillLayer& f, float t, float out[4])
    {
        const std::vector<doc::GradientStop>& st = f.stops;
        out[3]                                   = 1.0f;
        if (st.empty ())
        {
            out[0] = out[1] = out[2] = t;
        }
        else if (t <= st[0].location)
        {
            memcpy (out, st[0].color, 3 * sizeof (float));
        }
        else if (t >= st.back ().location)
        {
            memcpy (out, st.back ().color, 3 * sizeof (float));
        }
        else
        {
            for (size_t i = 0; i + 1 < st.size (); i++)
            {
                if (t <= st[i + 1].location)
                {
                    float span = fmaxf (st[i + 1].location - st[i].location, 1e-6f), k = (t - st[i].location) / span;
                    for (int c = 0; c < 3; c++)
                    {
                        out[c] = st[i].color[c] + (st[i + 1].color[c] - st[i].color[c]) * k;
                    }
                    break;
                }
            }
        }
        const std::vector<doc::CurvePoint>& os = f.opacity_stops;
        if (os.empty ())
        {
            return;
        }
        float a = os.back ().output;
        if (t <= os[0].input)
        {
            a = os[0].output;
        }
        else
        {
            for (size_t i = 0; i + 1 < os.size (); i++)
            {
                if (t <= os[i + 1].input)
                {
                    float span = fmaxf (os[i + 1].input - os[i].input, 1e-6f);
                    a          = os[i].output + (os[i + 1].output - os[i].output) * (t - os[i].input) / span;
                    break;
                }
            }
        }
        out[3] = fminf (fmaxf (a, 0.0f), 1.0f);
    }

    /* A fill layer over rect: a solid colour, a gradient laid out like Gradient Overlay over the
       layer's frame (or the canvas without Align), or a pattern tiled from the frame's corner (or
       the canvas's without Link), scaled and rotated. */
    void render_fill (const doc::Layer& layer, Rect rect, const Ctx& cx, Buffer* out)
    {
        const doc::FillLayer& f = layer.fill;
        *out                    = Buffer::transparent (rect);
        Rect frame              = fill_frame (layer, cx.canvas);
        size_t w = rect.width (), n = out->pixel_count ();
        if (f.kind == doc::FillKind::Solid)
        {
            for (size_t i = 0; i < n; i++)
            {
                memcpy (&out->px[i * 4], f.color, 3 * sizeof (float));
                out->px[i * 4 + 3] = 1.0f;
            }
            return;
        }
        if (f.kind == doc::FillKind::Gradient)
        {
            doc::Effect e = doc::Effect::make (doc::FxKind::GradientOverlay);
            e.angle       = f.angle;
            e.scale       = f.scale;
            e.gradient    = f.style;
            e.reverse     = f.reverse;
            Rect box      = f.align ? frame : cx.canvas;
#pragma omp parallel for schedule(static)
            for (ptrdiff_t ii = 0; ii < (ptrdiff_t) n; ii++)
            {
                size_t i  = (size_t) ii;
                int32_t x = rect.x0 + (int32_t) (i % w), y = rect.y0 + (int32_t) (i / w);
                float t = gradient_t (e, box, (float) x + 0.5f, (float) y + 0.5f), c[4];
                fill_gradient_color (f, t, c);
                if (f.dither)
                {
                    /* One 8-bit level of noise breaks up banding. */
                    uint32_t hsh = (uint32_t) x * 0x8da6b343u ^ (uint32_t) y * 0xd8163841u;
                    hsh ^= hsh >> 13;
                    hsh *= 0x5bd1e995u;
                    float d = ((float) (hsh & 0xffff) / 65535.0f - 0.5f) / 255.0f;
                    for (int k = 0; k < 3; k++)
                    {
                        c[k] = fminf (fmaxf (c[k] + d, 0.0f), 1.0f);
                    }
                }
                memcpy (&out->px[i * 4], c, sizeof c);
            }
            return;
        }
        if (f.pattern_w == 0 || f.pattern_h == 0 || f.pattern_rgba.size () < (size_t) f.pattern_w * f.pattern_h * 4)
        {
            return;
        }
        float ox = (f.link ? (float) frame.x0 : 0.0f) + f.phase[0],
              oy = (f.link ? (float) frame.y0 : 0.0f) + f.phase[1];
        float sc = fmaxf (f.pattern_scale, 0.01f), a = -f.pattern_angle * (float) M_PI / 180.0f, ca = cosf (a),
              sa   = sinf (a);
        int32_t pw = (int32_t) f.pattern_w, ph = (int32_t) f.pattern_h;
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) n; ii++)
        {
            size_t i = (size_t) ii;
            float x = (float) rect.x0 + (float) (i % w) + 0.5f - ox, y = (float) rect.y0 + (float) (i / w) + 0.5f - oy;
            float u = (x * ca - y * sa) / sc, v = (x * sa + y * ca) / sc;
            int32_t px = ((int32_t) floorf (u) % pw + pw) % pw, py = ((int32_t) floorf (v) % ph + ph) % ph;
            memcpy (
                &out->px[i * 4], &f.pattern_rgba[((size_t) py * f.pattern_w + (size_t) px) * 4], 4 * sizeof (float));
        }
    }

    /* The layer's own content over rect with its mask applied; false for layers without pixels of
       their own (adjustments). */
    bool render_content (const doc::Layer& layer, Rect rect, const Ctx& cx, Buffer* out)
    {
        if (layer.kind == doc::LayerKind::Adjustment)
        {
            return false;
        }
        if (layer.kind == doc::LayerKind::Group)
        {
            *out = Buffer::transparent (rect);
            composite_stack (layer.children, out, cx);
        }
        else if (layer.kind == doc::LayerKind::Fill)
        {
            render_fill (layer, rect, cx, out);
        }
        else
        {
            const raster::Surface* s = layer.surface ();
            if (s != NULL)
            {
                *out = surface_to_buffer (*s, rect, cx.cmyk);
            }
            else
            {
                *out = Buffer::transparent (rect);
            }
        }
        std::vector<float> m;
        if (mask_vals (layer, rect, cx, &m))
        {
            for (size_t i = 0; i < out->pixel_count (); i++)
            {
                out->px[i * 4 + 3] *= m[i];
            }
        }
        return true;
    }

    void blend_into_g (Buffer* backdrop, const Buffer& src, BlendMode mode, float opacity, float gamma, const Ctx& cx)
    {
        size_t n = backdrop->pixel_count ();
        if (mode == BlendMode::Normal && gamma == 1.0f && !cx.space.lab_mix)
        {
            for (size_t i = 0; i < n; i++)
            {
                const float* s = &src.px[i * 4];
                if (s[3] <= 0.0f)
                {
                    continue;
                }
                float* b = &backdrop->px[i * 4];
                composite_normal_rgb (b, s, opacity, b);
            }
            return;
        }
        Rect rect = backdrop->rect;
        int32_t w = (int32_t) rect.width ();
        for (size_t i = 0; i < n; i++)
        {
            float s[4];
            memcpy (s, &src.px[i * 4], sizeof s);
            if (s[3] <= 0.0f)
            {
                continue;
            }
            float* b = &backdrop->px[i * 4];
            if (mode == BlendMode::Dissolve)
            {
                int32_t x = rect.x0 + (int32_t) i % w;
                int32_t y = rect.y0 + (int32_t) i / w;
                s[3]      = dissolve_noise (x, y) < s[3] * opacity ? 1.0f : 0.0f;
                ps_composite (cx.space, BlendMode::Normal, b, s, 1.0f, b);
                continue;
            }
            composite_gamma (cx.space, mode, b, s, opacity, gamma, b);
        }
    }

    /* A layer's own opacity and fill: Fill is part of the blend for the special eight, plain
       coverage otherwise. */
    void blend_into_fill (
        Buffer* backdrop, const Buffer& src, BlendMode mode, float opacity, float fill, float gamma, const Ctx& cx)
    {
        if (fill >= 1.0f || !fill_is_special (mode))
        {
            blend_into_g (backdrop, src, mode, opacity * fill, gamma, cx);
            return;
        }
        for (size_t i = 0; i < backdrop->pixel_count (); i++)
        {
            const float* s = &src.px[i * 4];
            if (s[3] <= 0.0f)
            {
                continue;
            }
            float* b = &backdrop->px[i * 4];
            composite_fill (cx.space, mode, b, s, opacity, fill, gamma, b);
        }
    }

    /* A clipped layer composited atop its base: blended as if the base were opaque, keeping the
       base's alpha. */
    void composite_atop (const doc::Layer& layer, Buffer* base, const Ctx& cx)
    {
        if (layer.kind == doc::LayerKind::Adjustment)
        {
            Buffer adjusted = *base;
            apply_adjustment (layer.adjustment, &adjusted, adjust_context (cx));
            std::vector<float> mv;
            bool has_mv = mask_vals (layer, base->rect, cx, &mv);
            for (size_t i = 0; i < base->pixel_count (); i++)
            {
                float k  = layer.opacity * layer.fill_opacity * (has_mv ? mv[i] : 1.0f);
                float* p = &base->px[i * 4];
                float bl[3];
                ps_blend_rgb (cx.space, layer.blend, p, &adjusted.px[i * 4], bl);
                for (int c = 0; c < 3; c++)
                {
                    p[c] += (bl[c] - p[c]) * k;
                }
            }
            return;
        }
        Buffer content;
        if (!render_content (layer, base->rect, cx, &content))
        {
            return;
        }
        float opacity = layer.opacity * layer.fill_opacity;
        float gamma   = layer_text_gamma (layer, cx);
        for (size_t i = 0; i < base->pixel_count (); i++)
        {
            float* p    = &base->px[i * 4];
            float alpha = p[3];
            if (alpha <= 0.0f)
            {
                continue;
            }
            float opaque[4] = {p[0], p[1], p[2], 1.0f};
            float r[4];
            composite_gamma (cx.space, layer.blend, opaque, &content.px[i * 4], opacity, gamma, r);
            p[0] = r[0];
            p[1] = r[1];
            p[2] = r[2];
            p[3] = alpha;
        }
    }

    void composite_clipped (
        const std::vector<doc::Layer>& layers, size_t begin, size_t end, Buffer* base, const Ctx& cx)
    {
        for (size_t i = begin; i < end; i++)
        {
            if (layers[i].visible)
            {
                composite_atop (layers[i], base, cx);
            }
        }
    }

    bool any_visible (const std::vector<doc::Layer>& layers, size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; i++)
        {
            if (layers[i].visible)
            {
                return true;
            }
        }
        return false;
    }

    /* Pass-through groups composite their children straight into the backdrop; opacity and masks
       then mix the result with the original backdrop. Layers clipped to the group add their effect
       on the group's isolated rendering. */
    void composite_pass_through (const doc::Layer& layer, const std::vector<doc::Layer>& siblings, size_t clip_begin,
        size_t clip_end, Buffer* backdrop, const Ctx& cx)
    {
        Rect rect        = backdrop->rect;
        float opacity    = layer.opacity * layer.fill_opacity;
        bool needs_mix   = opacity < 1.0f || layer.has_mask || layer.has_vector_mask;
        bool has_clipped = any_visible (siblings, clip_begin, clip_end);
        Buffer before;
        if (needs_mix || has_clipped)
        {
            before = *backdrop;
        }
        composite_stack (layer.children, backdrop, cx);

        if (needs_mix)
        {
            std::vector<float> mv;
            bool has_mv = mask_vals (layer, rect, cx, &mv);
            for (size_t i = 0; i < backdrop->pixel_count (); i++)
            {
                float k        = opacity * (has_mv ? mv[i] : 1.0f);
                float* p       = &backdrop->px[i * 4];
                const float* a = &before.px[i * 4];
                float wa       = a[3] * (1.0f - k);
                float wb       = p[3] * k;
                float alpha    = wa + wb;
                if (alpha > 0.0f)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        p[c] = (a[c] * wa + p[c] * wb) / alpha;
                    }
                    p[3] = alpha;
                }
                else
                {
                    p[0] = p[1] = p[2] = p[3] = 0.0f;
                }
            }
        }

        Buffer iso;
        if (!has_clipped || !render_content (layer, rect, cx, &iso))
        {
            return;
        }
        Buffer clipped_iso = iso;
        composite_clipped (siblings, clip_begin, clip_end, &clipped_iso, cx);
        Buffer without = before;
        blend_into_g (&without, iso, BlendMode::Normal, opacity, 1.0f, cx);
        Buffer with = before;
        blend_into_g (&with, clipped_iso, BlendMode::Normal, opacity, 1.0f, cx);
        for (size_t i = 0; i < backdrop->pixel_count (); i++)
        {
            float* p        = &backdrop->px[i * 4];
            const float* w  = &with.px[i * 4];
            const float* wo = &without.px[i * 4];
            float pa        = p[3];
            float pm[4]     = {p[0] * pa, p[1] * pa, p[2] * pa, pa};
            for (int c = 0; c < 3; c++)
            {
                pm[c] += w[c] * w[3] - wo[c] * wo[3];
            }
            pm[3] += w[3] - wo[3];
            float a = pm[3] < 0.0f ? 0.0f : pm[3] > 1.0f ? 1.0f : pm[3];
            if (a > 0.0f)
            {
                for (int c = 0; c < 3; c++)
                {
                    float v = pm[c] / a;
                    p[c]    = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
                }
                p[3] = a;
            }
            else
            {
                p[0] = p[1] = p[2] = p[3] = 0.0f;
            }
        }
    }

    /* How much a Blend If range lets a level (0..255) through: nothing below the black point's
       first half, a ramp across its split, all of it up to the white point, then a ramp down. */
    float blend_range_weight (float v, const float black[2], const float white[2])
    {
        if (v < black[0] || v > white[1])
        {
            return 0.0f;
        }
        if (v < black[1])
        {
            return black[1] > black[0] ? (v - black[0]) / (black[1] - black[0]) : 1.0f;
        }
        if (v > white[0])
        {
            return white[1] > white[0] ? (white[1] - v) / (white[1] - white[0]) : 1.0f;
        }
        return 1.0f;
    }

    /* Blend If: `content`'s alpha scaled where this layer's or the underlying levels fall outside
       the ranges (Gray by luminosity, then each of R, G and B). Pixels of `content` outside
       `backdrop` see the backdrop as unknown and only test this layer. */
    void apply_blend_if (const doc::BlendIf& bi, Buffer* content, const Buffer& backdrop)
    {
        Rect r = content->rect, br = backdrop.rect;
        size_t w = r.width (), n = content->pixel_count ();
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) n; ii++)
        {
            size_t i = (size_t) ii;
            float* p = &content->px[i * 4];
            if (p[3] <= 0.0f)
            {
                continue;
            }
            int32_t x = r.x0 + (int32_t) (i % w), y = r.y0 + (int32_t) (i / w);
            const float* u = br.contains (x, y) ? backdrop.at (x, y) : NULL;
            float k        = 1.0f;
            for (int ch = 0; ch < 4 && k > 0.0f; ch++)
            {
                const doc::BlendRange& range = ch == 0 ? bi.gray : bi.channels[ch - 1];
                float sv                     = ch == 0 ? 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2] : p[ch - 1];
                k *= blend_range_weight (sv * 255.0f, range.this_black, range.this_white);
                if (u != NULL)
                {
                    float uv = ch == 0 ? 0.3f * u[0] + 0.59f * u[1] + 0.11f * u[2] : u[ch - 1];
                    k *= blend_range_weight (uv * 255.0f, range.under_black, range.under_white);
                }
            }
            p[3] *= k;
        }
    }

    void composite_layer (const doc::Layer& layer, const std::vector<doc::Layer>& siblings, size_t clip_begin,
        size_t clip_end, Buffer* backdrop, const Ctx& cx)
    {
        bool blend_if = !layer.blend_if.is_default ();
        Rect rect     = backdrop->rect;
        if (layer.kind != doc::LayerKind::Adjustment && has_effects (layer))
        {
            /* Layer styles: the content over the rect grown by twice the effects' reach, so every
               map pixel inside the rect sees all the shape it depends on. */
            Rect big = rect.inflate (2 * effects_margin (layer));
            if (empty_in (layer, big))
            {
                return;
            }
            Buffer content;
            if (!render_content (layer, big, cx, &content))
            {
                return;
            }
            composite_clipped (siblings, clip_begin, clip_end, &content, cx);
            if (blend_if)
            {
                apply_blend_if (layer.blend_if, &content, *backdrop);
            }
            composite_with_effects (layer, content, backdrop, layer_bounds (layer, cx.canvas), cx.light_angle,
                cx.light_altitude, cx.space, cx.doc);
            return;
        }
        if (empty_in (layer, rect))
        {
            return;
        }
        if (layer.kind == doc::LayerKind::Group && layer.blend == BlendMode::PassThrough && layer.fill_opacity >= 1.0f)
        {
            composite_pass_through (layer, siblings, clip_begin, clip_end, backdrop, cx);
            return;
        }
        if (layer.kind == doc::LayerKind::Adjustment)
        {
            /* The backdrop transformed, then blended back in through the mask and opacity. Layers
               clipped to an adjustment composite atop the adjusted result. */
            Buffer adjusted = *backdrop;
            apply_adjustment (layer.adjustment, &adjusted, adjust_context (cx));
            composite_clipped (siblings, clip_begin, clip_end, &adjusted, cx);
            std::vector<float> mv;
            bool has_mv   = mask_vals (layer, rect, cx, &mv);
            float opacity = layer.opacity * layer.fill_opacity;
            for (size_t i = 0; i < backdrop->pixel_count (); i++)
            {
                float k = opacity * (has_mv ? mv[i] : 1.0f);
                if (k <= 0.0f)
                {
                    continue;
                }
                float* b = &backdrop->px[i * 4];
                float bl[3];
                ps_blend_rgb (cx.space, layer.blend, b, &adjusted.px[i * 4], bl);
                for (int c = 0; c < 3; c++)
                {
                    b[c] += (bl[c] - b[c]) * k;
                }
            }
            return;
        }
        Buffer content;
        if (!render_content (layer, rect, cx, &content))
        {
            return;
        }
        composite_clipped (siblings, clip_begin, clip_end, &content, cx);
        if (blend_if)
        {
            apply_blend_if (layer.blend_if, &content, *backdrop);
        }
        blend_into_fill (
            backdrop, content, layer.blend, layer.opacity, layer.fill_opacity, layer_text_gamma (layer, cx), cx);
    }

    /* A sibling list (bottom to top) onto backdrop. A layer and the clipped layers that follow it
       form a clipping group. */
    void composite_stack_at (
        const std::vector<doc::Layer>& layers, size_t begin, size_t end, Buffer* backdrop, const Ctx& cx)
    {
        size_t i = begin;
        while (i < end)
        {
            const doc::Layer& base = layers[i];
            size_t j               = i + 1;
            while (j < end && layers[j].clipped && !base.clipped)
            {
                j++;
            }
            if (base.visible)
            {
                composite_layer (base, layers, i + 1, j, backdrop, cx);
                /* Photoshop applies adjustment layers to buffers of the document's depth, so their
                   result is rounded there (8-bit: 255 levels, 16-bit: 32768). */
                float q;
                if (base.kind == doc::LayerKind::Adjustment && adjustment_quantum (cx.depth, &q))
                {
                    for (size_t k = 0; k < backdrop->px.size (); k++)
                    {
                        backdrop->px[k] = floorf (backdrop->px[k] * q + 0.5f) / q;
                    }
                }
            }
            i = j > i + 1 ? j : i + 1;
        }
    }

    Ctx make_ctx (const doc::Document& d, const RenderSettings& settings)
    {
        Ctx cx;
        cx.doc                           = &d;
        cx.canvas                        = d.bounds ();
        cx.mode                          = d.mode;
        cx.depth                         = d.depth;
        cx.space.lab_mix                 = d.mode == color::ColorMode::Lab;
        cx.space.hdr                     = d.depth == color::SampleType::F32;
        cx.cmyk                          = settings.cmyk;
        cx.text_gamma                    = settings.text_gamma;
        cx.transfer                      = Transfer::for_document (d.mode, d.depth);
        cx.light_angle                   = d.light_angle;
        cx.light_altitude                = d.light_altitude;
        std::vector<doc::WalkEntry> walk = d.walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            const doc::Layer* l = walk[i].layer;
            if (!l->has_mask || !has_feather (l->mask))
            {
                continue;
            }
            raster::Surface m = settings.masks != NULL ? settings.masks->feathered (l->mask, cx.canvas)
                                                       : feathered_mask (l->mask, cx.canvas);
            cx.feathered.push_back (std::make_pair (l->id, m));
        }
        for (size_t i = 0; i < walk.size (); i++)
        {
            const doc::Layer* l = walk[i].layer;
            if (!l->has_vector_mask)
            {
                continue;
            }
            doc::LayerMask vm = doc::vector_mask_view (l->vector_mask);
            if (!has_feather (vm))
            {
                continue;
            }
            raster::Surface m =
                settings.masks != NULL ? settings.masks->feathered (vm, cx.canvas) : feathered_mask (vm, cx.canvas);
            cx.vfeathered.push_back (std::make_pair (l->id, m));
        }
        return cx;
    }

    Buffer render (const doc::Document& d, Rect rect, const RenderSettings& settings)
    {
        return render_tiled (d, rect, RENDER_TILE, settings);
    }

    /* The document over rect, in independent tiles rendered in parallel. Each tile writes its own
       part of the output, so the threads never touch the same pixels. */
    Buffer render_tiled (const doc::Document& d, Rect rect, int32_t tile, const RenderSettings& settings)
    {
        if (tile < 1)
        {
            tile = 1;
        }
        Buffer out = Buffer::transparent (rect);
        if (rect.is_empty ())
        {
            return out;
        }
        Ctx cx = make_ctx (d, settings);

        std::vector<Rect> tiles;
        for (int32_t y = rect.y0; y < rect.y1;)
        {
            int32_t y1 = geom::add_sat (y, tile);
            if (y1 > rect.y1)
            {
                y1 = rect.y1;
            }
            for (int32_t x = rect.x0; x < rect.x1;)
            {
                int32_t x1 = geom::add_sat (x, tile);
                if (x1 > rect.x1)
                {
                    x1 = rect.x1;
                }
                Rect t = {x, y, x1, y1};
                tiles.push_back (t);
                x = x1;
            }
            y = y1;
        }

        size_t w = rect.width ();
#pragma omp parallel for schedule(dynamic)
        for (ptrdiff_t k = 0; k < (ptrdiff_t) tiles.size (); k++)
        {
            Rect t      = tiles[(size_t) k];
            Buffer part = Buffer::transparent (t);
            composite_stack (d.layers, &part, cx);
            /* With `inks`, a Duotone image shows as printed: each gray through its inks. */
            if (settings.inks && d.mode == color::ColorMode::Duotone && d.has_duotone)
            {
                for (size_t i = 0; i < part.pixel_count (); i++)
                {
                    float *p = &part.px[i * 4], c[3];
                    d.duotone.render (p[0], c);
                    p[0] = c[0];
                    p[1] = c[1];
                    p[2] = c[2];
                }
            }
            size_t pw = t.width ();
            for (int32_t y = t.y0; y < t.y1; y++)
            {
                const float* src = &part.px[((size_t) (y - t.y0)) * pw * 4];
                float* dst       = &out.px[(((size_t) (y - rect.y0)) * w + (size_t) (t.x0 - rect.x0)) * 4];
                memcpy (dst, src, pw * 4 * sizeof (float));
            }
        }
        return out;
    }

    /* A fill layer's own pixels over rect (no mask): what a format without fill layers stores. */
    Buffer fill_layer_pixels (const doc::Document& d, const doc::Layer& l, Rect rect, const RenderSettings& settings)
    {
        Ctx cx = make_ctx (d, settings);
        Buffer out;
        render_fill (l, rect, cx, &out);
        return out;
    }

    Buffer flatten (const doc::Document& d, const RenderSettings& settings)
    {
        return render (d, d.bounds (), settings);
    }

    /* The composite as a surface of `format`, optionally over an opaque background colour. Fully
       transparent pixels keep the surface's default pixel. */
    raster::Surface composite_to_surface (const doc::Document& d, Rect rect, color::PixelFormat format,
        const float* background, const RenderSettings& settings)
    {
        Buffer b = render (d, rect, settings);
        if (background != NULL)
        {
            b = b.over_background (background);
        }
        raster::Surface s (format);
        size_t n = format.channels ();
        float empty[raster::MAX_CHANNELS];
        s.default_pixel (empty);
        std::vector<float> data (b.pixel_count () * n);
        for (size_t i = 0; i < b.pixel_count (); i++)
        {
            const float* p = &b.px[i * 4];
            float* o       = &data[i * n];
            if (format.alpha && (p[3] <= 0.0f || isnan (p[3])))
            {
                memcpy (o, empty, n * sizeof (float));
            }
            else
            {
                raster::from_rgba (format, p, settings.cmyk, o);
            }
        }
        s.write_region (rect, data.data (), data.size ());
        s.prune ();
        return s;
    }

} // namespace compose
