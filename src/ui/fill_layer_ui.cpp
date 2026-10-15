#include "ui/app.h"
#include <imgui.h>
#include <math.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace ui
{

    /* Fill layers' dialogs (Color Fill, Gradient Fill, Pattern Fill), opened when one is made from
       Layer > New Fill Layer or its thumbnail is double-clicked; and the Blend If bars of the Layer
       Style dialog's Blending Options. The canvas previews the settings on a copy; OK sets them,
       Cancel on a new fill layer removes it again. */

    const char* const FILL_TITLES[3]    = {"Color Fill", "Gradient Fill", "Pattern Fill"};
    const char* const FILL_STYLES       = "Linear\0Radial\0Angle\0Reflected\0Diamond\0";
    const char* const FILL_STYLE_IDS[5] = {"linear", "radial", "angle", "reflected", "diamond"};

    std::string fill_hex (const float c[3])
    {
        char buf[8];
        snprintf (buf, sizeof buf, "#%02x%02x%02x", (int) lroundf (c[0] * 255.0f), (int) lroundf (c[1] * 255.0f),
            (int) lroundf (c[2] * 255.0f));
        return buf;
    }

    /* The fill as layer.setFill's params. */
    engine::Value fill_params (const doc::FillLayer& f)
    {
        const char* const TYPES[3] = {"solid", "gradient", "pattern"};
        engine::Value p            = {{"type", TYPES[(int) f.kind]}};
        if (f.kind == doc::FillKind::Solid)
        {
            p["color"] = fill_hex (f.color);
        }
        else if (f.kind == doc::FillKind::Gradient)
        {
            engine::Value stops = engine::Value::array (), tr = engine::Value::array ();
            for (size_t i = 0; i < f.stops.size (); i++)
            {
                stops.push_back ({f.stops[i].location, fill_hex (f.stops[i].color)});
            }
            for (size_t i = 0; i < f.opacity_stops.size (); i++)
            {
                tr.push_back ({f.opacity_stops[i].input, f.opacity_stops[i].output * 100.0f});
            }
            p["stops"]        = stops;
            p["transparency"] = tr;
            p["style"]        = FILL_STYLE_IDS[(int) f.style];
            p["angle"]        = f.angle;
            p["scale"]        = f.scale * 100.0f;
            p["reverse"]      = f.reverse;
            p["dither"]       = f.dither;
            p["align"]        = f.align;
        }
        else
        {
            p["pattern"] = f.pattern_name;
            p["scale"]   = f.pattern_scale * 100.0f;
            p["angle"]   = f.pattern_angle;
            p["link"]    = f.link;
            p["phase"]   = {f.phase[0], f.phase[1]};
        }
        return p;
    }

    /* Opens the dialog on fill layer `id`; `fresh` when it was just made (Cancel removes it). */
    void App::open_fill_editor (doc::LayerId id, bool fresh)
    {
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (id) : NULL;
        if (l == NULL || l->kind != doc::LayerKind::Fill)
        {
            return;
        }
        engine::ensure_patterns (session_);
        FillEditor& f                            = fill_layer_ed_;
        f.open                                   = true;
        f.fresh                                  = fresh;
        f.layer                                  = id;
        f.work                                   = l->fill;
        f.stop                                   = 0;
        f.dirty                                  = true;
        f.drag                                   = -1;
        f.pattern_index                          = 0;
        const std::vector<engine::Pattern>& pats = session_.patterns;
        for (size_t i = 0; i < pats.size (); i++)
        {
            if (pats[i].name == l->fill.pattern_name)
            {
                f.pattern_index = (int) i;
            }
        }
    }

    /* A gradient's colour at t (the colour stops only), for the editor's bar. */
    void fill_bar_color (const doc::FillLayer& f, float t, float out[3])
    {
        const std::vector<doc::GradientStop>& st = f.stops;
        if (st.empty ())
        {
            out[0] = out[1] = out[2] = t;
            return;
        }
        if (t <= st[0].location)
        {
            memcpy (out, st[0].color, 3 * sizeof (float));
            return;
        }
        for (size_t i = 0; i + 1 < st.size (); i++)
        {
            if (t <= st[i + 1].location)
            {
                float k = (t - st[i].location) / fmaxf (st[i + 1].location - st[i].location, 1e-6f);
                for (int c = 0; c < 3; c++)
                {
                    out[c] = st[i].color[c] + (st[i + 1].color[c] - st[i].color[c]) * k;
                }
                return;
            }
        }
        memcpy (out, st.back ().color, 3 * sizeof (float));
    }

    /* The gradient editor: the bar, colour stops below it (click a stop to choose it, drag to move
       it, click under the bar to add one), the chosen stop's colour and location. True when
       anything changed. */
    bool App::gradient_stops_editor (doc::FillLayer* f)
    {
        FillEditor& e = fill_layer_ed_;
        float k = ui_scale_, w = 360 * k, h = 22 * k;
        ImDrawList* dl  = ImGui::GetWindowDrawList ();
        ImVec2 a        = ImGui::GetCursorScreenPos ();
        const int STEPS = 64;
        for (int i = 0; i < STEPS; i++)
        {
            float c0[3], c1[3];
            fill_bar_color (*f, (float) i / STEPS, c0);
            fill_bar_color (*f, (float) (i + 1) / STEPS, c1);
            ImU32 u0 = ImGui::ColorConvertFloat4ToU32 (ImVec4 (c0[0], c0[1], c0[2], 1.0f)),
                  u1 = ImGui::ColorConvertFloat4ToU32 (ImVec4 (c1[0], c1[1], c1[2], 1.0f));
            dl->AddRectFilledMultiColor (
                ImVec2 (a.x + w * i / STEPS, a.y), ImVec2 (a.x + w * (i + 1) / STEPS, a.y + h), u0, u1, u1, u0);
        }
        dl->AddRect (a, ImVec2 (a.x + w, a.y + h), theme_.card_border);
        /* The stops' strip. */
        float sy = a.y + h + 2 * k, sh = 14 * k;
        ImGui::SetCursorScreenPos (ImVec2 (a.x - 6 * k, sy));
        ImGui::InvisibleButton ("##stops", ImVec2 (w + 12 * k, sh));
        bool changed = false;
        float mx     = ImGui::GetIO ().MousePos.x;
        float t      = fminf (fmaxf ((mx - a.x) / w, 0.0f), 1.0f);
        if (ImGui::IsItemActivated ())
        {
            int near   = -1;
            float best = 7 * k;
            for (size_t i = 0; i < f->stops.size (); i++)
            {
                float d = fabsf (a.x + f->stops[i].location * w - mx);
                if (d <= best)
                {
                    best = d;
                    near = (int) i;
                }
            }
            if (near < 0)
            {
                doc::GradientStop s;
                s.location = t;
                fill_bar_color (*f, t, s.color);
                size_t at = 0;
                while (at < f->stops.size () && f->stops[at].location <= t)
                {
                    at++;
                }
                f->stops.insert (f->stops.begin () + (ptrdiff_t) at, s);
                near    = (int) at;
                changed = true;
            }
            e.stop = near;
            e.drag = near;
        }
        if (ImGui::IsItemActive () && e.drag >= 0 && e.drag < (int) f->stops.size ()
            && ImGui::IsMouseDragging (ImGuiMouseButton_Left, 1.0f))
        {
            f->stops[(size_t) e.drag].location = t;
            changed                            = true;
        }
        if (ImGui::IsItemDeactivated ())
        {
            /* Keep the stops in order, following the dragged one. */
            for (size_t i = 1; i < f->stops.size (); i++)
            {
                for (size_t j = i; j > 0 && f->stops[j].location < f->stops[j - 1].location; j--)
                {
                    std::swap (f->stops[j], f->stops[j - 1]);
                    if (e.stop == (int) j)
                    {
                        e.stop = (int) j - 1;
                    }
                    else if (e.stop == (int) j - 1)
                    {
                        e.stop = (int) j;
                    }
                }
            }
            e.drag = -1;
        }
        for (size_t i = 0; i < f->stops.size (); i++)
        {
            float x = a.x + f->stops[i].location * w;
            ImU32 c = ImGui::ColorConvertFloat4ToU32 (
                ImVec4 (f->stops[i].color[0], f->stops[i].color[1], f->stops[i].color[2], 1.0f));
            dl->AddTriangleFilled (ImVec2 (x, sy), ImVec2 (x - 5 * k, sy + 6 * k), ImVec2 (x + 5 * k, sy + 6 * k),
                (int) i == e.stop ? theme_.text : theme_.text_faint);
            dl->AddRectFilled (ImVec2 (x - 5 * k, sy + 6 * k), ImVec2 (x + 5 * k, sy + sh), c);
            dl->AddRect (ImVec2 (x - 5 * k, sy + 6 * k), ImVec2 (x + 5 * k, sy + sh),
                (int) i == e.stop ? theme_.text : theme_.card_border);
        }
        ImGui::SetCursorScreenPos (ImVec2 (a.x, sy + sh + 6 * k));
        if (e.stop >= 0 && e.stop < (int) f->stops.size ())
        {
            doc::GradientStop& s = f->stops[(size_t) e.stop];
            changed |= ImGui::ColorEdit3 ("Color##stop", s.color, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine (0, 16 * k);
            float loc = s.location * 100.0f;
            ImGui::SetNextItemWidth (140 * k);
            if (ImGui::SliderFloat ("Location##stop", &loc, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            {
                s.location = loc / 100.0f;
                changed    = true;
            }
            ImGui::SameLine (0, 16 * k);
            if (f->stops.size () <= 2)
            {
                ImGui::BeginDisabled ();
            }
            if (ImGui::Button ("Delete##stop"))
            {
                f->stops.erase (f->stops.begin () + e.stop);
                e.stop  = 0;
                changed = true;
            }
            if (f->stops.size () <= 2)
            {
                ImGui::EndDisabled ();
            }
        }
        ImGui::TextDisabled ("Click under the bar to add a stop; drag a stop to move it.");
        return changed;
    }

    void App::fill_editor_dialog ()
    {
        FillEditor& e = fill_layer_ed_;
        if (!e.open)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL || d->doc->layer (e.layer) == NULL)
        {
            e.open = false;
            clear_preview ();
            return;
        }
        float k           = ui_scale_;
        doc::FillLayer& f = e.work;
        char title[64];
        snprintf (title, sizeof title, "%s###filledit", FILL_TITLES[(int) f.kind]);
        ImGui::OpenPopup ("###filledit");
        if (!ImGui::BeginPopupModal (title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        bool changed = false;
        if (f.kind == doc::FillKind::Solid)
        {
            changed |= ImGui::ColorPicker3 ("##fillcolor", f.color, ImGuiColorEditFlags_DisplayHex);
        }
        else if (f.kind == doc::FillKind::Gradient)
        {
            ImGui::TextUnformatted ("Gradient:");
            changed |= gradient_stops_editor (&f);
            ImGui::Separator ();
            int style = (int) f.style;
            ImGui::SetNextItemWidth (160 * k);
            if (ImGui::Combo ("Style", &style, FILL_STYLES))
            {
                f.style = (doc::GradientStyle) style;
                changed = true;
            }
            ImGui::SetNextItemWidth (160 * k);
            changed |= ImGui::SliderFloat ("Angle", &f.angle, -180.0f, 180.0f, "%.0f°", ImGuiSliderFlags_AlwaysClamp);
            float scale = f.scale * 100.0f;
            ImGui::SetNextItemWidth (160 * k);
            if (ImGui::SliderFloat ("Scale", &scale, 10.0f, 150.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            {
                f.scale = scale / 100.0f;
                changed = true;
            }
            changed |= ImGui::Checkbox ("Reverse", &f.reverse);
            ImGui::SameLine ();
            changed |= ImGui::Checkbox ("Dither", &f.dither);
            ImGui::SameLine ();
            changed |= ImGui::Checkbox ("Align with layer", &f.align);
        }
        else
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Pattern:");
            ImGui::SameLine ();
            if (pattern_combo ("##fillpattern", &e.pattern_index, 180 * k) && e.pattern_index >= 0
                && (size_t) e.pattern_index < session_.patterns.size ())
            {
                const engine::Pattern& pt = session_.patterns[(size_t) e.pattern_index];
                f.pattern_name            = pt.name;
                f.pattern_w               = pt.width;
                f.pattern_h               = pt.height;
                f.pattern_rgba            = pt.rgba;
                changed                   = true;
            }
            float scale = f.pattern_scale * 100.0f;
            ImGui::SetNextItemWidth (160 * k);
            if (ImGui::SliderFloat ("Scale", &scale, 1.0f, 1000.0f, "%.0f%%",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
            {
                f.pattern_scale = scale / 100.0f;
                changed         = true;
            }
            ImGui::SetNextItemWidth (160 * k);
            changed |=
                ImGui::SliderFloat ("Angle", &f.pattern_angle, -180.0f, 180.0f, "%.0f°", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::Checkbox ("Link with Layer", &f.link);
            ImGui::SameLine ();
            if (ImGui::Button ("Snap to Origin"))
            {
                f.phase[0] = f.phase[1] = 0.0f;
                changed                 = true;
            }
        }
        if (changed)
        {
            e.dirty = true;
        }
        if (e.dirty && !ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            e.dirty                             = false;
            std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
            doc::Layer* l                       = copy->layer_mut (e.layer);
            if (l != NULL)
            {
                l->fill = f;
            }
            preview_doc_ = copy;
            preview_serial_++;
        }
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            e.open = false;
            clear_preview ();
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                engine::Value p = fill_params (f);
                p["layer"]      = e.layer;
                run_command ("layer.setFill", p);
            }
            else if (e.fresh)
            {
                run_command ("edit.undo", engine::Value::object ());
            }
        }
        ImGui::EndPopup ();
    }

    /* ---- Blend If ---- */

    /* One Blend If bar: a ramp (black to white, or to the channel's colour) with the black and
       white handles under it. Drag a handle to move it (both halves); Alt-drag one half to split
       it. `bar` tells the bars apart for dragging. True when a value changed. */
    bool App::blend_if_bar (int bar, const char* label, float black[2], float white[2], int channel)
    {
        float k = ui_scale_, w = 300 * k, h = 12 * k, hh = 10 * k;
        ImGui::TextUnformatted (label);
        ImGui::SameLine (110 * k);
        char nums[48];
        if (black[0] == black[1] && white[0] == white[1])
        {
            snprintf (nums, sizeof nums, "%.0f   %.0f", black[0], white[1]);
        }
        else
        {
            snprintf (nums, sizeof nums, "%.0f / %.0f   %.0f / %.0f", black[0], black[1], white[0], white[1]);
        }
        ImGui::TextDisabled ("%s", nums);
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 a       = ImGui::GetCursorScreenPos ();
        ImVec4 end     = channel == 1 ? ImVec4 (1, 0, 0, 1)
                       : channel == 2 ? ImVec4 (0, 1, 0, 1)
                       : channel == 3 ? ImVec4 (0, 0, 1, 1)
                                      : ImVec4 (1, 1, 1, 1);
        ImU32 c0 = IM_COL32 (0, 0, 0, 255), c1 = ImGui::ColorConvertFloat4ToU32 (end);
        dl->AddRectFilledMultiColor (a, ImVec2 (a.x + w, a.y + h), c0, c1, c1, c0);
        dl->AddRect (a, ImVec2 (a.x + w, a.y + h), theme_.card_border);
        ImGui::SetCursorScreenPos (ImVec2 (a.x - 6 * k, a.y + h));
        char id[24];
        snprintf (id, sizeof id, "##blendif%d", bar);
        ImGui::InvisibleButton (id, ImVec2 (w + 12 * k, hh + 2 * k));
        float mx = ImGui::GetIO ().MousePos.x, v = fminf (fmaxf ((mx - a.x) / w * 255.0f, 0.0f), 255.0f);
        float* vals[4] = {&black[0], &black[1], &white[0], &white[1]};
        bool changed   = false;
        if (ImGui::IsItemActivated ())
        {
            int best = -1;
            float bd = 1e9f;
            for (int i = 0; i < 4; i++)
            {
                float d = fabsf (a.x + *vals[i] / 255.0f * w - mx);
                /* Equal halves: pick by which side the pointer is on. */
                if (d < bd - 0.5f || (fabsf (d - bd) <= 0.5f && ((i % 2 == 1) == (mx > a.x + *vals[i] / 255.0f * w))))
                {
                    bd   = d;
                    best = i;
                }
            }
            bi_drag_bar_    = bar;
            bi_drag_handle_ = best;
            bi_drag_split_  = ImGui::GetIO ().KeyAlt;
            bi_drag_from_   = v;
            memcpy (bi_drag_start_, black, 2 * sizeof (float));
            memcpy (bi_drag_start_ + 2, white, 2 * sizeof (float));
        }
        if (ImGui::IsItemActive () && bi_drag_bar_ == bar && bi_drag_handle_ >= 0)
        {
            float s[4];
            memcpy (s, bi_drag_start_, sizeof s);
            float dv = v - bi_drag_from_;
            int hd = bi_drag_handle_, pair = hd / 2;
            if (bi_drag_split_)
            {
                s[hd] = fminf (fmaxf (s[hd] + dv, 0.0f), 255.0f);
            }
            else
            {
                s[pair * 2]     = fminf (fmaxf (s[pair * 2] + dv, 0.0f), 255.0f);
                s[pair * 2 + 1] = fminf (fmaxf (s[pair * 2 + 1] + dv, 0.0f), 255.0f);
            }
            /* Keep black <= its split <= white's split <= white. */
            s[1] = fmaxf (s[1], s[0]);
            s[3] = fmaxf (s[3], s[2]);
            if (pair == 0)
            {
                s[2] = fmaxf (s[2], s[1]);
                s[3] = fmaxf (s[3], s[2]);
            }
            else
            {
                s[1] = fminf (s[1], s[2]);
                s[0] = fminf (s[0], s[1]);
            }
            for (int i = 0; i < 4; i++)
            {
                if (*vals[i] != s[i])
                {
                    changed = true;
                }
                *vals[i] = s[i];
            }
        }
        if (ImGui::IsItemDeactivated () && bi_drag_bar_ == bar)
        {
            bi_drag_bar_ = -1;
        }
        for (int i = 0; i < 4; i++)
        {
            float x = a.x + *vals[i] / 255.0f * w, y = a.y + h;
            bool split = i < 2 ? black[0] != black[1] : white[0] != white[1];
            float half = split ? 4 * k : 5 * k;
            ImVec2 p0 (x, y), p1 (x - half, y + hh), p2 (x + half, y + hh);
            if (split)
            {
                /* Half triangles show the split. */
                if (i % 2 == 0)
                {
                    p2 = ImVec2 (x, y + hh);
                }
                else
                {
                    p1 = ImVec2 (x, y + hh);
                }
            }
            dl->AddTriangleFilled (p0, p1, p2, i < 2 ? IM_COL32 (20, 20, 20, 255) : IM_COL32 (245, 245, 245, 255));
            dl->AddTriangle (p0, p1, p2, theme_.text_faint);
        }
        ImGui::SetCursorScreenPos (ImVec2 (a.x, a.y + h + hh + 6 * k));
        ImGui::Dummy (ImVec2 (w, 0));
        return changed;
    }

} // namespace ui
