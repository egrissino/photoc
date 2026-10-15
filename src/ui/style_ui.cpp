#include "color/blend.h"
#include "engine/style.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The Layer Style dialog: Photoshop's list of effects on the left (a checkbox each; + adds
       another instance of the kinds that allow several), the selected effect's settings on the
       right, previewed live on the canvas. OK sets the whole style with one step; Cancel leaves
       the layer as it was. The Layers panel lists a styled layer's effects under it. */

    /* The kinds in the order of Photoshop's list. */
    const doc::FxKind STYLE_ORDER[doc::FX_KIND_COUNT] = {doc::FxKind::BevelEmboss, doc::FxKind::Stroke,
        doc::FxKind::InnerShadow, doc::FxKind::InnerGlow, doc::FxKind::Satin, doc::FxKind::ColorOverlay,
        doc::FxKind::GradientOverlay, doc::FxKind::PatternOverlay, doc::FxKind::OuterGlow, doc::FxKind::DropShadow};

    bool fx_multi (doc::FxKind k)
    {
        return k == doc::FxKind::DropShadow || k == doc::FxKind::InnerShadow || k == doc::FxKind::ColorOverlay
            || k == doc::FxKind::GradientOverlay || k == doc::FxKind::Stroke;
    }

    int blend_index (color::BlendMode m)
    {
        for (size_t i = 0; i < color::LAYER_MODE_COUNT; i++)
        {
            if (color::LAYER_MODES[i] == m)
            {
                return (int) i;
            }
        }
        return 0;
    }

    /* A blend mode combo editing `m`; true when changed. */
    bool fx_blend (const char* label, color::BlendMode* m, float width)
    {
        int i      = blend_index (*m);
        int before = i;
        blend_combo (label, &i, width);
        *m = color::LAYER_MODES[i];
        return i != before;
    }

    /* A slider over a 0..1 value shown as percent. */
    bool fx_percent (const char* label, float* v, float width)
    {
        float pct = *v * 100.0f;
        ImGui::SetNextItemWidth (width);
        bool changed = ImGui::SliderFloat (label, &pct, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
        if (changed)
        {
            *v = pct / 100.0f;
        }
        return changed;
    }

    bool fx_px (const char* label, float* v, float lo, float hi, float width)
    {
        ImGui::SetNextItemWidth (width);
        return ImGui::SliderFloat (label, v, lo, hi, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
    }

    /* Opens the dialog on the active layer; `kind` (when not NULL) selects that effect, adding it
       when the layer has none. */
    void App::open_layer_style (const doc::FxKind* kind, int index)
    {
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        if (l == NULL)
        {
            set_error ("select a layer first");
            return;
        }
        if (l->kind == doc::LayerKind::Adjustment)
        {
            set_error ("adjustment layers can't have a layer style");
            return;
        }
        if (d->doc->is_background (l->id))
        {
            set_error (
                "the Background can't have a layer style; double-click it in the Layers panel to make it a layer "
                "first");
            return;
        }
        engine::ensure_patterns (session_);
        show_style_     = true;
        style_layer_    = l->id;
        style_work_     = l->style;
        style_light_[0] = d->doc->light_angle;
        style_light_[1] = d->doc->light_altitude;
        style_sel_      = index >= 0 && index < (int) style_work_.items.size () ? index : index == -2 ? -2 : -1;
        style_blend_    = l->blend;
        style_opacity_  = l->opacity;
        style_fill_     = l->fill_opacity;
        for (int c = 0; c < 3; c++)
        {
            style_channels_[c] = (l->excluded_channels & (1u << c)) == 0;
        }
        style_blend_if_ = l->blend_if;
        if (kind != NULL && style_sel_ < 0)
        {
            for (size_t i = 0; i < style_work_.items.size () && style_sel_ < 0; i++)
            {
                if (style_work_.items[i].kind == *kind)
                {
                    style_sel_ = (int) i;
                }
            }
            if (style_sel_ < 0)
            {
                size_t at = doc::fx_insert_at (style_work_.items, *kind);
                style_work_.items.insert (style_work_.items.begin () + (ptrdiff_t) at, doc::Effect::make (*kind));
                style_sel_ = (int) at;
            }
            else
            {
                style_work_.items[(size_t) style_sel_].enabled = true;
            }
        }
        style_dirty_ = true;
    }

    /* The selected effect's settings; true when anything changed. */
    bool App::effect_settings (doc::Effect* e)
    {
        float k = ui_scale_, fw = 200 * k;
        bool ch           = false;
        const char* title = doc::fx_kind_label (e->kind);
        ImGui::PushFont (fonts_.semibold, 0.0f);
        ImGui::TextUnformatted (title);
        ImGui::PopFont ();
        ImGui::Separator ();
        bool shadow = e->kind == doc::FxKind::DropShadow || e->kind == doc::FxKind::InnerShadow;
        bool glow   = e->kind == doc::FxKind::OuterGlow || e->kind == doc::FxKind::InnerGlow;
        if (e->kind == doc::FxKind::Stroke)
        {
            ImGui::SeparatorText ("Structure");
            ch |= fx_px ("Size", &e->size, 1, 250, fw);
            int pos = (int) e->position;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::Combo ("Position", &pos, "Outside\0Inside\0Center\0"))
            {
                e->position = (doc::StrokePosition) pos;
                ch          = true;
            }
            ch |= fx_blend ("Blend Mode", &e->blend, fw);
            ch |= fx_percent ("Opacity", &e->opacity, fw);
            ImGui::SeparatorText ("Fill Type: Color");
            ch |= ImGui::ColorEdit3 ("Color", e->color, ImGuiColorEditFlags_NoInputs);
            return ch;
        }
        if (e->kind == doc::FxKind::BevelEmboss)
        {
            ImGui::SeparatorText ("Structure");
            int style = (int) e->bevel;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::Combo ("Style", &style, "Outer Bevel\0Inner Bevel\0Emboss\0Pillow Emboss\0"))
            {
                e->bevel = (doc::BevelStyle) style;
                ch       = true;
            }
            ImGui::BeginDisabled ();
            int tech = 0;
            ImGui::SetNextItemWidth (fw);
            ImGui::Combo ("Technique", &tech, "Smooth\0");
            ImGui::EndDisabled ();
            float depth = e->depth * 100.0f;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::SliderFloat ("Depth", &depth, 1.0f, 1000.0f, "%.0f %%",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
            {
                e->depth = depth / 100.0f;
                ch       = true;
            }
            int dir = e->up ? 0 : 1;
            ImGui::TextUnformatted ("Direction:");
            ImGui::SameLine ();
            if (ImGui::RadioButton ("Up", &dir, 0) | ImGui::RadioButton ("Down", &dir, 1))
            {
                e->up = dir == 0;
                ch    = true;
            }
            ch |= fx_px ("Size", &e->size, 0, 250, fw);
            ch |= fx_px ("Soften", &e->soften, 0, 16, fw);
            ImGui::SeparatorText ("Shading");
        }
        else if (shadow || e->kind == doc::FxKind::Satin)
        {
            ImGui::SeparatorText ("Structure");
            ch |= fx_blend ("Blend Mode", &e->blend, fw);
            ImGui::SameLine ();
            ch |= ImGui::ColorEdit3 ("##fxcolor", e->color, ImGuiColorEditFlags_NoInputs);
            ch |= fx_percent ("Opacity", &e->opacity, fw);
        }
        else if (glow)
        {
            ImGui::SeparatorText ("Structure");
            ch |= fx_blend ("Blend Mode", &e->blend, fw);
            ch |= fx_percent ("Opacity", &e->opacity, fw);
            ch |= ImGui::ColorEdit3 ("Color", e->color, ImGuiColorEditFlags_NoInputs);
            ImGui::SeparatorText ("Elements");
            int tech = e->precise ? 1 : 0;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::Combo ("Technique", &tech, "Softer\0Precise\0"))
            {
                e->precise = tech == 1;
                ch         = true;
            }
            if (e->kind == doc::FxKind::InnerGlow)
            {
                int src = e->center ? 1 : 0;
                ImGui::TextUnformatted ("Source:");
                ImGui::SameLine ();
                if (ImGui::RadioButton ("Center", &src, 1) | ImGui::RadioButton ("Edge", &src, 0))
                {
                    e->center = src == 1;
                    ch        = true;
                }
            }
            ch |= fx_percent (e->kind == doc::FxKind::OuterGlow ? "Spread" : "Choke", &e->spread, fw);
            ch |= fx_px ("Size", &e->size, 0, 250, fw);
            ImGui::SeparatorText ("Quality");
            float range = e->range * 100.0f;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::SliderFloat ("Range", &range, 1.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
            {
                e->range = range / 100.0f;
                ch       = true;
            }
            return ch;
        }
        else if (e->kind == doc::FxKind::ColorOverlay)
        {
            ImGui::SeparatorText ("Color");
            ch |= fx_blend ("Blend Mode", &e->blend, fw);
            ImGui::SameLine ();
            ch |= ImGui::ColorEdit3 ("##fxcolor", e->color, ImGuiColorEditFlags_NoInputs);
            ch |= fx_percent ("Opacity", &e->opacity, fw);
            return ch;
        }
        else if (e->kind == doc::FxKind::GradientOverlay)
        {
            ImGui::SeparatorText ("Gradient");
            ch |= fx_blend ("Blend Mode", &e->blend, fw);
            ch |= fx_percent ("Opacity", &e->opacity, fw);
            ch |= ImGui::ColorEdit3 ("Start", e->color, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine ();
            ch |= ImGui::ColorEdit3 ("End", e->color2, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine ();
            ch |= ImGui::Checkbox ("Reverse", &e->reverse);
            int style = (int) e->gradient;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::Combo ("Style", &style, "Linear\0Radial\0Angle\0Reflected\0Diamond\0"))
            {
                e->gradient = (doc::GradientStyle) style;
                ch          = true;
            }
        }
        if (e->kind == doc::FxKind::PatternOverlay)
        {
            ImGui::SeparatorText ("Pattern");
            ch |= fx_blend ("Blend Mode", &e->blend, fw);
            ch |= fx_percent ("Opacity", &e->opacity, fw);
            int index = 0;
            for (size_t i = 0; i < session_.patterns.size (); i++)
            {
                if (session_.patterns[i].name == e->pattern)
                {
                    index = (int) i;
                }
            }
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Pattern:");
            ImGui::SameLine ();
            if (pattern_combo ("##fxpattern", &index, fw) && index >= 0 && (size_t) index < session_.patterns.size ())
            {
                snprintf (e->pattern, sizeof e->pattern, "%s", session_.patterns[(size_t) index].name.c_str ());
                ch = true;
            }
            float scale = e->scale * 100.0f;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::SliderFloat ("Scale", &scale, 1.0f, 1000.0f, "%.0f %%",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
            {
                e->scale = scale / 100.0f;
                ch       = true;
            }
            ch |= ImGui::Checkbox ("Link with Layer", &e->link);
            ImGui::SameLine ();
            if (ImGui::Button ("Snap to Origin"))
            {
                e->phase[0] = e->phase[1] = 0.0f;
                ch                        = true;
            }
            return ch;
        }
        /* Angle (with the global light for shadows and bevels), then distance and size. */
        if (e->kind != doc::FxKind::ColorOverlay)
        {
            bool lit     = shadow || e->kind == doc::FxKind::BevelEmboss;
            float* angle = lit && e->global_light ? &style_light_[0] : &e->angle;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::SliderFloat ("Angle", angle, -180.0f, 180.0f, "%.0f°", ImGuiSliderFlags_AlwaysClamp))
            {
                ch = true;
            }
            if (lit)
            {
                ImGui::SameLine ();
                ch |= ImGui::Checkbox ("Use Global Light", &e->global_light);
            }
            if (e->kind == doc::FxKind::BevelEmboss)
            {
                float* alt = e->global_light ? &style_light_[1] : &e->altitude;
                ImGui::SetNextItemWidth (fw);
                ch |= ImGui::SliderFloat ("Altitude", alt, 0.0f, 90.0f, "%.0f°", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SeparatorText ("Highlight / Shadow");
                ch |= fx_blend ("Highlight Mode", &e->blend, fw);
                ImGui::SameLine ();
                ch |= ImGui::ColorEdit3 ("##hicolor", e->color, ImGuiColorEditFlags_NoInputs);
                ch |= fx_percent ("Opacity##hi", &e->opacity, fw);
                ch |= fx_blend ("Shadow Mode", &e->blend2, fw);
                ImGui::SameLine ();
                ch |= ImGui::ColorEdit3 ("##shcolor", e->color2, ImGuiColorEditFlags_NoInputs);
                ch |= fx_percent ("Opacity##sh", &e->opacity2, fw);
                return ch;
            }
        }
        if (e->kind == doc::FxKind::GradientOverlay)
        {
            float scale = e->scale * 100.0f;
            ImGui::SetNextItemWidth (fw);
            if (ImGui::SliderFloat ("Scale", &scale, 10.0f, 150.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp))
            {
                e->scale = scale / 100.0f;
                ch       = true;
            }
            return ch;
        }
        ch |= fx_px ("Distance", &e->distance, 0, 250, fw);
        if (shadow)
        {
            ch |= fx_percent (e->kind == doc::FxKind::DropShadow ? "Spread" : "Choke", &e->spread, fw);
        }
        ch |= fx_px ("Size", &e->size, 0, 250, fw);
        if (e->kind == doc::FxKind::Satin)
        {
            ch |= ImGui::Checkbox ("Invert", &e->invert);
        }
        if (e->kind == doc::FxKind::DropShadow)
        {
            ch |= ImGui::Checkbox ("Layer Knocks Out Drop Shadow", &e->knocks_out);
        }
        return ch;
    }

    void App::layer_style_dialog ()
    {
        if (!show_style_)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL || d->doc->layer (style_layer_) == NULL)
        {
            show_style_ = false;
            clear_preview ();
            return;
        }
        float k = ui_scale_;
        ImGui::SetNextWindowSize (ImVec2 (760 * k, 560 * k), ImGuiCond_Appearing);
        ImGui::OpenPopup ("Layer Style###layerstyle");
        if (!ImGui::BeginPopupModal ("Layer Style###layerstyle", NULL, 0))
        {
            return;
        }
        std::vector<doc::Effect>& items = style_work_.items;
        float list_w = 220 * k, buttons_w = 130 * k;
        ImGui::BeginChild ("##fxlist", ImVec2 (list_w, -1), ImGuiChildFlags_Borders);
        ImGui::TextDisabled ("Styles");
        ImGui::Separator ();
        if (ImGui::Selectable ("Blending Options", style_sel_ == -2))
        {
            style_sel_ = -2;
        }
        ImGui::Separator ();
        int add_kind = -1, add_after = -1;
        for (size_t o = 0; o < doc::FX_KIND_COUNT; o++)
        {
            doc::FxKind kind = STYLE_ORDER[o];
            bool any         = false;
            for (size_t i = 0; i < items.size (); i++)
            {
                if (items[i].kind != kind)
                {
                    continue;
                }
                any = true;
                ImGui::PushID ((int) i);
                if (ImGui::Checkbox ("##on", &items[i].enabled))
                {
                    style_dirty_ = true;
                }
                ImGui::SameLine ();
                if (ImGui::Selectable (
                        doc::fx_kind_label (kind), style_sel_ == (int) i, 0, ImVec2 (list_w - 80 * k, 0)))
                {
                    style_sel_       = (int) i;
                    items[i].enabled = true;
                    style_dirty_     = true;
                }
                if (fx_multi (kind))
                {
                    ImGui::SameLine ();
                    if (ImGui::SmallButton ("+"))
                    {
                        add_kind  = (int) kind;
                        add_after = (int) i;
                    }
                    if (ImGui::IsItemHovered ())
                    {
                        ImGui::SetTooltip ("Add another %s", doc::fx_kind_label (kind));
                    }
                }
                ImGui::PopID ();
            }
            if (any)
            {
                continue;
            }
            ImGui::PushID ((int) (1000 + o));
            bool off = false;
            if (ImGui::Checkbox ("##on", &off))
            {
                add_kind = (int) kind;
            }
            ImGui::SameLine ();
            if (ImGui::Selectable (doc::fx_kind_label (kind), false, 0, ImVec2 (list_w - 80 * k, 0)))
            {
                add_kind = (int) kind;
            }
            ImGui::PopID ();
        }
        if (add_kind >= 0)
        {
            doc::Effect fx = doc::Effect::make ((doc::FxKind) add_kind);
            size_t at      = add_after >= 0 ? (size_t) add_after : doc::fx_insert_at (items, (doc::FxKind) add_kind);
            items.insert (items.begin () + (ptrdiff_t) at, fx);
            style_sel_   = (int) at;
            style_dirty_ = true;
        }
        ImGui::EndChild ();
        ImGui::SameLine ();
        ImGui::BeginChild ("##fxsettings", ImVec2 (ImGui::GetContentRegionAvail ().x - buttons_w - 8 * k, -1),
            ImGuiChildFlags_Borders);
        if (style_sel_ == -2)
        {
            blending_options_page ();
        }
        else if (style_sel_ >= 0 && style_sel_ < (int) items.size ())
        {
            if (effect_settings (&items[(size_t) style_sel_]))
            {
                style_dirty_ = true;
            }
            ImGui::Spacing ();
            if (ImGui::Button ("Delete Effect"))
            {
                items.erase (items.begin () + style_sel_);
                style_sel_   = -1;
                style_dirty_ = true;
            }
        }
        else
        {
            ImGui::TextDisabled ("Choose an effect on the left.");
        }
        ImGui::EndChild ();
        ImGui::SameLine ();
        ImGui::BeginGroup ();
        bool ok = ImGui::Button ("OK", ImVec2 (buttons_w, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        bool cancel = ImGui::Button ("Cancel", ImVec2 (buttons_w, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ImGui::Checkbox ("Preview", &style_preview_))
        {
            style_dirty_ = true;
        }
        ImGui::EndGroup ();

        if (style_dirty_)
        {
            style_dirty_ = false;
            if (style_preview_)
            {
                std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
                doc::Layer* l                       = copy->layer_mut (style_layer_);
                if (l != NULL)
                {
                    l->style = style_work_;
                    /* Pattern Overlay draws from the document's patterns: add the chosen one. */
                    for (size_t i = 0; i < style_work_.items.size (); i++)
                    {
                        const doc::Effect& fx = style_work_.items[i];
                        if (fx.kind != doc::FxKind::PatternOverlay)
                        {
                            continue;
                        }
                        std::string name = fx.pattern[0] != 0         ? std::string (fx.pattern)
                                         : session_.patterns.empty () ? ""
                                                                      : session_.patterns[0].name;
                        if (fx.pattern[0] == 0)
                        {
                            snprintf (l->style.items[i].pattern, sizeof l->style.items[i].pattern, "%s", name.c_str ());
                        }
                        if (copy->pattern (name) != NULL)
                        {
                            continue;
                        }
                        for (size_t q = 0; q < session_.patterns.size (); q++)
                        {
                            if (session_.patterns[q].name != name)
                            {
                                continue;
                            }
                            doc::DocPattern dp = {session_.patterns[q].name, session_.patterns[q].width,
                                session_.patterns[q].height, session_.patterns[q].rgba};
                            copy->patterns.push_back (dp);
                        }
                    }
                    l->blend             = style_blend_;
                    l->opacity           = style_opacity_;
                    l->fill_opacity      = style_fill_;
                    l->excluded_channels = 0;
                    for (int c = 0; c < 3; c++)
                    {
                        l->excluded_channels |= style_channels_[c] ? 0u : 1u << c;
                    }
                    l->blend_if = style_blend_if_;
                }
                copy->light_angle    = style_light_[0];
                copy->light_altitude = style_light_[1];
                preview_doc_         = copy;
                preview_serial_++;
            }
            else
            {
                clear_preview ();
            }
        }
        if (ok || cancel)
        {
            show_style_ = false;
            clear_preview ();
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                if (style_light_[0] != d->doc->light_angle || style_light_[1] != d->doc->light_altitude)
                {
                    run_command ("layer.layerStyle.globalLight",
                        engine::Value{{"angle", style_light_[0]}, {"altitude", style_light_[1]}});
                }
                engine::Value list = engine::Value::array ();
                for (size_t i = 0; i < items.size (); i++)
                {
                    list.push_back (engine::effect_json (items[i]));
                }
                run_command ("layer.layerStyle.replace",
                    engine::Value{{"layer", style_layer_}, {"enabled", style_work_.enabled}, {"effects", list}});
                /* Blending Options: the layer's blending, then Blend If. */
                const doc::Layer* cur =
                    session_.active () != NULL ? session_.active ()->doc->layer (style_layer_) : NULL;
                if (cur != NULL)
                {
                    uint32_t excl = 0;
                    for (int c = 0; c < 3; c++)
                    {
                        excl |= style_channels_[c] ? 0u : 1u << c;
                    }
                    if (cur->blend != style_blend_ || cur->opacity != style_opacity_ || cur->fill_opacity != style_fill_
                        || cur->excluded_channels != excl)
                    {
                        engine::Value props = {{"layer", style_layer_}, {"blend", color::blend_label (style_blend_)},
                            {"opacity", style_opacity_}, {"fill", style_fill_},
                            {"channels", {style_channels_[0], style_channels_[1], style_channels_[2]}}};
                        run_command ("layer.setProps", props);
                    }
                    if (memcmp (&cur->blend_if, &style_blend_if_, sizeof style_blend_if_) != 0)
                    {
                        const char* const CH[4] = {"gray", "red", "green", "blue"};
                        engine::Value p         = {{"layer", style_layer_}};
                        for (int c = 0; c < 4; c++)
                        {
                            const doc::BlendRange& r = c == 0 ? style_blend_if_.gray : style_blend_if_.channels[c - 1];
                            p[CH[c]] = {{"this", {r.this_black[0], r.this_black[1], r.this_white[0], r.this_white[1]}},
                                {"underlying",
                                    {r.under_black[0], r.under_black[1], r.under_white[0], r.under_white[1]}}};
                        }
                        run_command ("layer.blendIf", p);
                    }
                }
            }
        }
        ImGui::EndPopup ();
    }

    /* The Blending Options page: General Blending (mode, opacity), Advanced Blending (fill opacity,
       channels) and Blend If. */
    void App::blending_options_page ()
    {
        float k = ui_scale_;
        ImGui::SeparatorText ("General Blending");
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::BeginCombo ("Blend Mode", color::blend_label (style_blend_)))
        {
            for (size_t i = 0; i < color::LAYER_MODE_COUNT; i++)
            {
                color::BlendMode m = color::LAYER_MODES[i];
                if (m == color::BlendMode::PassThrough)
                {
                    continue;
                }
                if (ImGui::Selectable (color::blend_label (m), m == style_blend_))
                {
                    style_blend_ = m;
                    style_dirty_ = true;
                }
            }
            ImGui::EndCombo ();
        }
        float op = style_opacity_ * 100.0f, fill = style_fill_ * 100.0f;
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::SliderFloat ("Opacity", &op, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
        {
            style_opacity_ = op / 100.0f;
            style_dirty_   = true;
        }
        ImGui::SeparatorText ("Advanced Blending");
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::SliderFloat ("Fill Opacity", &fill, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
        {
            style_fill_  = fill / 100.0f;
            style_dirty_ = true;
        }
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Channels:");
        const char* const CH_LABELS[3] = {"R", "G", "B"};
        for (int c = 0; c < 3; c++)
        {
            ImGui::SameLine ();
            if (ImGui::Checkbox (CH_LABELS[c], &style_channels_[c]))
            {
                style_dirty_ = true;
            }
        }
        ImGui::SeparatorText ("Blend If");
        ImGui::SetNextItemWidth (120 * k);
        ImGui::Combo ("##blendifch", &bi_channel_, "Gray\0Red\0Green\0Blue\0");
        doc::BlendRange& r = bi_channel_ == 0 ? style_blend_if_.gray : style_blend_if_.channels[bi_channel_ - 1];
        if (blend_if_bar (0, "This Layer:", r.this_black, r.this_white, bi_channel_))
        {
            style_dirty_ = true;
        }
        if (blend_if_bar (1, "Underlying Layer:", r.under_black, r.under_white, bi_channel_))
        {
            style_dirty_ = true;
        }
        ImGui::TextDisabled ("Alt-drag a slider to split it for a smoother transition.");
    }

    /* Under a styled layer in the Layers panel: "Effects" with an eye for the whole style, then
       each effect with its own eye. Double-click opens the Layer Style dialog on it. */
    void App::effect_rows (const doc::Layer& l, int depth)
    {
        float k = ui_scale_, rh = roundf (24.0f * k), eye_w = 30.0f * k;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float th = ImGui::GetTextLineHeight (), g = 14 * k;
        for (int r = -1; r < (int) l.style.items.size (); r++)
        {
            ImGui::PushID ((int) l.id * 64 + r + 1);
            ImVec2 p = ImGui::GetCursorScreenPos ();
            float w  = ImGui::GetContentRegionAvail ().x;
            bool on  = r < 0 ? l.style.enabled : l.style.items[(size_t) r].enabled;
            dl->AddLine (ImVec2 (p.x, p.y + rh - 1), ImVec2 (p.x + w, p.y + rh - 1), theme_.separator);
            if (ImGui::InvisibleButton ("##fxeye", ImVec2 (eye_w, rh)))
            {
                engine::Value params = {{"layer", l.id}, {"visible", !on}};
                if (r >= 0)
                {
                    params["effect"] = r;
                }
                run_command ("layer.setEffectsVisible", params);
            }
            if (on)
            {
                draw_icon (
                    dl, &icons_, "eye", ImVec2 (p.x + (eye_w - g) * 0.5f, p.y + (rh - g) * 0.5f), g, theme_.icon);
            }
            dl->AddLine (ImVec2 (p.x + eye_w, p.y), ImVec2 (p.x + eye_w, p.y + rh), theme_.separator);
            float x = p.x + eye_w + (float) (depth * 16 + (r < 0 ? 40 : 56)) * k;
            ImGui::SetCursorScreenPos (ImVec2 (p.x + eye_w, p.y));
            ImGui::InvisibleButton ("##fxrow", ImVec2 (w - eye_w, rh));
            if (ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                if (session_.active ()->active_layer != l.id)
                {
                    run_command ("layer.select", engine::Value{{"layer", l.id}});
                }
                open_layer_style (r >= 0 ? &l.style.items[(size_t) r].kind : NULL, r);
            }
            if (r >= 0 && ImGui::BeginPopupContextItem ("##fxctx"))
            {
                if (ImGui::MenuItem ("Edit Effect…"))
                {
                    if (session_.active ()->active_layer != l.id)
                    {
                        run_command ("layer.select", engine::Value{{"layer", l.id}});
                    }
                    open_layer_style (&l.style.items[(size_t) r].kind, r);
                }
                if (ImGui::MenuItem ("Delete Effect"))
                {
                    run_command ("layer.layerStyle.clear", engine::Value{{"layer", l.id}, {"effect", r}});
                }
                ImGui::EndPopup ();
            }
            const char* label = r < 0 ? "Effects" : doc::fx_kind_label (l.style.items[(size_t) r].kind);
            dl->AddText (
                ImVec2 (x, p.y + (rh - th) * 0.5f), on && l.visible ? theme_.text_dim : theme_.text_faint, label);
            ImGui::SetCursorScreenPos (ImVec2 (p.x, p.y + rh));
            ImGui::Dummy (ImVec2 (w, 0));
            ImGui::PopID ();
        }
    }

} // namespace ui
