#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The Properties panel, as Photoshop's: a header naming what is selected, then its sections.
       Pixel layers and groups show their Transform (W, H, X, Y); adjustment layers their editor;
       a targeted mask its Density and Feather; with no layer, the document. */

    const char* mode_name (color::ColorMode m);
    geom::Rect layer_extent (const doc::Layer& l);

    /* The bounds a layer's content covers (its children's for a group). */
    geom::Rect layer_extent (const doc::Layer& l)
    {
        if (l.is_group ())
        {
            geom::Rect r;
            for (size_t i = 0; i < l.children.size (); i++)
            {
                r = r.unite (layer_extent (l.children[i]));
            }
            return r;
        }
        const raster::Surface* s = l.surface ();
        return s != NULL ? s->content_bounds () : geom::Rect ();
    }

    void App::props_header (const char* icon, const char* title)
    {
        float k = ui_scale_, g = 16 * k;
        ImVec2 p = ImGui::GetCursorScreenPos ();
        float fh = ImGui::GetFrameHeight ();
        draw_icon (ImGui::GetWindowDrawList (), &icons_, icon, ImVec2 (p.x, p.y + (fh - g) * 0.5f), g, theme_.icon);
        ImGui::SetCursorScreenPos (ImVec2 (p.x + g + 8 * k, p.y));
        ImGui::AlignTextToFramePadding ();
        ImGui::PushFont (fonts_.semibold, 0.0f);
        ImGui::TextUnformatted (title);
        ImGui::PopFont ();
        ImGui::Separator ();
    }

    /* A section title in the faint text colour. */
    void section (const char* title)
    {
        ImGui::Spacing ();
        ImGui::TextDisabled ("%s", title);
    }

    /* W, H, X and Y of the layer's content. X and Y move it (layer.translate); W and H scale it from
       its top-left corner (edit.transform). Edits apply when a field is released. */
    void App::layer_transform_properties (const doc::Document& d, const doc::Layer& l)
    {
        float k      = ui_scale_;
        geom::Rect b = layer_extent (l);
        section ("Transform");
        if (b.is_empty ())
        {
            ImGui::TextDisabled ("Empty layer");
            return;
        }
        float cur[4] = {(float) b.width (), (float) b.height (), (float) b.x0, (float) b.y0};
        if (!xform_editing_)
        {
            memcpy (xform_edit_, cur, sizeof cur);
        }
        const char* labels[4] = {"W", "H", "X", "Y"};
        bool active           = false;
        int done              = -1;
        float half            = (ImGui::GetContentRegionAvail ().x - 30 * k) * 0.5f;
        for (int i = 0; i < 4; i++)
        {
            if (i % 2 == 1)
            {
                ImGui::SameLine (0, 14 * k);
            }
            opt_label (labels[i]);
            ImGui::SetNextItemWidth (fmaxf (half - 20 * k, 50 * k));
            char id[8];
            snprintf (id, sizeof id, "##t%d", i);
            ImGui::DragFloat (id, &xform_edit_[i], 1.0f, i < 2 ? 1.0f : -100000.0f, 100000.0f, "%.0f px",
                ImGuiSliderFlags_AlwaysClamp);
            active = active || ImGui::IsItemActive ();
            if (ImGui::IsItemDeactivatedAfterEdit ())
            {
                done = i;
            }
        }
        xform_editing_ = active;
        if (done >= 2)
        {
            int32_t dx = (int32_t) lroundf (xform_edit_[2] - cur[2]), dy = (int32_t) lroundf (xform_edit_[3] - cur[3]);
            if (dx != 0 || dy != 0)
            {
                run_command ("layer.translate", engine::Value{{"layer", l.id}, {"dx", dx}, {"dy", dy}});
            }
        }
        else if (done >= 0)
        {
            double x0 = b.x0, y0 = b.y0;
            double w = fmax (1.0, roundf (xform_edit_[0])), h = fmax (1.0, roundf (xform_edit_[1]));
            if (w != b.width () || h != b.height ())
            {
                engine::Value quad = {{x0, y0}, {x0 + w, y0}, {x0 + w, y0 + h}, {x0, y0 + h}};
                run_command ("edit.transform",
                    engine::Value{{"layer", l.id}, {"rect", {b.x0, b.y0, b.x1, b.y1}}, {"quad", quad}});
            }
        }
        (void) d;
    }

    /* Properties > Masks: density and feather preview live on a copy while a field is held and
       commit as one step on release. */
    void App::mask_properties (const doc::Layer& l)
    {
        float k = ui_scale_;
        props_header ("layer-mask", "Layer Mask");
        if (!mask_editing_)
        {
            mask_density_ = l.mask.density * 100.0f;
            mask_feather_ = l.mask.feather;
        }
        bool changed = false, active = false, done = false;
        float w = ImGui::GetContentRegionAvail ().x - 70 * k;
        section ("Density");
        ImGui::SetNextItemWidth (w);
        changed = ImGui::SliderFloat ("##density", &mask_density_, 0.0f, 100.0f, "%.0f%%") || changed;
        active  = active || ImGui::IsItemActive ();
        done    = done || ImGui::IsItemDeactivatedAfterEdit ();
        section ("Feather");
        ImGui::SetNextItemWidth (w);
        changed =
            ImGui::SliderFloat ("##mfeather", &mask_feather_, 0.0f, 1000.0f, "%.1f px", ImGuiSliderFlags_Logarithmic)
            || changed;
        active        = active || ImGui::IsItemActive ();
        done          = done || ImGui::IsItemDeactivatedAfterEdit ();
        mask_editing_ = active;
        if (changed && active)
        {
            std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*session_.active ()->doc);
            doc::Layer* target                  = copy->layer_mut (l.id);
            target->mask.density                = mask_density_ / 100.0f;
            target->mask.feather                = mask_feather_;
            preview_doc_                        = copy;
            preview_serial_++;
        }
        if (done)
        {
            clear_preview ();
            run_command ("layer.layerMask.edit",
                engine::Value{{"layer", l.id}, {"density", mask_density_}, {"feather", mask_feather_}});
        }
        ImGui::Spacing ();
        ImGui::Separator ();
        float box = 24 * k;
        if (icon_button ("##mlink", l.mask.linked ? "link" : "unlink", box, l.mask.linked,
                l.mask.linked ? "Unlink mask from layer" : "Link mask to layer"))
        {
            run_command ("layer.layerMask.linked", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (l.kind == doc::LayerKind::Raster)
        {
            if (icon_button ("##mapply", "check", box, false, "Apply mask"))
            {
                run_command ("layer.layerMask.apply", engine::Value{{"layer", l.id}});
            }
            ImGui::SameLine (0, 4 * k);
        }
        if (icon_button ("##menable", l.mask.enabled ? "eye" : "eye-off", box, false,
                l.mask.enabled ? "Disable mask" : "Enable mask"))
        {
            run_command ("layer.layerMask.enabled", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##mdelete", "trash", box, false, "Delete mask"))
        {
            run_command ("layer.layerMask.delete", engine::Value{{"layer", l.id}});
        }
    }

    void App::document_properties (const doc::Document& d)
    {
        props_header ("file", "Document");
        section ("Canvas");
        ImGui::Text ("W  %u px      H  %u px", d.size.width, d.size.height);
        ImGui::Text ("Resolution  %g ppi", (double) d.resolution_dpi);
        unsigned bits = d.depth == color::SampleType::U8 ? 8 : d.depth == color::SampleType::U16 ? 16 : 32;
        ImGui::Text ("Mode  %s Color, %u bit", mode_name (d.mode), bits);
        ImGui::Spacing ();
        if (ImGui::Button ("Image Size…"))
        {
            run_command ("image.imageSize", engine::Value::object ());
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Canvas Size…"))
        {
            run_command ("image.canvasSize", engine::Value::object ());
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Trim…"))
        {
            run_command ("image.trim", engine::Value::object ());
        }
    }

    void App::adjustment_properties (const engine::DocState& d, const doc::Layer& l)
    {
        float k = ui_scale_;
        props_header ("adjustment-layer", l.adjustment.label ());
        if (props_layer_ != l.id || (!props_editing_ && props_revision_ != d.revision))
        {
            int channel = props_.channel, tone = props_.tone, row = props_.row;
            bool same = props_layer_ == l.id;
            adjust_editor_init (&props_, l.adjustment);
            if (same)
            {
                props_.channel = channel;
                props_.tone    = tone;
                props_.row     = row;
            }
            props_layer_    = l.id;
            props_revision_ = d.revision;
        }
        float buttons_h = 24 * k + ImGui::GetStyle ().ItemSpacing.y * 2;
        ImGui::BeginChild ("##adjust", ImVec2 (0, -buttons_h));
        bool changed, finished;
        adjust_editor_draw (&props_, &changed, &finished);
        ImGui::EndChild ();
        if (changed)
        {
            props_editing_ = true;
            preview_properties (l);
        }
        if (finished)
        {
            engine::Value p = props_.params;
            p["layer"]      = l.id;
            clear_preview ();
            props_editing_ = false;
            run_command ("layer.setAdjustment", p);
            const engine::DocState* after = session_.active ();
            if (after != NULL)
            {
                props_revision_ = after->revision;
            }
        }
        /* Photoshop's bottom row: clip to the layer below, reset, visibility, delete. */
        float box = 24 * k;
        ImGui::Separator ();
        if (icon_button ("##aclip", "clip-below", box, l.clipped,
                l.clipped ? "Release from the layer below" : "Clip to the layer below"))
        {
            run_command (
                l.clipped ? "layer.releaseClippingMask" : "layer.createClippingMask", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##areset", "undo-2", box, false, "Reset to adjustment defaults"))
        {
            run_command ("layer.setAdjustment", engine::Value{{"layer", l.id}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##avisible", l.visible ? "eye" : "eye-off", box, false, "Toggle layer visibility"))
        {
            run_command ("layer.setProps", engine::Value{{"layer", l.id}, {"visible", !l.visible}});
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##adelete", "trash", box, false, "Delete this adjustment layer"))
        {
            run_command ("layer.delete", engine::Value{{"layer", l.id}});
        }
    }

    void App::properties_window ()
    {
        ImGui::Begin ("Properties");
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        std::shared_ptr<const doc::Document> keep = d->doc;
        const doc::Layer* l                       = d->doc->layer (d->active_layer);
        bool adjustment                           = l != NULL && l->kind == doc::LayerKind::Adjustment;
        if (!adjustment || mask_targeted ())
        {
            if (props_editing_)
            {
                clear_preview ();
            }
            props_editing_ = false;
            props_layer_   = doc::NO_LAYER;
        }
        if (l == NULL)
        {
            mask_editing_ = false;
            document_properties (*d->doc);
        }
        else if (l->has_mask && mask_targeted ())
        {
            mask_properties (*l);
        }
        else if (adjustment)
        {
            mask_editing_ = false;
            adjustment_properties (*d, *l);
        }
        else if (l->kind == doc::LayerKind::Shape)
        {
            mask_editing_ = false;
            shape_properties (*l);
        }
        else
        {
            mask_editing_ = false;
            props_header (l->is_group () ? "folder" : "image", l->is_group ()                  ? "Group"
                                                               : d->doc->is_background (l->id) ? "Background"
                                                                                               : "Pixel Layer");
            layer_transform_properties (*d->doc, *l);
            if (l->has_vector_mask)
            {
                vector_mask_properties (*l);
            }
            if (l->has_mask)
            {
                section ("Layer Mask");
                ImGui::TextDisabled ("Click the mask thumbnail in Layers to edit it.");
            }
        }
        ImGui::End ();
    }

} // namespace ui
