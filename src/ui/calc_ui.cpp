#include "color/blend.h"
#include "engine/apply_image.h"
#include "ui/app.h"
#include <ctype.h>
#include <imgui.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>

namespace ui
{

    /* Image > Apply Image and Image > Calculations: pick a document, layer and channel per source
       (and an optional mask), the blending and opacity; Apply Image previews on the canvas,
       Calculations makes a new channel, document or selection. */

    /* The separable blend modes Apply Image offers (their labels are what the engine parses),
       then Add and Subtract. */
    void calc_blend_choices (std::vector<std::string>* out)
    {
        out->clear ();
        for (size_t i = 0; i < color::LAYER_MODE_COUNT; i++)
        {
            color::BlendMode m = color::LAYER_MODES[i];
            if (!color::blend_is_separable (m) || m == color::BlendMode::Dissolve || m == color::BlendMode::PassThrough)
            {
                continue;
            }
            out->push_back (color::blend_label (m));
        }
        out->push_back ("Add");
        out->push_back ("Subtract");
    }

    /* The channels a source can name: the composite, each colour channel, the alpha channels,
       then the selection, the layer's transparency and its mask where there are such. */
    void calc_channel_choices (
        const doc::Document& d, doc::LayerId layer, std::vector<std::string>* labels, std::vector<engine::Value>* ids)
    {
        labels->clear ();
        ids->clear ();
        const char *names[4], *composite;
        size_t n = engine::color_channel_names (d.mode, names, &composite);
        labels->push_back (composite);
        ids->push_back ("composite");
        for (size_t i = 0; n > 1 && i < n; i++)
        {
            labels->push_back (names[i]);
            ids->push_back (names[i]);
        }
        for (size_t i = 0; i < d.channels.size (); i++)
        {
            labels->push_back (d.channels[i].name);
            ids->push_back ((int64_t) i);
        }
        if (d.has_selection)
        {
            labels->push_back ("Selection");
            ids->push_back ("selection");
        }
        const doc::Layer* l = layer != 0 ? d.layer (layer) : NULL;
        if (l != NULL)
        {
            labels->push_back ("Transparency");
            ids->push_back ("transparency");
            if (l->has_mask)
            {
                labels->push_back ("Layer Mask");
                ids->push_back ("mask");
            }
        }
    }

    void App::open_apply_image (bool calculations)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        ApplyImageDialog& a = apply_image_;
        bool fresh          = !a.used || a.calc != calculations;
        a.open              = true;
        a.calc              = calculations;
        a.used              = true;
        a.dirty             = true;
        if (!fresh)
        {
            return;
        }
        for (int i = 0; i < 3; i++)
        {
            a.src[i].doc     = (int) session_.active_index ();
            a.src[i].layer   = 0;
            a.src[i].channel = 0;
            a.src[i].invert  = false;
        }
        a.use_mask = false;
        std::vector<std::string> blends;
        calc_blend_choices (&blends);
        a.blend = 0;
        for (size_t i = 0; i < blends.size (); i++)
        {
            if (blends[i] == "Multiply")
            {
                a.blend = (int) i;
            }
        }
        a.opacity  = 100.0f;
        a.scale    = 1.0f;
        a.offset   = 0.0f;
        a.preserve = false;
        a.result   = 0;
        a.preview  = true;
    }

    /* A source spec from row `i`. */
    engine::Value App::calc_source_spec (int i)
    {
        const CalcSource& c = apply_image_.src[i];
        engine::Value v     = {{"document", c.doc}, {"invert", c.invert}};
        if (c.layer != 0)
        {
            v["layer"] = c.layer;
        }
        const engine::DocState* sd = session_.doc_at ((size_t) c.doc);
        if (sd != NULL)
        {
            std::vector<std::string> labels;
            std::vector<engine::Value> ids;
            calc_channel_choices (*sd->doc, c.layer, &labels, &ids);
            if (c.channel >= 0 && (size_t) c.channel < ids.size ())
            {
                v["channel"] = ids[(size_t) c.channel];
            }
        }
        return v;
    }

    engine::Value App::apply_image_params ()
    {
        ApplyImageDialog& a = apply_image_;
        std::vector<std::string> blends;
        calc_blend_choices (&blends);
        engine::Value p = {{"blending", blends[(size_t) a.blend]}, {"opacity", a.opacity}};
        if (a.blend >= (int) blends.size () - 2)
        {
            p["scale"]  = a.scale;
            p["offset"] = a.offset;
        }
        if (a.calc)
        {
            p["source1"]                 = calc_source_spec (0);
            p["source2"]                 = calc_source_spec (1);
            const char* const RESULTS[3] = {"newChannel", "newDocument", "selection"};
            p["result"]                  = RESULTS[a.result];
        }
        else
        {
            p["source"]               = calc_source_spec (0);
            p["preserveTransparency"] = a.preserve;
        }
        if (a.use_mask)
        {
            p["mask"] = calc_source_spec (2);
        }
        return p;
    }

    /* One source's rows: document, layer, channel and Invert. True when something changed. */
    bool App::calc_source_rows (int i, const char* title)
    {
        CalcSource& c = apply_image_.src[i];
        float k       = ui_scale_;
        bool changed  = false;
        ImGui::PushID (i);
        ImGui::SeparatorText (title);
        const engine::DocState* sd  = session_.doc_at ((size_t) c.doc);
        const engine::DocState* cur = session_.active ();
        if (sd == NULL)
        {
            c.doc = (int) session_.active_index ();
            sd    = cur;
        }
        ImGui::SetNextItemWidth (240 * k);
        if (ImGui::BeginCombo ("Document", sd->doc->name.c_str ()))
        {
            for (size_t d = 0; d < session_.doc_count (); d++)
            {
                const engine::DocState* o = session_.doc_at (d);
                /* Only documents of the same size can be sources. */
                bool fits = o->doc->size.width == cur->doc->size.width && o->doc->size.height == cur->doc->size.height;
                ImGui::PushID ((int) d);
                if (ImGui::Selectable (
                        o->doc->name.c_str (), (int) d == c.doc, fits ? 0 : ImGuiSelectableFlags_Disabled))
                {
                    c.doc     = (int) d;
                    c.layer   = 0;
                    c.channel = 0;
                    changed   = true;
                }
                ImGui::PopID ();
            }
            ImGui::EndCombo ();
        }
        const doc::Document& doc = *sd->doc;
        const doc::Layer* layer  = c.layer != 0 ? doc.layer (c.layer) : NULL;
        if (layer == NULL)
        {
            c.layer = 0;
        }
        ImGui::SetNextItemWidth (240 * k);
        if (ImGui::BeginCombo ("Layer", layer != NULL ? layer->name.c_str () : "Merged"))
        {
            if (ImGui::Selectable ("Merged", c.layer == 0))
            {
                c.layer   = 0;
                c.channel = 0;
                changed   = true;
            }
            std::vector<doc::WalkEntry> all = doc.walk ();
            for (size_t w = 0; w < all.size (); w++)
            {
                const doc::Layer* l = all[w].layer;
                if (l->kind != doc::LayerKind::Raster)
                {
                    continue;
                }
                ImGui::PushID ((int) w);
                if (ImGui::Selectable (l->name.c_str (), l->id == c.layer))
                {
                    c.layer   = l->id;
                    c.channel = 0;
                    changed   = true;
                }
                ImGui::PopID ();
            }
            ImGui::EndCombo ();
        }
        std::vector<std::string> labels;
        std::vector<engine::Value> ids;
        calc_channel_choices (doc, c.layer, &labels, &ids);
        if (c.channel < 0 || (size_t) c.channel >= labels.size ())
        {
            c.channel = 0;
        }
        ImGui::SetNextItemWidth (240 * k);
        if (ImGui::BeginCombo ("Channel", labels[(size_t) c.channel].c_str ()))
        {
            for (size_t ch = 0; ch < labels.size (); ch++)
            {
                ImGui::PushID ((int) ch);
                if (ImGui::Selectable (labels[ch].c_str (), (int) ch == c.channel))
                {
                    c.channel = (int) ch;
                    changed   = true;
                }
                ImGui::PopID ();
            }
            ImGui::EndCombo ();
        }
        changed |= ImGui::Checkbox ("Invert", &c.invert);
        ImGui::PopID ();
        return changed;
    }

    void App::apply_image_dialog ()
    {
        ApplyImageDialog& a = apply_image_;
        if (!a.open)
        {
            return;
        }
        if (session_.active () == NULL)
        {
            a.open = false;
            clear_preview ();
            return;
        }
        float k           = ui_scale_;
        const char* title = a.calc ? "Calculations###applyimage" : "Apply Image###applyimage";
        ImGui::OpenPopup ("###applyimage");
        if (!ImGui::BeginPopupModal (title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        bool changed = calc_source_rows (0, a.calc ? "Source 1" : "Source");
        if (a.calc)
        {
            changed |= calc_source_rows (1, "Source 2");
        }
        ImGui::SeparatorText (a.calc ? "Blending" : "Target");
        std::vector<std::string> blends;
        calc_blend_choices (&blends);
        ImGui::SetNextItemWidth (240 * k);
        if (ImGui::BeginCombo ("Blending", blends[(size_t) a.blend].c_str ()))
        {
            for (size_t i = 0; i < blends.size (); i++)
            {
                if (ImGui::Selectable (blends[i].c_str (), (int) i == a.blend))
                {
                    a.blend = (int) i;
                    changed = true;
                }
            }
            ImGui::EndCombo ();
        }
        bool editing = false;
        ImGui::SetNextItemWidth (240 * k);
        changed |= ImGui::SliderFloat ("Opacity", &a.opacity, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        editing = editing || ImGui::IsItemActive ();
        if (a.blend >= (int) blends.size () - 2)
        {
            ImGui::SetNextItemWidth (110 * k);
            changed |= ImGui::DragFloat ("Scale", &a.scale, 0.01f, 1.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (110 * k);
            changed |=
                ImGui::DragFloat ("Offset", &a.offset, 1.0f, -255.0f, 255.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        }
        if (!a.calc)
        {
            changed |= ImGui::Checkbox ("Preserve Transparency", &a.preserve);
        }
        changed |= ImGui::Checkbox ("Mask…", &a.use_mask);
        if (a.use_mask)
        {
            changed |= calc_source_rows (2, "Mask");
        }
        if (a.calc)
        {
            ImGui::SeparatorText ("Result");
            ImGui::SetNextItemWidth (240 * k);
            changed |= ImGui::Combo ("Result", &a.result, "New Channel\0New Document\0Selection\0");
        }
        else
        {
            changed |= ImGui::Checkbox ("Preview", &a.preview);
        }
        if (changed)
        {
            a.dirty = true;
        }
        /* Apply Image previews on a copy once a slider is let go. */
        if (!a.calc && a.dirty && !editing)
        {
            a.dirty                   = false;
            const engine::DocState* d = session_.active ();
            if (a.preview)
            {
                std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
                engine::Error err;
                if (engine::apply_image_to (session_, copy.get (), d->active_layer, apply_image_params (), &err))
                {
                    preview_doc_ = copy;
                    preview_serial_++;
                }
                else
                {
                    set_error (err.message);
                    clear_preview ();
                }
            }
            else
            {
                clear_preview ();
            }
        }
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            a.open = false;
            clear_preview ();
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                run_command (a.calc ? "image.calculations" : "image.applyImage", apply_image_params ());
            }
        }
        ImGui::EndPopup ();
    }

} // namespace ui
