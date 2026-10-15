#include "compose/masks.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <GL/gl.h>
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The Layers panel, as Photoshop lays it out: blend mode and opacity, then locks and fill;
       rows with the eye, the layer thumbnail (and its mask's), the name and badges; buttons along
       the bottom. Thumbnails are rendered small from the layer's pixels and redone only when its
       tiles change. */

    void checkerboard (ImDrawList* dl, ImVec2 a, ImVec2 b, float cell)
    {
        dl->AddRectFilled (a, b, IM_COL32 (255, 255, 255, 255));
        for (float y = a.y; y < b.y; y += cell)
        {
            for (float x = a.x; x < b.x; x += cell)
            {
                bool dark = ((int) ((x - a.x) / cell) + (int) ((y - a.y) / cell)) % 2 == 1;
                if (dark)
                {
                    dl->AddRectFilled (ImVec2 (x, y), ImVec2 (fminf (x + cell, b.x), fminf (y + cell, b.y)),
                        IM_COL32 (204, 204, 204, 255));
                }
            }
        }
    }

    /* The layer's (or its mask's) thumbnail texture, w x h pixels for the document's aspect inside
       a px square. */
    unsigned int App::thumbnail (const doc::Document& d, const doc::Layer& l, bool mask, int px, int* tw, int* th)
    {
        float dw = (float) d.size.width, dh = (float) d.size.height;
        float scale                = (float) px / fmaxf (dw, dh);
        *tw                        = (int) fmaxf (1.0f, roundf (dw * scale));
        *th                        = (int) fmaxf (1.0f, roundf (dh * scale));
        const raster::Surface* src = mask ? &l.mask.surface : l.surface ();
        if (src == NULL)
        {
            return 0;
        }
        for (size_t i = 0; i < thumbs_.size (); i++)
        {
            Thumb& t = thumbs_[i];
            if (t.id != l.id || t.mask != mask)
            {
                continue;
            }
            if (t.w == *tw && t.h == *th && compose::same_tiles (t.src, *src))
            {
                return t.tex;
            }
            glDeleteTextures (1, &t.tex);
            thumbs_.erase (thumbs_.begin () + (ptrdiff_t) i);
            break;
        }
        if (thumbs_.size () > 256)
        {
            for (size_t i = 0; i < thumbs_.size (); i++)
            {
                glDeleteTextures (1, &thumbs_[i].tex);
            }
            thumbs_.clear ();
        }
        /* Four samples per thumbnail pixel, averaged with alpha weighting. */
        std::vector<uint8_t> rgba ((size_t) *tw * *th * 4);
        float step = 1.0f / scale;
        for (int y = 0; y < *th; y++)
        {
            for (int x = 0; x < *tw; x++)
            {
                float acc[4] = {0, 0, 0, 0};
                for (int s = 0; s < 4; s++)
                {
                    int32_t sx = (int32_t) (((float) x + 0.25f + 0.5f * (float) (s & 1)) * step),
                            sy = (int32_t) (((float) y + 0.25f + 0.5f * (float) (s >> 1)) * step);
                    float c[4];
                    if (mask)
                    {
                        float v = src->sample_channel (sx, sy, 0);
                        c[0] = c[1] = c[2] = v;
                        c[3]               = 1.0f;
                    }
                    else
                    {
                        src->rgba (sx, sy, session_.cmyk, c);
                    }
                    for (int k = 0; k < 3; k++)
                    {
                        acc[k] += c[k] * c[3];
                    }
                    acc[3] += c[3];
                }
                uint8_t* o = &rgba[((size_t) y * *tw + x) * 4];
                for (int k = 0; k < 3; k++)
                {
                    o[k] = (uint8_t) lroundf (fminf (acc[3] > 0 ? acc[k] / acc[3] : 0.0f, 1.0f) * 255.0f);
                }
                o[3] = (uint8_t) lroundf (fminf (acc[3] / 4.0f, 1.0f) * 255.0f);
            }
        }
        Thumb t;
        t.id   = l.id;
        t.mask = mask;
        t.src  = *src;
        t.w    = *tw;
        t.h    = *th;
        glGenTextures (1, &t.tex);
        glBindTexture (GL_TEXTURE_2D, t.tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, *tw, *th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
        thumbs_.push_back (t);
        return t.tex;
    }

    /* A thumbnail box: the image on a checkerboard (or an icon for layers without pixels), framed
       when it is the paint target. */
    void App::thumb_box (const doc::Document& d, const doc::Layer& l, bool mask, ImVec2 a, float size, bool target)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float k        = ui_scale_;
        ImVec2 b (a.x + size, a.y + size);
        const raster::Surface* src = mask ? &l.mask.surface : l.surface ();
        if (!mask && l.kind == doc::LayerKind::Text)
        {
            src = NULL;
        }
        if (!mask && l.kind == doc::LayerKind::Fill)
        {
            /* A fill layer shows its fill: the colour, the gradient's colours, or the pattern's. */
            const doc::FillLayer& f = l.fill;
            if (f.kind == doc::FillKind::Solid)
            {
                dl->AddRectFilled (
                    a, b, ImGui::ColorConvertFloat4ToU32 (ImVec4 (f.color[0], f.color[1], f.color[2], 1.0f)));
            }
            else if (f.kind == doc::FillKind::Gradient && !f.stops.empty ())
            {
                const int STEPS = 8;
                for (int i = 0; i < STEPS; i++)
                {
                    float t0 = (float) i / STEPS, t1 = (float) (i + 1) / STEPS;
                    const doc::GradientStop *s0 = &f.stops[0], *s1 = &f.stops.back ();
                    ImU32 c0, c1;
                    float c[2][3];
                    for (int e = 0; e < 2; e++)
                    {
                        float t = e == 0 ? t0 : t1;
                        memcpy (c[e], s0->color, sizeof c[e]);
                        for (size_t q = 0; q + 1 < f.stops.size (); q++)
                        {
                            if (t >= f.stops[q].location && t <= f.stops[q + 1].location)
                            {
                                float kk = (t - f.stops[q].location)
                                         / fmaxf (f.stops[q + 1].location - f.stops[q].location, 1e-6f);
                                for (int ch = 0; ch < 3; ch++)
                                {
                                    c[e][ch] =
                                        f.stops[q].color[ch] + (f.stops[q + 1].color[ch] - f.stops[q].color[ch]) * kk;
                                }
                            }
                        }
                        if (t > s1->location)
                        {
                            memcpy (c[e], s1->color, sizeof c[e]);
                        }
                    }
                    c0 = ImGui::ColorConvertFloat4ToU32 (ImVec4 (c[0][0], c[0][1], c[0][2], 1.0f));
                    c1 = ImGui::ColorConvertFloat4ToU32 (ImVec4 (c[1][0], c[1][1], c[1][2], 1.0f));
                    dl->AddRectFilledMultiColor (
                        ImVec2 (a.x + size * t0, a.y), ImVec2 (a.x + size * t1, b.y), c0, c1, c1, c0);
                }
            }
            else
            {
                dl->AddRectFilled (a, b, theme_.field, theme_.radius_sm);
                float g = size * 0.6f;
                draw_icon (
                    dl, &icons_, "grid-3x3", ImVec2 (a.x + (size - g) * 0.5f, a.y + (size - g) * 0.5f), g, theme_.icon);
            }
            dl->AddRect (a, b, theme_.card_border);
            return;
        }
        if (src == NULL)
        {
            dl->AddRectFilled (a, b, theme_.field, theme_.radius_sm);
            const char* icon = l.is_group ()                        ? "folder"
                             : l.kind == doc::LayerKind::Adjustment ? "adjustment-layer"
                             : l.kind == doc::LayerKind::Text       ? "type"
                                                                    : "image";
            float g          = size * 0.6f;
            draw_icon (dl, &icons_, icon, ImVec2 (a.x + (size - g) * 0.5f, a.y + (size - g) * 0.5f), g, theme_.icon);
        }
        else
        {
            int tw, th;
            unsigned int tex = thumbnail (d, l, mask, (int) size, &tw, &th);
            ImVec2 ia (a.x + floorf ((size - (float) tw) * 0.5f), a.y + floorf ((size - (float) th) * 0.5f));
            ImVec2 ib (ia.x + (float) tw, ia.y + (float) th);
            if (!mask)
            {
                checkerboard (dl, ia, ib, 4.0f * k);
            }
            if (tex != 0)
            {
                dl->AddImage ((ImTextureID) (intptr_t) tex, ia, ib);
            }
            dl->AddRect (ImVec2 (ia.x - 1, ia.y - 1), ImVec2 (ib.x + 1, ib.y + 1), theme_.card_border);
            if (mask && !l.mask.enabled)
            {
                dl->AddLine (ia, ib, IM_COL32 (220, 40, 40, 255), 2.0f * k);
                dl->AddLine (ImVec2 (ib.x, ia.y), ImVec2 (ia.x, ib.y), IM_COL32 (220, 40, 40, 255), 2.0f * k);
            }
            if (target)
            {
                dl->AddRect (ImVec2 (ia.x - 3 * k, ia.y - 3 * k), ImVec2 (ib.x + 3 * k, ib.y + 3 * k), theme_.text,
                    2.0f * k, 0, 1.5f * k);
            }
        }
        if (!mask && l.kind == doc::LayerKind::Smart)
        {
            /* The smart object badge in the corner. */
            float g = roundf (size * 0.42f);
            ImVec2 at (b.x - g + 1 * k, b.y - g + 1 * k);
            dl->AddRectFilled (at, ImVec2 (at.x + g, at.y + g), theme_.chrome, 2.0f * k);
            draw_icon (dl, &icons_, "file", ImVec2 (at.x + 1 * k, at.y + 1 * k), g - 2 * k, theme_.text);
        }
    }

    /* One row: eye (Alt-click shows only this layer), indentation and group disclosure, the clip
       arrow, thumbnails, name (click selects, Ctrl-click toggles, Shift-click extends, double-click
       renames, drag reorders, Alt-drag copies), badges. Clicking the mask thumbnail paints on the
       mask; Shift-click disables it. */
    void App::layer_row (const doc::Document& d, const doc::Layer& l, int depth)
    {
        const engine::DocState* st = session_.active ();
        ImGuiIO& io                = ImGui::GetIO ();
        float k                    = ui_scale_;
        float rh = roundf (36.0f * k), thumb = roundf (28.0f * k);
        ImGui::PushID ((int) l.id);
        ImVec2 p       = ImGui::GetCursorScreenPos ();
        float w        = ImGui::GetContentRegionAvail ().x;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        bool selected  = st->is_selected (l.id);
        bool on_mask   = l.has_mask && mask_target_layer_ == l.id && st->active_layer == l.id;
        if (selected)
        {
            dl->AddRectFilled (p, ImVec2 (p.x + w, p.y + rh), theme_.row_selected);
        }
        dl->AddLine (ImVec2 (p.x, p.y + rh - 1), ImVec2 (p.x + w, p.y + rh - 1), theme_.separator);

        /* The eye column. */
        float eye_w = 26.0f * k;
        ImGui::SetCursorScreenPos (p);
        if (ImGui::InvisibleButton ("##eye", ImVec2 (eye_w, rh)))
        {
            if (io.KeyAlt)
            {
                run_command ("layer.showOnly", engine::Value{{"layer", l.id}});
            }
            else
            {
                run_command ("layer.setProps", engine::Value{{"layer", l.id}, {"visible", !l.visible}});
            }
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Show/hide (Alt-click: show only this layer)");
        }
        float g = 16.0f * k;
        if (l.visible)
        {
            draw_icon (dl, &icons_, "eye", ImVec2 (p.x + (eye_w - g) * 0.5f, p.y + (rh - g) * 0.5f), g, theme_.icon);
        }
        dl->AddLine (ImVec2 (p.x + eye_w, p.y), ImVec2 (p.x + eye_w, p.y + rh), theme_.separator);

        float x = p.x + eye_w + 6 * k + 16.0f * k * (float) depth;
        if (l.is_group ())
        {
            ImGui::SetCursorScreenPos (ImVec2 (x, p.y));
            if (ImGui::InvisibleButton ("##expand", ImVec2 (14 * k, rh)))
            {
                run_command ("layer.setExpanded", engine::Value{{"layer", l.id}, {"expanded", !l.expanded}});
            }
            float a = 10.0f * k;
            draw_icon (dl, &icons_, l.expanded ? "chevron-down" : "chevron-right",
                ImVec2 (x + 2 * k, p.y + (rh - a) * 0.5f), a, theme_.icon);
            x += 16 * k;
        }
        if (l.clipped)
        {
            float a = 12.0f * k;
            draw_icon (dl, &icons_, "clip-below", ImVec2 (x, p.y + (rh - a) * 0.5f), a, theme_.icon);
            x += 16 * k;
        }
        float ty = p.y + (rh - thumb) * 0.5f;
        if (l.kind == doc::LayerKind::Fill)
        {
            /* Double-clicking a fill layer's thumbnail opens its settings. */
            ImGui::SetCursorScreenPos (ImVec2 (x, ty));
            ImGui::InvisibleButton ("##fillthumb", ImVec2 (thumb, thumb));
            if (ImGui::IsItemClicked (ImGuiMouseButton_Left))
            {
                if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                {
                    open_fill_editor (l.id, false);
                }
                else if (st->active_layer != l.id)
                {
                    run_command ("layer.select", engine::Value{{"layer", l.id}});
                }
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("Fill layer. Double-click to change its fill.");
            }
        }
        if (l.kind == doc::LayerKind::Smart)
        {
            /* Double-clicking a smart object's thumbnail opens its contents, as in Photoshop. */
            ImGui::SetCursorScreenPos (ImVec2 (x, ty));
            ImGui::InvisibleButton ("##smartthumb", ImVec2 (thumb, thumb));
            if (ImGui::IsItemClicked (ImGuiMouseButton_Left))
            {
                if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                {
                    run_command ("layer.smartObject.editContents", engine::Value{{"layer", l.id}});
                }
                else if (st->active_layer != l.id)
                {
                    run_command ("layer.select", engine::Value{{"layer", l.id}});
                }
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("Smart object. Double-click to edit its contents.");
            }
        }
        thumb_box (d, l, false, ImVec2 (x, ty), thumb, selected && !on_mask && l.has_mask);
        x += thumb + 4 * k;
        if (l.has_mask)
        {
            if (l.mask.linked)
            {
                float a = 10.0f * k;
                draw_icon (dl, &icons_, "link", ImVec2 (x, p.y + (rh - a) * 0.5f), a, theme_.text_faint);
            }
            x += 12 * k;
            ImGui::SetCursorScreenPos (ImVec2 (x, ty));
            if (ImGui::InvisibleButton ("##mask", ImVec2 (thumb, thumb)))
            {
                if (io.KeyShift)
                {
                    run_command ("layer.layerMask.enabled", engine::Value{{"layer", l.id}});
                }
                else
                {
                    if (st->active_layer != l.id)
                    {
                        run_command ("layer.select", engine::Value{{"layer", l.id}});
                    }
                    mask_target_layer_ = l.id;
                }
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("Layer mask. Click: paint on the mask. Shift-click: disable/enable it.");
            }
            thumb_box (d, l, true, ImVec2 (x, ty), thumb, on_mask);
            x += thumb + 4 * k;
        }
        if (l.has_vector_mask)
        {
            if (l.vector_mask.linked)
            {
                float a = 10.0f * k;
                draw_icon (dl, &icons_, "link", ImVec2 (x, p.y + (rh - a) * 0.5f), a, theme_.text_faint);
            }
            x += 12 * k;
            ImGui::SetCursorScreenPos (ImVec2 (x, ty));
            if (ImGui::InvisibleButton ("##vmask", ImVec2 (thumb, thumb)))
            {
                if (io.KeyShift)
                {
                    run_command ("layer.vectorMask.enabled", engine::Value{{"layer", l.id}});
                }
                else
                {
                    if (st->active_layer != l.id)
                    {
                        run_command ("layer.select", engine::Value{{"layer", l.id}});
                    }
                    path_sel_          = -3;
                    path_hidden_layer_ = doc::NO_LAYER;
                }
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("Vector mask. Click: show its path. Shift-click: disable/enable it.");
            }
            path_thumb (d, l.vector_mask.path, ImVec2 (x, ty), thumb);
            if (!l.vector_mask.enabled)
            {
                ImVec2 b (x + thumb, ty + thumb);
                dl->AddLine (ImVec2 (x, ty), b, IM_COL32 (220, 40, 40, 255), 2.0f * k);
                dl->AddLine (ImVec2 (b.x, ty), ImVec2 (x, b.y), IM_COL32 (220, 40, 40, 255), 2.0f * k);
            }
            x += thumb + 4 * k;
        }
        x += 6 * k;

        /* Badges on the right: lock and link. */
        float right = p.x + w - 8 * k;
        float bsz   = 12.0f * k;
        if (l.locks.all || l.locks.pixels || l.locks.position || l.locks.transparency)
        {
            right -= bsz;
            draw_icon (dl, &icons_, "lock", ImVec2 (right, p.y + (rh - bsz) * 0.5f), bsz,
                l.locks.all ? theme_.text : theme_.text_faint);
            right -= 6 * k;
        }
        if (!l.style.items.empty ())
        {
            /* "fx" with a chevron that shows or hides the effects under the layer. */
            bool open = std::find (fx_collapsed_.begin (), fx_collapsed_.end (), l.id) == fx_collapsed_.end ();
            float fw  = ImGui::CalcTextSize ("fx").x;
            right -= fw + bsz + 2 * k;
            ImGui::SetCursorScreenPos (ImVec2 (right, p.y));
            if (ImGui::InvisibleButton ("##fxtoggle", ImVec2 (fw + bsz + 2 * k, rh)))
            {
                if (open)
                {
                    fx_collapsed_.push_back (l.id);
                }
                else
                {
                    fx_collapsed_.erase (std::find (fx_collapsed_.begin (), fx_collapsed_.end (), l.id));
                }
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("Show or hide the layer's effects");
            }
            ImGui::PushFont (fonts_.semibold, 0.0f);
            dl->AddText (ImVec2 (right, p.y + (rh - ImGui::GetTextLineHeight ()) * 0.5f),
                l.style.enabled ? theme_.text : theme_.text_faint, "fx");
            ImGui::PopFont ();
            draw_icon (dl, &icons_, open ? "chevron-down" : "chevron-right",
                ImVec2 (right + fw + 2 * k, p.y + (rh - bsz) * 0.5f), bsz, theme_.icon);
            right -= 6 * k;
        }
        if (l.link_group != 0)
        {
            right -= bsz;
            draw_icon (dl, &icons_, "link", ImVec2 (right, p.y + (rh - bsz) * 0.5f), bsz, theme_.icon);
            right -= 6 * k;
        }

        /* The name: the rest of the row, which is also where selection, rename and drag happen. */
        float name_w = fmaxf (right - x, 20.0f * k);
        float th     = ImGui::GetTextLineHeight ();
        if (rename_layer_ == l.id)
        {
            ImGui::SetCursorScreenPos (ImVec2 (x, p.y + (rh - ImGui::GetFrameHeight ()) * 0.5f));
            ImGui::SetNextItemWidth (name_w);
            if (rename_focus_)
            {
                ImGui::SetKeyboardFocusHere ();
                rename_focus_ = false;
            }
            bool enter = ImGui::InputText ("##rename", rename_buf_, sizeof rename_buf_,
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (enter || (ImGui::IsItemDeactivated () && !ImGui::IsKeyPressed (ImGuiKey_Escape)))
            {
                if (strcmp (rename_buf_, l.name.c_str ()) != 0)
                {
                    run_command ("layer.renameLayer", engine::Value{{"layer", l.id}, {"name", rename_buf_}});
                }
                rename_layer_ = doc::NO_LAYER;
            }
            else if (ImGui::IsItemDeactivated ())
            {
                rename_layer_ = doc::NO_LAYER;
            }
        }
        else
        {
            ImGui::SetCursorScreenPos (ImVec2 (x - 4 * k, p.y));
            ImGui::InvisibleButton ("##name", ImVec2 (name_w + 4 * k, rh));
            bool clicked = ImGui::IsItemClicked (ImGuiMouseButton_Left);
            if (clicked && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                rename_layer_ = l.id;
                rename_focus_ = true;
                snprintf (rename_buf_, sizeof rename_buf_, "%s", l.name.c_str ());
            }
            else if (clicked)
            {
                const char* mode = io.KeyCtrl ? "toggle" : io.KeyShift ? "range" : "replace";
                run_command ("layer.select", engine::Value{{"layer", l.id}, {"mode", mode}});
                if (strcmp (mode, "replace") == 0)
                {
                    mask_target_layer_ = doc::NO_LAYER;
                }
            }
            if (ImGui::BeginPopupContextItem ("##context"))
            {
                if (!selected)
                {
                    run_command ("layer.select", engine::Value{{"layer", l.id}});
                }
                layer_context_menu ();
                ImGui::EndPopup ();
            }
            if (ImGui::BeginDragDropSource ())
            {
                doc::LayerId id = l.id;
                ImGui::SetDragDropPayload ("PC_LAYER", &id, sizeof id);
                std::vector<doc::LayerId> sel = st->selected ();
                if (selected && sel.size () > 1)
                {
                    ImGui::Text ("%zu layers", sel.size ());
                }
                else
                {
                    ImGui::TextUnformatted (l.name.c_str ());
                }
                ImGui::EndDragDropSource ();
            }
            if (ImGui::BeginDragDropTarget ())
            {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload ("PC_LAYER");
                if (payload != NULL)
                {
                    doc::LayerId src;
                    memcpy (&src, payload->Data, sizeof src);
                    /* The upper part of a row drops above it, the lower part below; the middle of a
                       group drops into it. */
                    float t         = (io.MousePos.y - p.y) / rh;
                    const char* pos = l.is_group () && t > 0.3f && t < 0.7f ? "into" : t < 0.5f ? "above" : "below";
                    engine::Value params          = {{"target", l.id}, {"position", pos}, {"copy", io.KeyAlt}};
                    std::vector<doc::LayerId> sel = st->selected ();
                    if (st->is_selected (src) && sel.size () > 1)
                    {
                        params["layers"] = sel;
                    }
                    else
                    {
                        params["layer"] = src;
                    }
                    run_command ("layer.moveTo", params);
                }
                ImGui::EndDragDropTarget ();
            }
            ImGui::PushClipRect (ImVec2 (x, p.y), ImVec2 (x + name_w, p.y + rh), true);
            dl->AddText (
                ImVec2 (x, p.y + (rh - th) * 0.5f), l.visible ? theme_.text : theme_.text_faint, l.name.c_str ());
            ImGui::PopClipRect ();
        }
        ImGui::SetCursorScreenPos (ImVec2 (p.x, p.y + rh));
        ImGui::Dummy (ImVec2 (w, 0));
        ImGui::PopID ();
    }

    void App::layer_rows (const doc::Document& d, const std::vector<doc::Layer>& layers, int depth)
    {
        for (size_t i = layers.size (); i-- > 0;)
        {
            layer_row (d, layers[i], depth);
            const doc::Layer& l = layers[i];
            if (l.kind == doc::LayerKind::Smart && !l.smart.filters.empty ())
            {
                smart_filter_rows (l, depth);
            }
            if (!l.style.items.empty ()
                && std::find (fx_collapsed_.begin (), fx_collapsed_.end (), l.id) == fx_collapsed_.end ())
            {
                effect_rows (l, depth);
            }
            if (l.is_group () && l.expanded)
            {
                layer_rows (d, l.children, depth + 1);
            }
        }
    }

    /* Blend mode and opacity, then the locks and fill, as at the top of Photoshop's panel. */
    void App::layer_options (const doc::Document& d)
    {
        const engine::DocState* st = session_.active ();
        const doc::Layer* active   = st->active_layer != doc::NO_LAYER ? d.layer (st->active_layer) : NULL;
        float k                    = ui_scale_;
        if (active == NULL)
        {
            ImGui::BeginDisabled ();
        }
        const char* current = active != NULL ? color::blend_label (active->blend) : "Normal";
        float w             = ImGui::GetContentRegionAvail ().x;
        ImGui::SetNextItemWidth (fmaxf (w - 150 * k, 90 * k));
        if (ImGui::BeginCombo ("##blend", current))
        {
            if (active != NULL && active->is_group ()
                && ImGui::Selectable ("Pass Through", active->blend == color::BlendMode::PassThrough))
            {
                run_command ("layer.setProps", engine::Value{{"blend", "Pass Through"}});
            }
            for (size_t i = 0; i < color::LAYER_MODE_COUNT; i++)
            {
                color::BlendMode m = color::LAYER_MODES[i];
                if (ImGui::Selectable (color::blend_label (m), active != NULL && m == active->blend))
                {
                    run_command ("layer.setProps", engine::Value{{"blend", color::blend_label (m)}});
                }
            }
            ImGui::EndCombo ();
        }
        opt_gap ();
        opt_label ("Opacity:");
        float opacity = opacity_editing_ ? opacity_edit_ : active != NULL ? active->opacity * 100.0f : 100.0f;
        ImGui::SetNextItemWidth (-1);
        ImGui::DragFloat ("##opacity", &opacity, 0.4f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        opacity_editing_ = ImGui::IsItemActive ();
        opacity_edit_    = opacity;
        if (ImGui::IsItemDeactivatedAfterEdit ())
        {
            run_command ("layer.setProps", engine::Value{{"opacity", opacity / 100.0f}});
        }

        opt_label ("Lock:");
        const char* names[4] = {"transparency", "pixels", "position", "all"};
        const char* icons[4] = {"grid-2x2", "brush", "move", "lock"};
        const char* tips[4]  = {"Lock transparent pixels  (/)", "Lock image pixels", "Lock position", "Lock all"};
        bool values[4]       = {false, false, false, false};
        if (active != NULL)
        {
            values[0] = active->locks.transparency;
            values[1] = active->locks.pixels;
            values[2] = active->locks.position;
            values[3] = active->locks.all;
        }
        float box = ImGui::GetFrameHeight ();
        for (int i = 0; i < 4; i++)
        {
            char id[16];
            snprintf (id, sizeof id, "##lock%d", i);
            if (icon_button (id, icons[i], box, values[i], tips[i]))
            {
                run_command ("layer.setProps", engine::Value{{"locks", {{names[i], !values[i]}}}});
            }
            ImGui::SameLine (0, 2 * k);
        }
        opt_gap ();
        opt_label ("Fill:");
        float fill = fill_editing_ ? fill_edit_ : active != NULL ? active->fill_opacity * 100.0f : 100.0f;
        ImGui::SetNextItemWidth (-1);
        ImGui::DragFloat ("##fill", &fill, 0.4f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        fill_editing_ = ImGui::IsItemActive ();
        fill_edit_    = fill;
        if (ImGui::IsItemDeactivatedAfterEdit ())
        {
            run_command ("layer.setProps", engine::Value{{"fill", fill / 100.0f}});
        }
        if (active == NULL)
        {
            ImGui::EndDisabled ();
        }
    }

    /* The buttons along the bottom: link, add mask, new adjustment layer, new group, new layer,
       delete. */
    void App::layer_buttons (const doc::Document& d)
    {
        float k = ui_scale_, box = 24.0f * k;
        float total = 7 * box + 6 * 4 * k;
        ImGui::SetCursorPosX (ImGui::GetCursorPosX () + fmaxf (ImGui::GetContentRegionAvail ().x - total, 0.0f));
        if (icon_button ("##link", "link", box, false, "Link Layers"))
        {
            run_command ("layer.linkLayers", engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##fx", "sparkles", box, false, "Add a layer style"))
        {
            ImGui::OpenPopup ("addfx");
        }
        if (ImGui::BeginPopup ("addfx"))
        {
            const doc::FxKind order[10] = {doc::FxKind::BevelEmboss, doc::FxKind::Stroke, doc::FxKind::InnerShadow,
                doc::FxKind::InnerGlow, doc::FxKind::Satin, doc::FxKind::ColorOverlay, doc::FxKind::GradientOverlay,
                doc::FxKind::PatternOverlay, doc::FxKind::OuterGlow, doc::FxKind::DropShadow};
            for (int i = 0; i < 10; i++)
            {
                char label[64];
                snprintf (label, sizeof label, "%s…", doc::fx_kind_label (order[i]));
                if (ImGui::MenuItem (label))
                {
                    open_layer_style (&order[i], -1);
                }
            }
            ImGui::EndPopup ();
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button (
                "##addmask", "layer-mask", box, false, "Add Layer Mask (from the selection when there is one)"))
        {
            run_command (d.has_selection ? "layer.layerMask.revealSelection" : "layer.layerMask.revealAll",
                engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##adjust", "adjustment-layer", box, false, "Create new fill or adjustment layer"))
        {
            ImGui::OpenPopup ("newadjust");
        }
        if (ImGui::BeginPopup ("newadjust"))
        {
            const std::vector<engine::CommandSpec>& all = session_.commands ();
            const char* prefix                          = "layer.newAdjustmentLayer.";
            for (size_t i = 0; i < all.size (); i++)
            {
                if (strncmp (all[i].id, prefix, strlen (prefix)) == 0)
                {
                    command_item (all[i].id, NULL);
                }
            }
            ImGui::EndPopup ();
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##group", "folder", box, false, "Create a new group"))
        {
            run_command ("layer.new.group", engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##new", "plus", box, false, "Create a new layer"))
        {
            run_command ("layer.new.layer", engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##delete", "trash", box, false, "Delete layer"))
        {
            run_command ("layer.delete", engine::Value::object ());
        }
    }

    void App::layers_window ()
    {
        ImGui::Begin ("Layers");
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        /* Commands run from the rows replace the session's document: keep this one alive meanwhile. */
        std::shared_ptr<const doc::Document> doc = d->doc;
        layer_options (*doc);
        float k         = ui_scale_;
        float buttons_h = 24.0f * k + ImGui::GetStyle ().ItemSpacing.y * 2 + 4 * k;
        ImGui::PushStyleColor (ImGuiCol_ChildBg, theme_.field);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0, 0));
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0, 0));
        ImGui::BeginChild ("##rows", ImVec2 (0, -buttons_h), ImGuiChildFlags_Borders);
        layer_rows (*doc, doc->layers, 0);
        ImGui::EndChild ();
        ImGui::PopStyleVar (2);
        ImGui::PopStyleColor ();
        layer_buttons (*doc);
        ImGui::End ();
    }

} // namespace ui
