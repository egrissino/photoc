#include "algo/matting.h"
#include "algo/select_ops.h"
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

    /* Select > Select and Mask: Photoshop's workspace for refining a selection. The toolbar holds
       its tools (Quick Selection, Refine Edge Brush, Brush, Lasso, Hand, Zoom), the options bar
       their settings, and a Properties panel on the right the view mode, Edge Detection, Global
       Refinements and Output Settings. The canvas previews the refined selection in the chosen
       view; OK runs select.selectAndMask, Cancel undoes what the workspace's tools changed. */

    const int SAM_QUICK      = 0;
    const int SAM_REFINE     = 1;
    const int SAM_BRUSH      = 2;
    const int SAM_LASSO      = 3;
    const int SAM_HAND       = 4;
    const int SAM_ZOOM       = 5;
    const int SAM_TOOL_COUNT = 6;

    struct SamToolInfo
    {
        const char* label;
        const char* icon;
        const char* key;
        ImGuiKey shortcut;
    };

    const SamToolInfo SAM_TOOLS[SAM_TOOL_COUNT] = {
        {"Quick Selection Tool", "lasso-select", "W", ImGuiKey_W},
        {"Refine Edge Brush Tool", "brush-replace", "R", ImGuiKey_R},
        {"Brush Tool", "brush", "B", ImGuiKey_B},
        {"Lasso Tool", "lasso", "L", ImGuiKey_L},
        {"Hand Tool", "hand", "H", ImGuiKey_H},
        {"Zoom Tool", "zoom-in", "Z", ImGuiKey_Z},
    };

    /* The views, in Photoshop's order (also the order F steps through). */
    const int SAM_ONION                               = 0;
    const int SAM_ANTS                                = 1;
    const int SAM_OVERLAY                             = 2;
    const int SAM_ON_BLACK                            = 3;
    const int SAM_ON_WHITE                            = 4;
    const int SAM_BLACK_WHITE                         = 5;
    const int SAM_ON_LAYERS                           = 6;
    const char* const SAM_VIEW_LABELS[SAM_VIEW_COUNT] = {"Onion Skin (O)", "Marching Ants (M)", "Overlay (V)",
        "On Black (A)", "On White (T)", "Black & White (K)", "On Layers (Y)"};
    const ImGuiKey SAM_VIEW_KEYS[SAM_VIEW_COUNT]      = {
        ImGuiKey_O, ImGuiKey_M, ImGuiKey_V, ImGuiKey_A, ImGuiKey_T, ImGuiKey_K, ImGuiKey_Y};
    const float SAM_VIEW_OPACITY[SAM_VIEW_COUNT] = {50.0f, -1.0f, 50.0f, 100.0f, 100.0f, -1.0f, -1.0f};

    const char* const SAM_OUTPUT_IDS[6] = {
        "selection", "layerMask", "newLayer", "newLayerWithMask", "newDocument", "newDocumentWithMask"};
    const char* const SAM_OUTPUT_LABELS[6] = {"Selection", "Layer Mask", "New Layer", "New Layer with Layer Mask",
        "New Document", "New Document with Layer Mask"};

    void sam_reset_settings (SelectMask* m)
    {
        m->radius        = 0.0f;
        m->smart_radius  = false;
        m->smooth        = 0.0f;
        m->feather       = 0.0f;
        m->contrast      = 0.0f;
        m->shift_edge    = 0.0f;
        m->decontaminate = false;
        m->amount        = 100.0f;
        m->output        = 0;
        for (int i = 0; i < SAM_VIEW_COUNT; i++)
        {
            m->opacity[i] = SAM_VIEW_OPACITY[i];
        }
        m->view = SAM_ONION;
    }

    void App::open_select_and_mask ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        std::string why;
        const engine::CommandSpec* spec = session_.find_command ("select.selectAndMask");
        if (spec != NULL && !session_.command_enabled (*spec, &why))
        {
            set_error (why);
            return;
        }
        if (xf_.active)
        {
            commit_free_transform ();
        }
        SelectMask& m = sam_;
        if (!m.used || !m.remember)
        {
            sam_reset_settings (&m);
        }
        m.used          = true;
        m.active        = true;
        m.doc           = d->doc->id;
        m.history_at    = d->history.past_len ();
        m.seen_revision = 0;
        m.seen_serial   = 0;
        m.strokes       = engine::Value::array ();
        m.painting      = false;
        m.dirty         = true;
        m.hide_view     = false;
        m.show_edge     = false;
        m.show_original = false;
        m.saved_tool    = tool_;
        m.tool          = SAM_QUICK;
        tool_           = Tool::QuickSelection;
        /* Photoshop's default output: the selection, or the layer mask when that's what's refined. */
        const doc::Layer* l = d->doc->layer (d->active_layer);
        m.output            = !d->doc->has_selection && l != NULL && l->has_mask ? 1 : m.remember ? m.output : 0;
    }

    /* Leaves the workspace: OK applies the refinement (one history step), Cancel undoes what its
       tools changed. */
    void App::close_select_and_mask (bool ok)
    {
        SelectMask& m = sam_;
        if (!m.active)
        {
            return;
        }
        m.active = false;
        clear_preview ();
        tool_                     = m.saved_tool;
        const engine::DocState* d = session_.active ();
        if (d == NULL || d->doc->id != m.doc)
        {
            return;
        }
        if (ok)
        {
            engine::Value p = sam_params (false);
            run_command ("select.selectAndMask", p);
            return;
        }
        while (session_.active () != NULL && session_.active ()->history.past_len () > m.history_at)
        {
            engine::Error err;
            if (!session_.execute ("edit.undo", engine::Value::object (), NULL, &err))
            {
                break;
            }
        }
    }

    /* The command's params; the preview leaves out the output (its coverage is all it needs). */
    engine::Value App::sam_params (bool preview)
    {
        const SelectMask& m = sam_;
        engine::Value p     = {{"radius", m.radius}, {"smartRadius", m.smart_radius}, {"smooth", m.smooth},
                {"feather", m.feather}, {"contrast", m.contrast}, {"shiftEdge", m.shift_edge}};
        if (!m.strokes.empty ())
        {
            p["refineStrokes"] = m.strokes;
        }
        if (!preview)
        {
            p["decontaminate"] = m.decontaminate;
            p["amount"]        = m.amount;
            p["output"]        = SAM_OUTPUT_IDS[m.output];
        }
        return p;
    }

    /* A canvas-wide one-colour layer (no tiles: its default pixel), at `opacity`, masked by `mask`. */
    doc::Layer sam_veil (engine::Session& s, const doc::Document& d, const char* name, float r, float g, float b,
        float opacity, const raster::Surface* mask)
    {
        color::PixelFormat fmt = d.pixel_format ();
        float rgba[4]          = {r, g, b, 1.0f}, px[raster::MAX_CHANNELS];
        raster::from_rgba (fmt, rgba, s.cmyk, px);
        doc::Layer l = doc::Layer::raster (s.ids, name, fmt);
        l.pixels     = raster::Surface::with_default (fmt, px);
        l.opacity    = opacity;
        if (mask != NULL)
        {
            l.has_mask     = true;
            l.mask         = doc::LayerMask::reveal_all ();
            l.mask.surface = *mask;
        }
        return l;
    }

    /* f(coverage) over the canvas as a GRAY8 surface: f of the default outside the coverage's
       content, f of each value inside it. */
    raster::Surface sam_map (const raster::Surface& cov, geom::Rect canvas, float scale, float offset)
    {
        float outside       = offset;
        raster::Surface out = raster::Surface::with_default (color::PixelFormat::gray8 (), &outside);
        geom::Rect area     = cov.content_bounds ().intersect (canvas);
        if (area.is_empty ())
        {
            return out;
        }
        std::vector<float> v = algo::mask_from_surface (&cov, area);
        for (size_t i = 0; i < v.size (); i++)
        {
            v[i] = fminf (fmaxf (offset + scale * v[i], 0.0f), 1.0f);
        }
        out.write_region (area, v.data (), v.size ());
        return out;
    }

    /* The preview: the refined coverage (or the edge band with Show Edge, the original with Show
       Original) shown on a copy of the document in the chosen view. */
    void App::sam_preview ()
    {
        SelectMask& m             = sam_;
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        geom::Rect canvas                   = copy->bounds ();
        const doc::Layer* al                = copy->layer (d->active_layer);
        const raster::Surface* original     = copy->has_selection        ? &copy->selection
                                            : al != NULL && al->has_mask ? &al->mask.surface
                                                                         : NULL;
        if (original == NULL)
        {
            clear_preview ();
            return;
        }
        raster::Surface cov;
        if (m.show_edge)
        {
            size_t r                  = (size_t) fminf (fmaxf (roundf (m.radius), 1.0f), 250.0f);
            geom::Rect area           = original->content_bounds ().inflate ((int32_t) r + 1).intersect (canvas);
            std::vector<float> v      = algo::mask_from_surface (original, area);
            std::vector<float> grown  = algo::mat_morph (v, area.width (), area.height (), r, true),
                               shrunk = algo::mat_morph (v, area.width (), area.height (), r, false);
            for (size_t i = 0; i < v.size (); i++)
            {
                v[i] = fminf (fmaxf (grown[i] - shrunk[i], 0.0f), 1.0f);
            }
            cov = algo::mask_to_surface (v, area);
        }
        else if (m.show_original)
        {
            cov = *original;
        }
        else
        {
            engine::Error err;
            if (!engine::select_and_mask_coverage (session_, *d->doc, d->active_layer, sam_params (true), &cov, &err))
            {
                set_error (err.message);
                return;
            }
        }
        int view            = m.view;
        float o             = m.opacity[view] < 0.0f ? 1.0f : m.opacity[view] / 100.0f;
        copy->selection     = cov;
        copy->has_selection = view == SAM_ANTS && cov.tile_count () > 0;
        if (m.hide_view)
        {
            copy->has_selection = false;
        }
        else if (view == SAM_OVERLAY || view == SAM_ON_BLACK || view == SAM_ON_WHITE)
        {
            raster::Surface inv = sam_map (cov, canvas, -1.0f, 1.0f);
            float c[3]          = {view == SAM_ON_WHITE  ? 1.0f
                                   : view == SAM_OVERLAY ? 1.0f
                                                         : 0.0f,
                view == SAM_ON_WHITE ? 1.0f : 0.0f, view == SAM_ON_WHITE ? 1.0f : 0.0f};
            copy->layers.push_back (sam_veil (session_, *copy, "View", c[0], c[1], c[2], o, &inv));
        }
        else if (view == SAM_BLACK_WHITE)
        {
            copy->layers.push_back (sam_veil (session_, *copy, "Black", 0.0f, 0.0f, 0.0f, 1.0f, NULL));
            copy->layers.push_back (sam_veil (session_, *copy, "White", 1.0f, 1.0f, 1.0f, 1.0f, &cov));
        }
        else if ((view == SAM_ON_LAYERS || view == SAM_ONION) && al != NULL)
        {
            /* The layer masked by the coverage; Onion Skin also hides the others and lets some of
               the unselected area show through. */
            doc::LayerPath path;
            if (view == SAM_ONION && copy->path_of (d->active_layer, &path) && !path.empty ())
            {
                for (size_t i = 0; i < copy->layers.size (); i++)
                {
                    if (i != path[0])
                    {
                        copy->layers[i].visible = false;
                    }
                }
            }
            float keep    = view == SAM_ONION ? 1.0f - o : 0.0f;
            doc::Layer* l = copy->layer_mut (d->active_layer);
            if (l != NULL)
            {
                l->has_mask     = true;
                l->mask         = doc::LayerMask::reveal_all ();
                l->mask.surface = sam_map (cov, canvas, 1.0f - keep, keep);
            }
        }
        preview_doc_ = copy;
        preview_serial_++;
    }

    /* Each frame: leave when the document goes away, and preview again when the settings, the
       view or the selection (edited by the workspace's tools) changed. */
    void App::sam_frame ()
    {
        SelectMask& m = sam_;
        if (!m.active)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL || d->doc->id != m.doc)
        {
            m.active = false;
            tool_    = m.saved_tool;
            clear_preview ();
            return;
        }
        if (d->revision != m.seen_revision)
        {
            m.seen_revision = d->revision;
            m.dirty         = true;
        }
        /* Quick Selection previews its stroke itself while dragging. */
        if (quick_active_ || m.painting)
        {
            return;
        }
        if (m.dirty && !m.editing)
        {
            m.dirty = false;
            sam_preview ();
        }
    }

    /* The workspace's toolbar, in place of the main one. */
    void App::sam_toolbar (ImVec2 pos, ImVec2 size)
    {
        ImGui::SetNextWindowPos (pos);
        ImGui::SetNextWindowSize (size);
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.chrome);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (5.0f * ui_scale_, 8.0f * ui_scale_));
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (2.0f * ui_scale_, 3.0f * ui_scale_));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                               | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::Begin ("##samtoolbar", NULL, flags);
        float box = 30.0f * ui_scale_;
        for (int i = 0; i < SAM_TOOL_COUNT; i++)
        {
            char tip[96], id[16];
            snprintf (tip, sizeof tip, "%s  (%s)", SAM_TOOLS[i].label, SAM_TOOLS[i].key);
            snprintf (id, sizeof id, "##samtool%d", i);
            if (icon_button (id, SAM_TOOLS[i].icon, box, sam_.tool == i, tip))
            {
                sam_pick_tool (i);
            }
        }
        ImGui::End ();
        ImGui::PopStyleVar (3);
        ImGui::PopStyleColor ();
    }

    void App::sam_pick_tool (int t)
    {
        sam_.tool                      = t;
        const Tool MAP[SAM_TOOL_COUNT] = {
            Tool::QuickSelection, Tool::QuickSelection, Tool::QuickSelection, Tool::Lasso, Tool::Hand, Tool::Zoom};
        tool_ = MAP[t];
    }

    /* The options bar while the workspace is open: the tool's add / subtract mode and size. */
    void App::sam_options ()
    {
        SelectMask& m = sam_;
        float k = ui_scale_, box = ImGui::GetFrameHeight ();
        if (m.tool == SAM_HAND || m.tool == SAM_ZOOM)
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextDisabled ("Select and Mask");
            return;
        }
        /* Add and subtract (Alt subtracts while held). */
        bool* subtract = m.tool == SAM_REFINE ? &m.refine_subtract : m.tool == SAM_BRUSH ? &m.brush_subtract : NULL;
        if (subtract != NULL)
        {
            if (icon_button ("##samadd", "square-plus", box, !*subtract, "Expand Detection Area / Add to Selection"))
            {
                *subtract = false;
            }
            ImGui::SameLine (0, 2 * k);
            if (icon_button ("##samsub", "squares-subtract", box, *subtract,
                    "Restore Original Edge / Subtract from Selection  (Alt)"))
            {
                *subtract = true;
            }
        }
        else
        {
            int modes            = m.tool == SAM_QUICK ? 3 : 4;
            const char* icons[4] = {"square", "square-plus", "squares-subtract", "copy"};
            const char* tips[4]  = {"New Selection", "Add to Selection  (Shift)", "Subtract from Selection  (Alt)",
                 "Intersect with Selection  (Shift+Alt)"};
            if (selection_mode_ >= modes)
            {
                selection_mode_ = 1;
            }
            for (int i = 0; i < modes; i++)
            {
                char id[16];
                snprintf (id, sizeof id, "##sammode%d", i);
                if (icon_button (id, icons[i], box, selection_mode_ == i, tips[i]))
                {
                    selection_mode_ = i;
                }
                ImGui::SameLine (0, 2 * k);
            }
        }
        ImGui::SameLine (0, 14 * k);
        if (m.tool != SAM_LASSO)
        {
            float* size = m.tool == SAM_REFINE ? &m.refine_size : m.tool == SAM_BRUSH ? &m.brush_size : &quick_size_;
            ImGui::AlignTextToFramePadding ();
            ImGui::TextUnformatted ("Size:");
            ImGui::SameLine ();
            ImGui::SetNextItemWidth (110 * k);
            ImGui::DragFloat ("##samsize", size, 0.5f, 1.0f, 5000.0f, "%.0f px",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        }
        if (m.tool == SAM_QUICK)
        {
            ImGui::SameLine (0, 14 * k);
            ImGui::Checkbox ("Sample All Layers", &quick_all_layers_);
        }
    }

    /* A slider row of the Properties panel; true when it was let go after a change. */
    bool sam_slider (const char* label, const char* id, float* v, float lo, float hi, const char* fmt, bool log,
        float k, bool* editing)
    {
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted (label);
        ImGui::SameLine (110 * k);
        ImGui::SetNextItemWidth (-1);
        ImGui::SliderFloat (
            id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp | (log ? ImGuiSliderFlags_Logarithmic : 0));
        *editing = *editing || ImGui::IsItemActive ();
        return ImGui::IsItemDeactivatedAfterEdit ();
    }

    /* The Properties panel down the right: View Mode, Edge Detection, Global Refinements, Output
       Settings, OK and Cancel. */
    void App::sam_properties (ImVec2 pos, ImVec2 size)
    {
        SelectMask& m = sam_;
        float k       = ui_scale_;
        ImGui::SetNextWindowPos (pos);
        ImGui::SetNextWindowSize (size);
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.card);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                               | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                               | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::Begin ("Properties###samprops", NULL, flags);
        ImGui::PopStyleColor ();
        bool editing = false, changed = false;

        ImGui::SeparatorText ("View Mode");
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("View:");
        ImGui::SameLine (110 * k);
        ImGui::SetNextItemWidth (-1);
        if (ImGui::BeginCombo ("##samview", SAM_VIEW_LABELS[m.view]))
        {
            for (int i = 0; i < SAM_VIEW_COUNT; i++)
            {
                if (ImGui::Selectable (SAM_VIEW_LABELS[i], m.view == i))
                {
                    m.view  = i;
                    changed = true;
                }
            }
            ImGui::EndCombo ();
        }
        changed |= ImGui::Checkbox ("Show Edge (J)", &m.show_edge);
        ImGui::SameLine ();
        changed |= ImGui::Checkbox ("Show Original (P)", &m.show_original);
        if (m.opacity[m.view] >= 0.0f)
        {
            changed |=
                sam_slider ("Opacity", "##samopacity", &m.opacity[m.view], 0.0f, 100.0f, "%.0f%%", false, k, &editing);
        }

        ImGui::SeparatorText ("Edge Detection");
        changed |= sam_slider ("Radius", "##samradius", &m.radius, 0.0f, 250.0f, "%.0f px", false, k, &editing);
        changed |= ImGui::Checkbox ("Smart Radius", &m.smart_radius);

        ImGui::SeparatorText ("Global Refinements");
        changed |= sam_slider ("Smooth", "##samsmooth", &m.smooth, 0.0f, 100.0f, "%.0f", false, k, &editing);
        changed |= sam_slider ("Feather", "##samfeather", &m.feather, 0.0f, 1000.0f, "%.1f px", true, k, &editing);
        changed |= sam_slider ("Contrast", "##samcontrast", &m.contrast, 0.0f, 100.0f, "%.0f%%", false, k, &editing);
        changed |=
            sam_slider ("Shift Edge", "##samshift", &m.shift_edge, -100.0f, 100.0f, "%+.0f%%", false, k, &editing);
        if (ImGui::Button ("Clear Selection"))
        {
            run_command ("select.deselect", engine::Value::object ());
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Invert"))
        {
            run_command ("select.inverse", engine::Value::object ());
        }

        ImGui::SeparatorText ("Output Settings");
        ImGui::Checkbox ("Decontaminate Colors", &m.decontaminate);
        if (!m.decontaminate)
        {
            ImGui::BeginDisabled ();
        }
        sam_slider ("Amount", "##samamount", &m.amount, 0.0f, 100.0f, "%.0f%%", false, k, &editing);
        if (!m.decontaminate)
        {
            ImGui::EndDisabled ();
        }
        /* Decontaminating makes new pixels: Selection and Layer Mask aren't offered with it. */
        if (m.decontaminate && m.output < 3)
        {
            m.output = 3;
        }
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Output To:");
        ImGui::SameLine (110 * k);
        ImGui::SetNextItemWidth (-1);
        if (ImGui::BeginCombo ("##samoutput", SAM_OUTPUT_LABELS[m.output]))
        {
            for (int i = 0; i < 6; i++)
            {
                bool usable = !m.decontaminate || i >= 2;
                if (ImGui::Selectable (SAM_OUTPUT_LABELS[i], m.output == i, usable ? 0 : ImGuiSelectableFlags_Disabled))
                {
                    m.output = i;
                }
            }
            ImGui::EndCombo ();
        }
        ImGui::Checkbox ("Remember Settings", &m.remember);

        ImGui::Spacing ();
        ImGui::Separator ();
        float bw = 90 * k;
        ImGui::SetCursorPosX (
            ImGui::GetWindowWidth () - bw * 2 - ImGui::GetStyle ().ItemSpacing.x - ImGui::GetStyle ().WindowPadding.x);
        bool cancel = ImGui::Button ("Cancel", ImVec2 (bw, 0));
        ImGui::SameLine ();
        bool ok = ImGui::Button ("OK", ImVec2 (bw, 0));
        ImGui::End ();
        m.editing = editing;
        if (changed)
        {
            m.dirty = true;
        }
        if (ok || cancel)
        {
            close_select_and_mask (ok);
        }
    }

    /* The Refine Edge Brush and the Brush: a stroke collected while dragging. The Refine Edge
       Brush's strokes join the refinement; the Brush paints into the selection on release. */
    void App::sam_input (float ox, float oy)
    {
        SelectMask& m = sam_;
        ImGuiIO& io   = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetMouseCursor (ImGuiMouseCursor_None);
        }
        bool refine = m.tool == SAM_REFINE;
        float size  = refine ? m.refine_size : m.brush_size;
        if (activated)
        {
            m.painting = true;
            m.paint_points.clear ();
            m.paint_subtract = (refine ? m.refine_subtract : m.brush_subtract) != io.KeyAlt;
        }
        if (!m.painting)
        {
            return;
        }
        if (active)
        {
            size_t n   = m.paint_points.size ();
            float step = fmaxf (size / 8.0f, 1.0f);
            if (n < 2 || hypotf (mx - m.paint_points[n - 2], my - m.paint_points[n - 1]) >= step)
            {
                m.paint_points.push_back (mx);
                m.paint_points.push_back (my);
            }
        }
        if (released)
        {
            m.painting        = false;
            engine::Value pts = engine::Value::array ();
            for (size_t i = 0; i + 1 < m.paint_points.size (); i += 2)
            {
                pts.push_back ({m.paint_points[i], m.paint_points[i + 1]});
            }
            m.paint_points.clear ();
            if (pts.empty ())
            {
                return;
            }
            engine::Value stroke = {{"points", pts}, {"size", size}, {"mode", m.paint_subtract ? "subtract" : "add"}};
            if (refine)
            {
                m.strokes.push_back (stroke);
                m.dirty = true;
            }
            else
            {
                run_command ("select.brushStroke", stroke);
            }
        }
    }

    /* The brush outline, and the stroke being painted. */
    void App::sam_overlay (float ox, float oy)
    {
        SelectMask& m = sam_;
        if (m.tool != SAM_REFINE && m.tool != SAM_BRUSH)
        {
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float size     = m.tool == SAM_REFINE ? m.refine_size : m.brush_size;
        float r        = size * 0.5f * zoom_;
        ImU32 col      = m.tool == SAM_REFINE ? IM_COL32 (60, 160, 255, 160) : IM_COL32 (255, 255, 255, 120);
        for (size_t i = 0; i + 1 < m.paint_points.size (); i += 2)
        {
            ImVec2 c (ox + m.paint_points[i] * zoom_, oy + m.paint_points[i + 1] * zoom_);
            dl->AddCircleFilled (c, r, col, 24);
        }
        ImVec2 mp = ImGui::GetIO ().MousePos;
        dl->AddCircle (mp, r, IM_COL32 (0, 0, 0, 200), 0, 1.5f);
        dl->AddCircle (mp, fmaxf (r - 1.0f, 1.0f), IM_COL32 (255, 255, 255, 200), 0, 1.0f);
        bool subtract = (m.tool == SAM_REFINE ? m.refine_subtract : m.brush_subtract) != ImGui::GetIO ().KeyAlt;
        dl->AddLine (ImVec2 (mp.x - 4, mp.y), ImVec2 (mp.x + 4, mp.y), IM_COL32 (255, 255, 255, 230), 1.5f);
        if (!subtract)
        {
            dl->AddLine (ImVec2 (mp.x, mp.y - 4), ImVec2 (mp.x, mp.y + 4), IM_COL32 (255, 255, 255, 230), 1.5f);
        }
    }

    /* The workspace's keys: its tools, the views (O M V A T K Y, F the next one, X held hides the
       view), J Show Edge, P Show Original, [ ] the brush size, Enter OK and Escape Cancel. */
    void App::sam_keys ()
    {
        SelectMask& m = sam_;
        if (ImGui::GetIO ().WantTextInput)
        {
            return;
        }
        if (ImGui::Shortcut (ImGuiKey_Enter, ImGuiInputFlags_RouteGlobal)
            || ImGui::Shortcut (ImGuiKey_KeypadEnter, ImGuiInputFlags_RouteGlobal))
        {
            close_select_and_mask (true);
            return;
        }
        if (ImGui::Shortcut (ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal))
        {
            close_select_and_mask (false);
            return;
        }
        if (ImGui::Shortcut (ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))
        {
            run_command ("edit.undo", engine::Value::object ());
        }
        if (ImGui::GetIO ().KeyCtrl)
        {
            return;
        }
        for (int i = 0; i < SAM_TOOL_COUNT; i++)
        {
            if (ImGui::IsKeyPressed (SAM_TOOLS[i].shortcut, false))
            {
                sam_pick_tool (i);
            }
        }
        for (int i = 0; i < SAM_VIEW_COUNT; i++)
        {
            if (ImGui::IsKeyPressed (SAM_VIEW_KEYS[i], false))
            {
                m.view  = i;
                m.dirty = true;
            }
        }
        if (ImGui::IsKeyPressed (ImGuiKey_F, false))
        {
            m.view  = (m.view + 1) % SAM_VIEW_COUNT;
            m.dirty = true;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_J, false))
        {
            m.show_edge = !m.show_edge;
            m.dirty     = true;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_P, false))
        {
            m.show_original = !m.show_original;
            m.dirty         = true;
        }
        bool hide = ImGui::IsKeyDown (ImGuiKey_X);
        if (hide != m.hide_view)
        {
            m.hide_view = hide;
            m.dirty     = true;
        }
        float* size = m.tool == SAM_REFINE ? &m.refine_size
                    : m.tool == SAM_BRUSH  ? &m.brush_size
                    : m.tool == SAM_QUICK  ? &quick_size_
                                           : NULL;
        if (size != NULL && ImGui::IsKeyPressed (ImGuiKey_LeftBracket))
        {
            *size = fmaxf (1.0f, *size * 0.9f);
        }
        if (size != NULL && ImGui::IsKeyPressed (ImGuiKey_RightBracket))
        {
            *size = fminf (5000.0f, *size * 1.1f);
        }
    }

} // namespace ui
