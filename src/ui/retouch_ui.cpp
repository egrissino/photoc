#include "compose/effects.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <imgui.h>
#include <math.h>
#include <string.h>

namespace ui
{

    /* The retouching tools on the canvas. Clone Stamp and Healing Brush sample from a source set
       with Alt-click; Aligned keeps the offset from the first stroke, otherwise every stroke starts
       at the source again. Strokes preview live (the Healing Brush shows its texture, blended on
       release; Spot Healing shows its trail) and commit as one command on release. */

    const char* const CLONE_SAMPLES[] = {"current", "currentAndBelow", "all"};
    const char* const TONE_RANGES[]   = {"shadows", "midtones", "highlights"};
    const char* const SAMPLINGS[]     = {"continuous", "once", "backgroundSwatch"};
    const char* const LIMITS[]        = {"contiguous", "discontiguous", "findEdges"};
    const char* const REPLACE_MODES[] = {"hue", "saturation", "color", "luminosity"};
    const char* const ART_STYLE_IDS[] = {"tightShort", "tightMedium", "tightLong", "looseMedium", "looseLong", "dab",
        "tightCurl", "tightCurlLong", "looseCurl", "looseCurlLong"};

    const char* retouch_command (Tool t)
    {
        switch (t)
        {
        case Tool::CloneStamp:
            return "paint.cloneStamp";
        case Tool::SpotHealing:
            return "paint.spotHealing";
        case Tool::HealingBrush:
            return "paint.healingBrush";
        case Tool::HistoryBrush:
            return "paint.historyBrush";
        case Tool::Dodge:
            return "paint.dodge";
        case Tool::Burn:
            return "paint.burn";
        case Tool::Sponge:
            return "paint.sponge";
        case Tool::Blur:
            return "paint.blur";
        case Tool::Sharpen:
            return "paint.sharpen";
        case Tool::Smudge:
            return "paint.smudge";
        case Tool::ColorReplacement:
            return "paint.colorReplacement";
        case Tool::MixerBrush:
            return "paint.mixerBrush";
        case Tool::PatternStamp:
            return "paint.patternStamp";
        case Tool::ArtHistory:
            return "paint.artHistory";
        case Tool::BackgroundEraser:
            return "paint.backgroundEraser";
        case Tool::Remove:
            return "paint.remove";
        default:
            return NULL;
        }
    }

    /* The stroke's params without its points: the brush and the tool's options. */
    engine::Value App::retouch_params ()
    {
        const paint::BrushSettings& b = session_.tools.brush;
        engine::Value p = {{"size", b.size}, {"hardness", b.hardness * 100.0f}, {"opacity", b.opacity * 100.0f},
            {"flow", b.flow * 100.0f}};
        const engine::DocState* d = session_.active ();
        if (d != NULL)
        {
            p["layer"] = d->active_layer;
        }
        if (mask_targeted ())
        {
            p["target"] = "mask";
        }
        switch (tool_)
        {
        case Tool::CloneStamp:
        case Tool::HealingBrush:
            p["aligned"]     = clone_aligned_;
            p["sampleLayer"] = CLONE_SAMPLES[clone_sample_];
            if (clone_aligned_ && has_clone_offset_)
            {
                p["offset"] = {clone_offset_[0], clone_offset_[1]};
            }
            else
            {
                p["source"] = {clone_source_[0], clone_source_[1]};
            }
            break;
        case Tool::SpotHealing:
            p["sampleAllLayers"] = spot_all_layers_;
            p["type"]            = spot_type_ == 1 ? "contentAware" : "proximityMatch";
            break;
        case Tool::Remove:
            p["sampleAllLayers"] = remove_all_layers_;
            break;
        case Tool::BackgroundEraser:
            p["sampling"]          = SAMPLINGS[sample_mode_];
            p["limits"]            = LIMITS[limits_mode_];
            p["tolerance"]         = bg_tolerance_;
            p["protectForeground"] = bg_protect_;
            break;
        case Tool::ColorReplacement:
            p["mode"]      = REPLACE_MODES[replace_mode_];
            p["sampling"]  = SAMPLINGS[sample_mode_];
            p["limits"]    = LIMITS[limits_mode_];
            p["tolerance"] = cr_tolerance_;
            break;
        case Tool::MixerBrush:
            p["wet"]             = mixer_wet_;
            p["load"]            = mixer_load_;
            p["mix"]             = mixer_mix_;
            p["loaded"]          = mixer_loaded_;
            p["sampleAllLayers"] = mixer_all_layers_;
            break;
        case Tool::PatternStamp:
            p["pattern"] = pattern_index_;
            p["aligned"] = pattern_aligned_;
            break;
        case Tool::ArtHistory:
            p["style"]     = ART_STYLE_IDS[art_style_];
            p["area"]      = art_area_;
            p["tolerance"] = art_tolerance_;
            break;
        case Tool::Dodge:
        case Tool::Burn:
            p["range"]        = TONE_RANGES[tone_range_];
            p["exposure"]     = tone_exposure_;
            p["protectTones"] = tone_protect_;
            break;
        case Tool::Sponge:
            p["mode"]     = sponge_saturate_ ? "saturate" : "desaturate";
            p["vibrance"] = sponge_vibrance_;
            break;
        case Tool::Blur:
        case Tool::Sharpen:
            p["strength"]        = focus_strength_;
            p["sampleAllLayers"] = focus_all_layers_;
            break;
        case Tool::Smudge:
            p["strength"]        = focus_strength_;
            p["fingerPainting"]  = smudge_finger_;
            p["sampleAllLayers"] = focus_all_layers_;
            break;
        default:
            break;
        }
        return p;
    }

    void App::end_retouch ()
    {
        if (!retouching_)
        {
            return;
        }
        retouching_ = false;
        live_retouch_.reset ();
        live_dab_.reset ();
        clear_preview ();
        if (retouch_points_.size () < 2)
        {
            return;
        }
        engine::Value pts = engine::Value::array ();
        for (size_t i = 0; i + 1 < retouch_points_.size (); i += 2)
        {
            pts.push_back ({retouch_points_[i], retouch_points_[i + 1]});
        }
        retouch_points_.clear ();
        engine::Value p = retouch_params_;
        p["points"]     = pts;
        engine::Value result;
        engine::Error err;
        if (session_.execute (retouch_cmd_.c_str (), p, &result, &err))
        {
            const engine::CommandSpec* spec = session_.find_command (retouch_cmd_.c_str ());
            status_                         = spec != NULL ? spec->label : retouch_cmd_;
            status_is_error_                = false;
            /* Aligned: later strokes keep this stroke's offset. */
            if (clone_aligned_ && result.contains ("offset") && result["offset"].is_array ())
            {
                has_clone_offset_ = true;
                clone_offset_[0]  = result["offset"][0].get<int32_t> ();
                clone_offset_[1]  = result["offset"][1].get<int32_t> ();
            }
        }
        else
        {
            set_error (err.message);
        }
    }

    bool App::retouch_tool_input (float ox, float oy)
    {
        const char* cmd = retouch_command (tool_);
        if (cmd == NULL)
        {
            if (retouching_)
            {
                end_retouch ();
            }
            return false;
        }
        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        bool sources   = tool_ == Tool::CloneStamp || tool_ == Tool::HealingBrush;
        if (activated && sources && io.KeyAlt)
        {
            has_clone_source_ = true;
            has_clone_offset_ = false;
            clone_source_[0]  = mx;
            clone_source_[1]  = my;
            status_           = "Clone source set";
            status_is_error_  = false;
            return true;
        }
        if (activated)
        {
            const engine::CommandSpec* spec = session_.find_command (cmd);
            std::string why;
            if (spec == NULL || !session_.command_enabled (*spec, &why))
            {
                set_error (why);
                return true;
            }
            if (sources && !has_clone_source_)
            {
                set_error ("Alt-click to define a source point first");
                return true;
            }
            retouching_     = true;
            retouch_cmd_    = cmd;
            retouch_params_ = retouch_params ();
            retouch_points_.assign (1, mx);
            retouch_points_.push_back (my);
            engine::Value p = retouch_params_;
            p["points"]     = engine::Value::array ();
            p["points"].push_back ({mx, my});
            engine::Error err;
            /* Live preview: clone-style strokes and sequential-dab tools; Spot Healing and Sample All
               Layers strokes show their trail only. */
            bool all = retouch_params_.contains ("sampleAllLayers") && retouch_params_["sampleAllLayers"] == true;
            if (tool_ == Tool::CloneStamp || tool_ == Tool::HealingBrush || tool_ == Tool::HistoryBrush
                || tool_ == Tool::PatternStamp)
            {
                live_retouch_.reset (new engine::LiveRetouch ());
                if (!live_retouch_->begin (session_, tool_ == Tool::HealingBrush ? "paint.cloneStamp" : cmd, p, &err))
                {
                    live_retouch_.reset ();
                }
            }
            else if (tool_ != Tool::SpotHealing && tool_ != Tool::Remove && tool_ != Tool::ArtHistory && !all)
            {
                live_dab_.reset (new engine::LiveDab ());
                if (!live_dab_->begin (session_, cmd, p, &err))
                {
                    live_dab_.reset ();
                }
            }
            return true;
        }
        if (!retouching_)
        {
            return true;
        }
        if (active)
        {
            size_t n = retouch_points_.size ();
            float dx = mx - retouch_points_[n - 2], dy = my - retouch_points_[n - 1];
            if (dx * dx + dy * dy >= 0.25f)
            {
                retouch_points_.push_back (mx);
                retouch_points_.push_back (my);
                paint::StrokePoint sp = paint::StrokePoint::make (mx, my, 1.0f);
                geom::Rect dmg;
                std::shared_ptr<doc::Document> live;
                if (live_retouch_)
                {
                    dmg  = live_retouch_->push (&sp, 1);
                    live = live_retouch_->doc;
                }
                else if (live_dab_)
                {
                    dmg  = live_dab_->push (&sp, 1);
                    live = live_dab_->doc;
                }
                if (live)
                {
                    const doc::Layer* styled = live->layer (session_.active ()->active_layer);
                    if (styled != NULL && !dmg.is_empty () && compose::has_effects (*styled))
                    {
                        dmg = dmg.inflate (2 * compose::effects_margin (*styled));
                    }
                    stroke_damage_ = stroke_damage_.unite (dmg);
                    preview_doc_   = live;
                    preview_serial_++;
                }
            }
        }
        if (released)
        {
            end_retouch ();
        }
        return true;
    }

    void App::retouch_tool_overlay (float ox, float oy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        bool sources   = tool_ == Tool::CloneStamp || tool_ == Tool::HealingBrush;
        if (sources && has_clone_source_)
        {
            /* The source crosshair: where the next dab samples, following the pointer while
               painting with an offset. */
            float sx = clone_source_[0], sy = clone_source_[1];
            if (retouching_ && retouch_points_.size () >= 2)
            {
                float dx = clone_aligned_ && has_clone_offset_ ? (float) clone_offset_[0]
                                                               : clone_source_[0] - retouch_points_[0];
                float dy = clone_aligned_ && has_clone_offset_ ? (float) clone_offset_[1]
                                                               : clone_source_[1] - retouch_points_[1];
                sx       = retouch_points_[retouch_points_.size () - 2] + dx;
                sy       = retouch_points_.back () + dy;
            }
            ImVec2 c (ox + sx * zoom_, oy + sy * zoom_);
            dl->AddLine (ImVec2 (c.x - 8, c.y), ImVec2 (c.x + 8, c.y), IM_COL32 (0, 0, 0, 220), 3.0f);
            dl->AddLine (ImVec2 (c.x, c.y - 8), ImVec2 (c.x, c.y + 8), IM_COL32 (0, 0, 0, 220), 3.0f);
            dl->AddLine (ImVec2 (c.x - 8, c.y), ImVec2 (c.x + 8, c.y), IM_COL32 (255, 255, 255, 255), 1.0f);
            dl->AddLine (ImVec2 (c.x, c.y - 8), ImVec2 (c.x, c.y + 8), IM_COL32 (255, 255, 255, 255), 1.0f);
        }
        if (retouching_ && !live_retouch_ && !live_dab_ && retouch_points_.size () >= 2)
        {
            /* The trail of strokes without a live preview, at the brush's width. */
            std::vector<ImVec2> pts;
            for (size_t i = 0; i + 1 < retouch_points_.size (); i += 2)
            {
                pts.push_back (ImVec2 (ox + retouch_points_[i] * zoom_, oy + retouch_points_[i + 1] * zoom_));
            }
            float wpx = fmaxf (session_.tools.brush.size * zoom_, 1.0f);
            if (pts.size () == 1)
            {
                dl->AddCircleFilled (pts[0], wpx * 0.5f, IM_COL32 (255, 255, 255, 90));
            }
            else
            {
                dl->AddPolyline (pts.data (), (int) pts.size (), IM_COL32 (255, 255, 255, 90), 0, wpx);
            }
        }
    }

    void App::retouch_options ()
    {
        if (retouch_command (tool_) == NULL)
        {
            return;
        }
        float k = ui_scale_;
        switch (tool_)
        {
        case Tool::CloneStamp:
        case Tool::HealingBrush:
            if (ImGui::Checkbox ("Aligned", &clone_aligned_))
            {
                has_clone_offset_ = false;
            }
            opt_gap ();
            opt_label ("Sample:");
            ImGui::SetNextItemWidth (130 * k);
            ImGui::Combo ("##sample", &clone_sample_, "Current Layer\0Current & Below\0All Layers\0");
            opt_gap ();
            break;
        case Tool::SpotHealing:
            opt_label ("Type:");
            ImGui::SetNextItemWidth (130 * k);
            ImGui::Combo ("##spottype", &spot_type_, "Proximity Match\0Content-Aware\0");
            opt_gap ();
            ImGui::Checkbox ("Sample All Layers", &spot_all_layers_);
            opt_gap ();
            break;
        case Tool::Dodge:
        case Tool::Burn:
            opt_label ("Range:");
            ImGui::SetNextItemWidth (100 * k);
            ImGui::Combo ("##range", &tone_range_, "Shadows\0Midtones\0Highlights\0");
            opt_gap ();
            opt_label ("Exposure:");
            ImGui::SetNextItemWidth (90 * k);
            ImGui::DragFloat (
                "##exposure", &tone_exposure_, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            ImGui::Checkbox ("Protect Tones", &tone_protect_);
            opt_gap ();
            break;
        case Tool::Sponge:
            {
                int mode = sponge_saturate_ ? 1 : 0;
                opt_label ("Mode:");
                ImGui::SetNextItemWidth (100 * k);
                if (ImGui::Combo ("##spongemode", &mode, "Desaturate\0Saturate\0"))
                {
                    sponge_saturate_ = mode == 1;
                }
                opt_gap ();
                ImGui::Checkbox ("Vibrance", &sponge_vibrance_);
                opt_gap ();
                break;
            }
        case Tool::Blur:
        case Tool::Sharpen:
        case Tool::Smudge:
            opt_label ("Strength:");
            ImGui::SetNextItemWidth (90 * k);
            ImGui::DragFloat (
                "##strength", &focus_strength_, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            ImGui::Checkbox ("Sample All Layers", &focus_all_layers_);
            opt_gap ();
            if (tool_ == Tool::Smudge)
            {
                ImGui::Checkbox ("Finger Painting", &smudge_finger_);
                opt_gap ();
            }
            break;
        case Tool::Remove:
            ImGui::Checkbox ("Sample All Layers", &remove_all_layers_);
            opt_gap ();
            ImGui::TextDisabled ("Paint over what to remove.");
            break;
        case Tool::BackgroundEraser:
        case Tool::ColorReplacement:
            if (tool_ == Tool::ColorReplacement)
            {
                opt_label ("Mode:");
                ImGui::SetNextItemWidth (100 * k);
                ImGui::Combo ("##crmode", &replace_mode_, "Hue\0Saturation\0Color\0Luminosity\0");
                opt_gap ();
            }
            opt_label ("Sampling:");
            ImGui::SetNextItemWidth (140 * k);
            ImGui::Combo ("##sampling", &sample_mode_, "Continuous\0Once\0Background Swatch\0");
            opt_gap ();
            opt_label ("Limits:");
            ImGui::SetNextItemWidth (110 * k);
            ImGui::Combo ("##limits", &limits_mode_, "Contiguous\0Discontiguous\0Find Edges\0");
            opt_gap ();
            opt_label ("Tolerance:");
            ImGui::SetNextItemWidth (80 * k);
            ImGui::DragFloat ("##tol", tool_ == Tool::BackgroundEraser ? &bg_tolerance_ : &cr_tolerance_, 0.4f, 0.0f,
                100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            if (tool_ == Tool::BackgroundEraser)
            {
                ImGui::Checkbox ("Protect Foreground Color", &bg_protect_);
                opt_gap ();
            }
            break;
        case Tool::MixerBrush:
            ImGui::Checkbox ("Load", &mixer_loaded_);
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip (
                    "Load the brush with the foreground colour at each stroke (off: a clean brush that only mixes)");
            }
            opt_gap ();
            opt_label ("Wet:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##wet", &mixer_wet_, 0.4f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            opt_label ("Load:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##load", &mixer_load_, 0.4f, 1.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            opt_label ("Mix:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##mix", &mixer_mix_, 0.4f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            ImGui::Checkbox ("Sample All Layers", &mixer_all_layers_);
            opt_gap ();
            break;
        case Tool::PatternStamp:
            opt_label ("Pattern:");
            pattern_combo ("##pattern", &pattern_index_, 150 * k);
            opt_gap ();
            ImGui::Checkbox ("Aligned", &pattern_aligned_);
            opt_gap ();
            break;
        case Tool::ArtHistory:
            opt_label ("Style:");
            ImGui::SetNextItemWidth (130 * k);
            ImGui::Combo ("##artstyle", &art_style_,
                "Tight Short\0Tight Medium\0Tight Long\0Loose Medium\0Loose Long\0Dab\0Tight Curl\0Tight Curl "
                "Long\0Loose Curl\0Loose Curl Long\0");
            opt_gap ();
            opt_label ("Area:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##artarea", &art_area_, 0.5f, 0.0f, 500.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            opt_label ("Tolerance:");
            ImGui::SetNextItemWidth (70 * k);
            ImGui::DragFloat ("##arttol", &art_tolerance_, 0.4f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
            opt_gap ();
            break;
        default:
            break;
        }
    }

} // namespace ui
