#include "color/convert.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The View menu's display aids: rulers (drag a guide out of one; drag it back to delete it),
       guides (moved with the Move tool unless locked), the grid, the pixel grid at high zoom, layer
       edges, and Extras switching them all at once; Snap with Snap To guides, grid, layers and
       document bounds; Proof Colors and Gamut Warning through the working CMYK; screen modes and
       Tab hiding the panels. Guides belong to the document; the rest is the window's view. */

    const float GUIDE_GRAB_PX = 4.0f;
    const float SNAP_PX       = 8.0f;

    bool App::guides_shown () const
    {
        return view_extras_ && view_guides_;
    }

    /* The ruler step (document px) for the zoom: 1-2-5 steps at least 50 screen px apart. */
    float ruler_step (float zoom)
    {
        const float steps[3] = {1.0f, 2.0f, 5.0f};
        float base           = 1.0f;
        for (int i = 0; i < 12; i++)
        {
            for (int k = 0; k < 3; k++)
            {
                float s = steps[k] * base;
                if (s * zoom >= 50.0f)
                {
                    return s;
                }
            }
            base *= 10.0f;
        }
        return base;
    }

    /* Rulers along the top and left of the canvas area, with the pointer marked; dragging out of
       one starts a guide. */
    void App::rulers (ImVec2 full, ImVec2 full_size, float ox, float oy)
    {
        float k = ui_scale_, r = ruler_px_;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImU32 bg = theme_.chrome, fg = theme_.text_dim, line = theme_.separator;
        dl->AddRectFilled (full, ImVec2 (full.x + full_size.x, full.y + r), bg);
        dl->AddRectFilled (full, ImVec2 (full.x + r, full.y + full_size.y), bg);
        dl->AddLine (ImVec2 (full.x, full.y + r - 1), ImVec2 (full.x + full_size.x, full.y + r - 1), line);
        dl->AddLine (ImVec2 (full.x + r - 1, full.y), ImVec2 (full.x + r - 1, full.y + full_size.y), line);
        float step   = ruler_step (zoom_);
        float minor  = step / (step >= 10.0f && fmodf (step, 5.0f) == 0.0f ? 5.0f : 2.0f);
        ImFont* font = fonts_.mono != NULL ? fonts_.mono : ImGui::GetFont ();
        float fs     = ImGui::GetFontSize () * 0.75f;
        for (int axis = 0; axis < 2; axis++)
        {
            float origin = axis == 0 ? ox : oy;
            float lo     = axis == 0 ? full.x + r : full.y + r,
                  hi     = axis == 0 ? full.x + full_size.x : full.y + full_size.y;
            dl->PushClipRect (axis == 0 ? ImVec2 (lo, full.y) : ImVec2 (full.x, lo),
                axis == 0 ? ImVec2 (hi, full.y + r) : ImVec2 (full.x + r, hi), true);
            float v0 = floorf ((lo - origin) / zoom_ / minor) * minor;
            for (float v = v0; origin + v * zoom_ <= hi; v += minor)
            {
                float s = floorf (origin + v * zoom_) + 0.5f;
                bool major =
                    fabsf (fmodf (v, step)) < minor * 0.01f || fabsf (fabsf (fmodf (v, step)) - step) < minor * 0.01f;
                float len = major ? r : r * 0.3f;
                if (axis == 0)
                {
                    dl->AddLine (ImVec2 (s, full.y + r - len), ImVec2 (s, full.y + r), fg);
                }
                else
                {
                    dl->AddLine (ImVec2 (full.x + r - len, s), ImVec2 (full.x + r, s), fg);
                }
                if (!major)
                {
                    continue;
                }
                char label[32];
                snprintf (label, sizeof label, "%g", v);
                if (axis == 0)
                {
                    dl->AddText (font, fs, ImVec2 (s + 2 * k, full.y + 1 * k), fg, label);
                }
                else
                {
                    /* Down the left ruler, one digit under another, as Photoshop prints them. */
                    for (int c = 0; label[c] != 0; c++)
                    {
                        char one[2] = {label[c], 0};
                        dl->AddText (font, fs, ImVec2 (full.x + 2 * k, s + 1 * k + (float) c * fs * 0.9f), fg, one);
                    }
                }
            }
            dl->PopClipRect ();
        }
        /* The pointer's position. */
        ImVec2 m = ImGui::GetIO ().MousePos;
        if (m.x >= full.x + r && m.x < full.x + full_size.x)
        {
            dl->AddLine (ImVec2 (m.x, full.y), ImVec2 (m.x, full.y + r), theme_.text);
        }
        if (m.y >= full.y + r && m.y < full.y + full_size.y)
        {
            dl->AddLine (ImVec2 (full.x, m.y), ImVec2 (full.x + r, m.y), theme_.text);
        }
        /* Dragging out of a ruler: the top one makes horizontal guides, the left one vertical. */
        for (int axis = 0; axis < 2; axis++)
        {
            ImGui::SetCursorScreenPos (axis == 0 ? ImVec2 (full.x + r, full.y) : ImVec2 (full.x, full.y + r));
            ImGui::InvisibleButton (axis == 0 ? "##hruler" : "##vruler",
                axis == 0 ? ImVec2 (full_size.x - r, r) : ImVec2 (r, full_size.y - r));
            if (ImGui::IsItemActivated () && session_.active () != NULL && !view_lock_guides_)
            {
                guide_drag_          = -2;
                guide_drag_vertical_ = axis == 1;
                view_guides_ = view_extras_ = true;
            }
        }
        ImGui::SetCursorScreenPos (full);
        ImGui::Dummy (ImVec2 (r, r));
    }

    /* Grid, guides (with the one being dragged), pixel grid and layer edges over the image. */
    void App::view_overlays (float ox, float oy, ImVec2 origin, ImVec2 avail)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || !view_extras_)
        {
            return;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float w = (float) d->doc->size.width, h = (float) d->doc->size.height;
        ImVec2 a (ox, oy), b (ox + w * zoom_, oy + h * zoom_);
        ImVec2 c0 (fmaxf (a.x, origin.x), fmaxf (a.y, origin.y)),
            c1 (fminf (b.x, origin.x + avail.x), fminf (b.y, origin.y + avail.y));
        /* Pixel grid at 600% and closer. */
        if (view_pixel_grid_ && zoom_ >= 6.0f)
        {
            for (float x = ceilf ((c0.x - ox) / zoom_); ox + x * zoom_ <= c1.x; x += 1.0f)
            {
                dl->AddLine (
                    ImVec2 (ox + x * zoom_, c0.y), ImVec2 (ox + x * zoom_, c1.y), IM_COL32 (128, 128, 128, 70));
            }
            for (float y = ceilf ((c0.y - oy) / zoom_); oy + y * zoom_ <= c1.y; y += 1.0f)
            {
                dl->AddLine (
                    ImVec2 (c0.x, oy + y * zoom_), ImVec2 (c1.x, oy + y * zoom_), IM_COL32 (128, 128, 128, 70));
            }
        }
        /* The grid: lines every grid_spacing_ px, lighter subdivisions between. */
        if (view_grid_)
        {
            /* Subdivisions only while they stay 8 screen px apart. */
            float sub = grid_spacing_ / (float) (grid_subdivisions_ > 0 ? grid_subdivisions_ : 1);
            if (sub * zoom_ < 8.0f)
            {
                sub = grid_spacing_;
            }
            if (sub * zoom_ >= 8.0f)
            {
                for (int axis = 0; axis < 2; axis++)
                {
                    float extent = axis == 0 ? w : h;
                    for (float v = 0.0f; v <= extent; v += sub)
                    {
                        bool major = fmodf (v + 0.01f, grid_spacing_) < 0.02f;
                        ImU32 col  = major ? IM_COL32 (128, 128, 128, 170) : IM_COL32 (128, 128, 128, 80);
                        if (axis == 0)
                        {
                            dl->AddLine (ImVec2 (floorf (ox + v * zoom_) + 0.5f, c0.y),
                                ImVec2 (floorf (ox + v * zoom_) + 0.5f, c1.y), col);
                        }
                        else
                        {
                            dl->AddLine (ImVec2 (c0.x, floorf (oy + v * zoom_) + 0.5f),
                                ImVec2 (c1.x, floorf (oy + v * zoom_) + 0.5f), col);
                        }
                    }
                }
            }
        }
        /* Layer edges: the active layer's bounds. */
        if (view_layer_edges_)
        {
            const doc::Layer* l      = d->doc->layer (d->active_layer);
            const raster::Surface* s = l != NULL ? l->surface () : NULL;
            if (s != NULL)
            {
                geom::Rect r = s->content_bounds ();
                if (!r.is_empty ())
                {
                    dl->AddRect (ImVec2 (ox + (float) r.x0 * zoom_, oy + (float) r.y0 * zoom_),
                        ImVec2 (ox + (float) r.x1 * zoom_, oy + (float) r.y1 * zoom_), IM_COL32 (40, 120, 255, 255));
                }
            }
        }
        /* Guides across the whole canvas area, Photoshop's cyan. */
        if (view_guides_)
        {
            ImU32 col = IM_COL32 (0, 210, 255, 255);
            for (size_t i = 0; i < d->doc->guides.size (); i++)
            {
                if ((int) i == guide_drag_)
                {
                    continue;
                }
                const doc::Guide& g = d->doc->guides[i];
                if (g.vertical)
                {
                    float x = floorf (ox + (float) g.position * zoom_) + 0.5f;
                    dl->AddLine (ImVec2 (x, origin.y), ImVec2 (x, origin.y + avail.y), col);
                }
                else
                {
                    float y = floorf (oy + (float) g.position * zoom_) + 0.5f;
                    dl->AddLine (ImVec2 (origin.x, y), ImVec2 (origin.x + avail.x, y), col);
                }
            }
        }
        if (guide_drag_ != -1)
        {
            ImU32 col = IM_COL32 (0, 210, 255, 255);
            if (guide_drag_vertical_)
            {
                float x = floorf (ox + guide_drag_pos_ * zoom_) + 0.5f;
                dl->AddLine (ImVec2 (x, origin.y), ImVec2 (x, origin.y + avail.y), col);
            }
            else
            {
                float y = floorf (oy + guide_drag_pos_ * zoom_) + 0.5f;
                dl->AddLine (ImVec2 (origin.x, y), ImVec2 (origin.x + avail.x, y), col);
            }
        }
    }

    /* The guide within reach of the pointer (screen px), or -1. */
    int App::guide_at (float ox, float oy)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || !guides_shown () || view_lock_guides_)
        {
            return -1;
        }
        ImVec2 m     = ImGui::GetIO ().MousePos;
        int best     = -1;
        float best_d = GUIDE_GRAB_PX + 0.5f;
        for (size_t i = 0; i < d->doc->guides.size (); i++)
        {
            const doc::Guide& g = d->doc->guides[i];
            float dd            = g.vertical ? fabsf (ox + (float) g.position * zoom_ - m.x)
                                             : fabsf (oy + (float) g.position * zoom_ - m.y);
            if (dd < best_d)
            {
                best   = (int) i;
                best_d = dd;
            }
        }
        return best;
    }

    /* Guides follow the pointer while dragged (from a ruler, or with the Move tool); released over
       the canvas they land, outside it (back on a ruler) they go. Shift snaps to the ruler's
       ticks. True while a guide is being dragged. */
    bool App::guide_input (float ox, float oy, ImVec2 origin, ImVec2 avail)
    {
        ImGuiIO& io = ImGui::GetIO ();
        if (guide_drag_ == -1 && tool_ == Tool::Move && ImGui::IsItemHovered ())
        {
            int g = guide_at (ox, oy);
            if (g >= 0)
            {
                const doc::Guide& gd = session_.active ()->doc->guides[(size_t) g];
                ImGui::SetMouseCursor (gd.vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left))
                {
                    guide_drag_          = g;
                    guide_drag_vertical_ = gd.vertical;
                }
            }
        }
        if (guide_drag_ == -1)
        {
            return false;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            guide_drag_ = -1;
            return false;
        }
        float pos = guide_drag_vertical_ ? (io.MousePos.x - ox) / zoom_ : (io.MousePos.y - oy) / zoom_;
        if (io.KeyShift)
        {
            float step = ruler_step (zoom_) / 2.0f;
            pos        = roundf (pos / step) * step;
        }
        else if (view_snap_)
        {
            float x = pos, y = pos;
            snap_point (&x, &y, guide_drag_vertical_, !guide_drag_vertical_, true);
            pos = guide_drag_vertical_ ? x : y;
        }
        guide_drag_pos_ = zoom_ >= 1.0f ? roundf (pos) : pos;
        ImGui::SetMouseCursor (guide_drag_vertical_ ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            return true;
        }
        bool inside = io.MousePos.x >= origin.x && io.MousePos.y >= origin.y && io.MousePos.x < origin.x + avail.x
                   && io.MousePos.y < origin.y + avail.y;
        int idx     = guide_drag_;
        guide_drag_ = -1;
        if (idx == -2)
        {
            if (inside)
            {
                run_command (
                    "view.newGuide", engine::Value{{"orientation", guide_drag_vertical_ ? "vertical" : "horizontal"},
                                         {"position", guide_drag_pos_}});
            }
        }
        else if (!inside)
        {
            run_command ("view.deleteGuide", engine::Value{{"index", idx}});
        }
        else
        {
            run_command ("view.moveGuide", engine::Value{{"index", idx}, {"position", guide_drag_pos_}});
        }
        return true;
    }

    /* Snap: a document point pulled to the nearest guide, grid line, layer edge or document edge
       within reach (each axis on its own when asked). `guides_too` is false while a guide itself
       moves. */
    void App::snap_point (float* x, float* y, bool sx, bool sy, bool from_guide)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || !view_snap_ || zoom_ <= 0.0f)
        {
            return;
        }
        float reach   = SNAP_PX / zoom_;
        float best[2] = {reach, reach};
        float out[2]  = {*x, *y};
        float in[2]   = {*x, *y};
        bool want[2]  = {sx, sy};
        for (int axis = 0; axis < 2; axis++)
        {
            if (!want[axis])
            {
                continue;
            }
            std::vector<float> targets;
            if (snap_bounds_)
            {
                targets.push_back (0.0f);
                targets.push_back (axis == 0 ? (float) d->doc->size.width : (float) d->doc->size.height);
            }
            if (snap_guides_ && guides_shown () && !from_guide)
            {
                for (size_t i = 0; i < d->doc->guides.size (); i++)
                {
                    if (d->doc->guides[i].vertical == (axis == 0))
                    {
                        targets.push_back ((float) d->doc->guides[i].position);
                    }
                }
            }
            if (snap_grid_ && view_extras_ && view_grid_)
            {
                float sub = grid_spacing_ / (float) (grid_subdivisions_ > 0 ? grid_subdivisions_ : 1);
                targets.push_back (roundf (in[axis] / sub) * sub);
            }
            if (snap_layers_)
            {
                std::vector<doc::WalkEntry> walk = d->doc->walk ();
                for (size_t i = 0; i < walk.size (); i++)
                {
                    const raster::Surface* s = walk[i].layer->visible ? walk[i].layer->surface () : NULL;
                    if (s == NULL || walk[i].layer->id == snap_skip_layer_)
                    {
                        continue;
                    }
                    geom::Rect r = s->content_bounds ();
                    if (r.is_empty ())
                    {
                        continue;
                    }
                    targets.push_back (axis == 0 ? (float) r.x0 : (float) r.y0);
                    targets.push_back (axis == 0 ? (float) r.x1 : (float) r.y1);
                }
            }
            for (size_t i = 0; i < targets.size (); i++)
            {
                float dd = fabsf (targets[i] - in[axis]);
                if (dd < best[axis])
                {
                    best[axis] = dd;
                    out[axis]  = targets[i];
                }
            }
        }
        *x = out[0];
        *y = out[1];
    }

    /* A move of a box (x0, y0, x1, y1) by (dx, dy), adjusted so one of its edges or its centre
       lands on a snap target. */
    void App::snap_box_move (const float box[4], float* dx, float* dy)
    {
        if (!view_snap_)
        {
            return;
        }
        float best[2] = {INFINITY, INFINITY};
        float adj[2]  = {0.0f, 0.0f};
        for (int e = 0; e < 3; e++)
        {
            float px = e == 0 ? box[0] : e == 1 ? box[2] : (box[0] + box[2]) * 0.5f;
            float py = e == 0 ? box[1] : e == 1 ? box[3] : (box[1] + box[3]) * 0.5f;
            float sx = px + *dx, sy = py + *dy, tx = sx, ty = sy;
            snap_point (&tx, &ty, true, true, false);
            if (tx != sx && fabsf (tx - sx) < best[0])
            {
                best[0] = fabsf (tx - sx);
                adj[0]  = tx - sx;
            }
            if (ty != sy && fabsf (ty - sy) < best[1])
            {
                best[1] = fabsf (ty - sy);
                adj[1]  = ty - sy;
            }
        }
        *dx += adj[0];
        *dy += adj[1];
    }

    /* Proof Colors: the image as the working CMYK prints it; Gamut Warning: what CMYK can't hold in
       gray. */
    void App::proof_filter (compose::Buffer* b)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || (!view_proof_ && !view_gamut_) || d->doc->mode == color::ColorMode::Cmyk)
        {
            return;
        }
        size_t n = b->pixel_count ();
#pragma omp parallel for schedule(static)
        for (ptrdiff_t ii = 0; ii < (ptrdiff_t) n; ii++)
        {
            float* p = &b->px[(size_t) ii * 4];
            float rgb[3], cmyk[4], back[3];
            for (int c = 0; c < 3; c++)
            {
                rgb[c] = fminf (fmaxf (p[c], 0.0f), 1.0f);
            }
            color::rgb_to_cmyk (session_.cmyk, rgb, cmyk);
            color::cmyk_to_rgb (session_.cmyk, cmyk, back);
            float diff = fmaxf (fabsf (back[0] - rgb[0]), fmaxf (fabsf (back[1] - rgb[1]), fabsf (back[2] - rgb[2])));
            if (view_gamut_ && diff > 0.04f && p[3] > 0.0f)
            {
                p[0] = p[1] = p[2] = 0.5f;
            }
            else if (view_proof_)
            {
                for (int c = 0; c < 3; c++)
                {
                    p[c] = back[c];
                }
            }
        }
    }

    /* Zoom presets that keep the view's centre. */
    void App::zoom_to (float z)
    {
        if (zoom_ <= 0.0f)
        {
            return;
        }
        zoom_by (z / zoom_, canvas_center_x_, canvas_center_y_);
    }

    /* Fit Layer(s) on Screen: the selected layers' bounds fill the view. */
    void App::fit_layers ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || zoom_ <= 0.0f)
        {
            return;
        }
        geom::Rect r;
        std::vector<doc::LayerId> sel = d->selected ();
        for (size_t i = 0; i < sel.size (); i++)
        {
            const doc::Layer* l = d->doc->layer (sel[i]);
            if (l != NULL && l->surface () != NULL)
            {
                r = r.unite (l->surface ()->content_bounds ());
            }
        }
        if (r.is_empty ())
        {
            return;
        }
        float z   = fminf (canvas_avail_x_ / (float) r.width (), canvas_avail_y_ / (float) r.height ()) * 0.95f;
        zoom_     = fminf (fmaxf (z, 0.02f), 32.0f);
        fit_view_ = false;
        float cx = (float) (r.x0 + r.x1) * 0.5f, cy = (float) (r.y0 + r.y1) * 0.5f;
        pan_x_ = -(cx - (float) d->doc->size.width * 0.5f) * zoom_;
        pan_y_ = -(cy - (float) d->doc->size.height * 0.5f) * zoom_;
    }

    void App::set_screen_mode (int mode)
    {
        screen_mode_ = (mode % 3 + 3) % 3;
        SDL_SetWindowFullscreen (window_, screen_mode_ != 0);
        if (screen_mode_ == 2 && !panels_hidden_)
        {
            toggle_panels ();
        }
        if (screen_mode_ == 0 && panels_hidden_)
        {
            toggle_panels ();
        }
    }

    /* Tab: the panels, toolbar and options bar out of the way (the canvas fills the window). */
    void App::toggle_panels ()
    {
        panels_hidden_ = !panels_hidden_;
        if (!panels_hidden_)
        {
            redock_canvas_ = true;
        }
    }

    void App::toggle_view (bool* flag)
    {
        *flag = !*flag;
        if (flag == &view_proof_ || flag == &view_gamut_)
        {
            channel_view_dirty_ = true;
        }
    }

    /* View menu commands from shortcuts and the menu. */
    bool App::view_command (const char* id)
    {
        struct Toggle
        {
            const char* id;
            bool* flag;
        };
        Toggle toggles[] = {{"view.rulers", &view_rulers_}, {"view.extras", &view_extras_},
            {"view.show.grid", &view_grid_}, {"view.show.guides", &view_guides_},
            {"view.show.selectionEdges", &view_sel_edges_}, {"view.show.layerEdges", &view_layer_edges_},
            {"view.show.pixelGrid", &view_pixel_grid_}, {"view.snap", &view_snap_},
            {"view.snapTo.guides", &snap_guides_}, {"view.snapTo.grid", &snap_grid_},
            {"view.snapTo.layers", &snap_layers_}, {"view.snapTo.documentBounds", &snap_bounds_},
            {"view.lockGuides", &view_lock_guides_}, {"view.proofColors", &view_proof_},
            {"view.gamutWarning", &view_gamut_}};
        for (size_t i = 0; i < sizeof toggles / sizeof toggles[0]; i++)
        {
            if (strcmp (id, toggles[i].id) != 0)
            {
                continue;
            }
            toggle_view (toggles[i].flag);
            /* Showing grid or guides turns Extras back on, as in Photoshop. */
            if ((toggles[i].flag == &view_grid_ || toggles[i].flag == &view_guides_) && *toggles[i].flag)
            {
                view_extras_ = true;
            }
            return true;
        }
        if (strcmp (id, "view.show.all") == 0)
        {
            view_extras_ = view_grid_ = view_guides_ = view_sel_edges_ = view_layer_edges_ = view_pixel_grid_ = true;
        }
        else if (strcmp (id, "view.snapTo.all") == 0)
        {
            bool all     = snap_guides_ && snap_grid_ && snap_layers_ && snap_bounds_;
            snap_guides_ = snap_grid_ = snap_layers_ = snap_bounds_ = !all;
        }
        else if (strcmp (id, "view.twoHundredPercent") == 0)
        {
            zoom_to (2.0f);
        }
        else if (strcmp (id, "view.printSize") == 0)
        {
            const engine::DocState* d = session_.active ();
            if (d != NULL)
            {
                zoom_to (96.0f / d->doc->resolution_dpi);
            }
        }
        else if (strcmp (id, "view.fitLayersOnScreen") == 0)
        {
            fit_layers ();
        }
        else if (strcmp (id, "view.screenMode.standard") == 0)
        {
            set_screen_mode (0);
        }
        else if (strcmp (id, "view.screenMode.fullScreenWithMenuBar") == 0)
        {
            set_screen_mode (1);
        }
        else if (strcmp (id, "view.screenMode.fullScreen") == 0)
        {
            set_screen_mode (2);
        }
        else if (strcmp (id, "view.screenMode.next") == 0)
        {
            set_screen_mode (screen_mode_ + 1);
        }
        else if (strcmp (id, "view.togglePanels") == 0)
        {
            toggle_panels ();
        }
        else if (strcmp (id, "view.newGuideDialog") == 0)
        {
            show_new_guide_ = true;
        }
        else if (strcmp (id, "view.newGuideLayoutDialog") == 0)
        {
            show_guide_layout_ = true;
        }
        else if (strcmp (id, "view.gridOptions") == 0)
        {
            show_grid_options_ = true;
        }
        else
        {
            return false;
        }
        return true;
    }

    void App::view_menu ()
    {
        bool doc = session_.active () != NULL;
        struct Item
        {
            const char* label;
            const char* id;
            const char* shortcut;
            bool* check;
        };
        if (ImGui::MenuItem ("Proof Colors", "Ctrl+Y", view_proof_, doc))
        {
            view_command ("view.proofColors");
        }
        if (ImGui::MenuItem ("Gamut Warning", "Ctrl+Shift+Y", view_gamut_, doc))
        {
            view_command ("view.gamutWarning");
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Zoom In", "Ctrl+=", false, doc))
        {
            run_command ("view.zoomIn", engine::Value::object ());
        }
        if (ImGui::MenuItem ("Zoom Out", "Ctrl+-", false, doc))
        {
            run_command ("view.zoomOut", engine::Value::object ());
        }
        if (ImGui::MenuItem ("Fit on Screen", "Ctrl+0", false, doc))
        {
            run_command ("view.fitOnScreen", engine::Value::object ());
        }
        if (ImGui::MenuItem ("Fit Layer(s) on Screen", NULL, false, doc))
        {
            view_command ("view.fitLayersOnScreen");
        }
        if (ImGui::MenuItem ("100%", "Ctrl+1", false, doc))
        {
            run_command ("view.actualPixels", engine::Value::object ());
        }
        if (ImGui::MenuItem ("200%", NULL, false, doc))
        {
            view_command ("view.twoHundredPercent");
        }
        if (ImGui::MenuItem ("Print Size", NULL, false, doc))
        {
            view_command ("view.printSize");
        }
        ImGui::Separator ();
        if (ImGui::BeginMenu ("Screen Mode"))
        {
            if (ImGui::MenuItem ("Standard Screen Mode", NULL, screen_mode_ == 0))
            {
                view_command ("view.screenMode.standard");
            }
            if (ImGui::MenuItem ("Full Screen Mode With Menu Bar", NULL, screen_mode_ == 1))
            {
                view_command ("view.screenMode.fullScreenWithMenuBar");
            }
            if (ImGui::MenuItem ("Full Screen Mode", NULL, screen_mode_ == 2))
            {
                view_command ("view.screenMode.fullScreen");
            }
            ImGui::Separator ();
            ImGui::TextDisabled ("F: next mode    Tab: hide panels");
            ImGui::EndMenu ();
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Extras", "Ctrl+H", view_extras_))
        {
            view_command ("view.extras");
        }
        if (ImGui::BeginMenu ("Show"))
        {
            Item show[] = {{"Layer Edges", "view.show.layerEdges", NULL, &view_layer_edges_},
                {"Selection Edges", "view.show.selectionEdges", NULL, &view_sel_edges_},
                {"Grid", "view.show.grid", "Ctrl+'", &view_grid_},
                {"Guides", "view.show.guides", "Ctrl+;", &view_guides_},
                {"Pixel Grid", "view.show.pixelGrid", NULL, &view_pixel_grid_}};
            for (size_t i = 0; i < sizeof show / sizeof show[0]; i++)
            {
                if (ImGui::MenuItem (show[i].label, show[i].shortcut, *show[i].check))
                {
                    view_command (show[i].id);
                }
            }
            ImGui::Separator ();
            if (ImGui::MenuItem ("All"))
            {
                view_command ("view.show.all");
            }
            if (ImGui::MenuItem ("Grid Options…"))
            {
                view_command ("view.gridOptions");
            }
            ImGui::EndMenu ();
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Rulers", "Ctrl+R", view_rulers_))
        {
            view_command ("view.rulers");
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Snap", "Ctrl+Shift+;", view_snap_))
        {
            view_command ("view.snap");
        }
        if (ImGui::BeginMenu ("Snap To", view_snap_))
        {
            Item to[] = {{"Guides", "view.snapTo.guides", NULL, &snap_guides_},
                {"Grid", "view.snapTo.grid", NULL, &snap_grid_}, {"Layers", "view.snapTo.layers", NULL, &snap_layers_},
                {"Document Bounds", "view.snapTo.documentBounds", NULL, &snap_bounds_}};
            for (size_t i = 0; i < sizeof to / sizeof to[0]; i++)
            {
                if (ImGui::MenuItem (to[i].label, NULL, *to[i].check))
                {
                    view_command (to[i].id);
                }
            }
            ImGui::Separator ();
            if (ImGui::MenuItem ("All", NULL, snap_guides_ && snap_grid_ && snap_layers_ && snap_bounds_))
            {
                view_command ("view.snapTo.all");
            }
            ImGui::EndMenu ();
        }
        ImGui::Separator ();
        if (ImGui::MenuItem ("Lock Guides", "Ctrl+Alt+;", view_lock_guides_))
        {
            view_command ("view.lockGuides");
        }
        command_item ("view.clearGuides", NULL);
        if (ImGui::MenuItem ("New Guide…", NULL, false, doc))
        {
            view_command ("view.newGuideDialog");
        }
        if (ImGui::MenuItem ("New Guide Layout…", NULL, false, doc))
        {
            view_command ("view.newGuideLayoutDialog");
        }
    }

    /* View shortcuts (Photoshop's), unless a text field has the keyboard. */
    void App::view_shortcuts ()
    {
        if (ImGui::GetIO ().WantTextInput)
        {
            return;
        }
        struct Key
        {
            ImGuiKeyChord chord;
            const char* id;
        };
        const Key keys[] = {{ImGuiMod_Ctrl | ImGuiKey_R, "view.rulers"}, {ImGuiMod_Ctrl | ImGuiKey_H, "view.extras"},
            {ImGuiMod_Ctrl | ImGuiKey_Apostrophe, "view.show.grid"},
            {ImGuiMod_Ctrl | ImGuiKey_Semicolon, "view.show.guides"},
            {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Semicolon, "view.snap"},
            {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_Semicolon, "view.lockGuides"},
            {ImGuiMod_Ctrl | ImGuiKey_Y, "view.proofColors"},
            {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Y, "view.gamutWarning"}, {ImGuiKey_F, "view.screenMode.next"},
            {ImGuiKey_Tab, "view.togglePanels"}};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        {
            if (ImGui::Shortcut (keys[i].chord, ImGuiInputFlags_RouteGlobal))
            {
                view_command (keys[i].id);
            }
        }
    }

    void App::new_guide_dialog ()
    {
        if (!show_new_guide_)
        {
            return;
        }
        ImGui::OpenPopup ("New Guide###newguide");
        if (!ImGui::BeginPopupModal ("New Guide###newguide", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        ImGui::RadioButton ("Horizontal", &guide_dialog_vertical_, 0);
        ImGui::SameLine ();
        ImGui::RadioButton ("Vertical", &guide_dialog_vertical_, 1);
        ImGui::SetNextItemWidth (160 * k);
        ImGui::InputFloat ("Position (px)", &guide_dialog_pos_, 1.0f, 10.0f, "%.1f");
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            show_new_guide_ = false;
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                view_guides_ = view_extras_ = true;
                run_command (
                    "view.newGuide", engine::Value{{"orientation", guide_dialog_vertical_ ? "vertical" : "horizontal"},
                                         {"position", guide_dialog_pos_}});
            }
        }
        ImGui::EndPopup ();
    }

    void App::guide_layout_dialog ()
    {
        if (!show_guide_layout_)
        {
            return;
        }
        ImGui::OpenPopup ("New Guide Layout###guidelayout");
        if (!ImGui::BeginPopupModal ("New Guide Layout###guidelayout", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_, w = 110 * k;
        ImGui::SeparatorText ("Columns");
        ImGui::SetNextItemWidth (w);
        ImGui::InputInt ("Number##cols", &layout_cols_);
        ImGui::SetNextItemWidth (w);
        ImGui::InputFloat ("Gutter (px)##cg", &layout_col_gutter_, 1.0f, 10.0f, "%.0f");
        ImGui::SeparatorText ("Rows");
        ImGui::SetNextItemWidth (w);
        ImGui::InputInt ("Number##rows", &layout_rows_);
        ImGui::SetNextItemWidth (w);
        ImGui::InputFloat ("Gutter (px)##rg", &layout_row_gutter_, 1.0f, 10.0f, "%.0f");
        ImGui::SeparatorText ("Margin (px)");
        const char* ml[4] = {"Top", "Left", "Bottom", "Right"};
        for (int i = 0; i < 4; i++)
        {
            if (i % 2 == 1)
            {
                ImGui::SameLine ();
            }
            ImGui::SetNextItemWidth (w);
            ImGui::InputFloat (ml[i], &layout_margin_[i], 0, 0, "%.0f");
        }
        ImGui::Checkbox ("Clear Existing Guides", &layout_clear_);
        layout_cols_ = layout_cols_ < 0 ? 0 : layout_cols_ > 1000 ? 1000 : layout_cols_;
        layout_rows_ = layout_rows_ < 0 ? 0 : layout_rows_ > 1000 ? 1000 : layout_rows_;
        bool ok      = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            show_guide_layout_ = false;
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                view_guides_ = view_extras_ = true;
                run_command (
                    "view.newGuideLayout", engine::Value{{"columns", layout_cols_}, {"rows", layout_rows_},
                                               {"columnGutter", layout_col_gutter_}, {"rowGutter", layout_row_gutter_},
                                               {"marginTop", layout_margin_[0]}, {"marginLeft", layout_margin_[1]},
                                               {"marginBottom", layout_margin_[2]}, {"marginRight", layout_margin_[3]},
                                               {"clearExisting", layout_clear_}});
            }
        }
        ImGui::EndPopup ();
    }

    /* Preferences > Guides, Grid: the grid's spacing and subdivisions. */
    void App::grid_options_dialog ()
    {
        if (!show_grid_options_)
        {
            return;
        }
        ImGui::OpenPopup ("Grid###gridoptions");
        if (!ImGui::BeginPopupModal ("Grid###gridoptions", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        ImGui::SetNextItemWidth (140 * k);
        ImGui::InputFloat ("Gridline Every (px)", &grid_spacing_, 1.0f, 10.0f, "%.0f");
        ImGui::SetNextItemWidth (140 * k);
        ImGui::InputInt ("Subdivisions", &grid_subdivisions_);
        grid_spacing_      = fminf (fmaxf (grid_spacing_, 1.0f), 10000.0f);
        grid_subdivisions_ = grid_subdivisions_ < 1 ? 1 : grid_subdivisions_ > 100 ? 100 : grid_subdivisions_;
        if (ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
            || ImGui::IsKeyPressed (ImGuiKey_Escape))
        {
            show_grid_options_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

} // namespace ui
