#include "ui/adjust_ui.h"

#include "compose/adjust.h"
#include "engine/adjust.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* Controls for one adjustment, editing the same parameter set the engine commands take
       (engine::adjustment_to_params). `changed` is set whenever a value moved this frame (redraw the
       preview); `finished` when an edit is complete (commit it): a slider released, a box ticked. */

    using doc::AdjustmentKind;
    using engine::Value;

    const char* const HUE_KEYS[6]       = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
    const char* const HUE_LABELS[6]     = {"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    const char* const SELECTIVE_KEYS[9] = {
        "reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};
    const char* const SELECTIVE_LABELS[9] = {
        "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas", "Whites", "Neutrals", "Blacks"};
    const char* const CHANNEL_KEYS[10] = {
        "red", "green", "blue", "cyan", "magenta", "yellow", "black", "lightness", "a", "b"};
    const char* const CHANNEL_LABELS[10] = {
        "Red", "Green", "Blue", "Cyan", "Magenta", "Yellow", "Black", "Lightness", "a", "b"};

    void adjust_editor_init (AdjustEditor* ed, const doc::Adjustment& adj)
    {
        ed->kind       = adj.kind;
        ed->params     = engine::adjustment_to_params (adj);
        ed->channel    = 0;
        ed->tone       = 1;
        ed->row        = 0;
        ed->curve_drag = -1;
    }

    /* Kinds whose menu item opens a dialog (labels ending in "..."): all but Invert. */
    bool adjust_has_dialog (AdjustmentKind kind)
    {
        return kind != AdjustmentKind::Invert && kind != AdjustmentKind::Unsupported;
    }

    float get_num (const Value& obj, const char* key, float def)
    {
        const Value* v = engine::param (obj, key);
        return v != NULL && v->is_number () ? (float) v->get<double> () : def;
    }

    bool get_bool (const Value& obj, const char* key, bool def)
    {
        const Value* v = engine::param (obj, key);
        return v != NULL && v->is_boolean () ? v->get<bool> () : def;
    }

    /* A slider over obj[key]. */
    void slider (Value& obj, const char* key, const char* label, float lo, float hi, float def, const char* format,
        bool log_scale, bool* changed, bool* finished)
    {
        float v = get_num (obj, key, def);
        if (ImGui::SliderFloat (label, &v, lo, hi, format, log_scale ? ImGuiSliderFlags_Logarithmic : 0))
        {
            obj[key] = v;
            *changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit ())
        {
            *finished = true;
        }
    }

    void slider_int (
        Value& obj, const char* key, const char* label, int lo, int hi, int def, bool* changed, bool* finished)
    {
        int v = (int) lroundf (get_num (obj, key, (float) def));
        if (ImGui::SliderInt (label, &v, lo, hi))
        {
            obj[key] = v;
            *changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit ())
        {
            *finished = true;
        }
    }

    void checkbox (Value& obj, const char* key, const char* label, bool def, bool* changed, bool* finished)
    {
        bool v = get_bool (obj, key, def);
        if (ImGui::Checkbox (label, &v))
        {
            obj[key]  = v;
            *changed  = true;
            *finished = true;
        }
    }

    bool read_rgb (const Value& obj, const char* key, float out[3])
    {
        const Value* v = engine::param (obj, key);
        if (v == NULL || !v->is_array () || v->size () < 3)
        {
            return false;
        }
        for (size_t i = 0; i < 3; i++)
        {
            out[i] = (*v)[i].is_number () ? (float) (*v)[i].get<double> () : 0.0f;
        }
        return true;
    }

    void color_edit (Value& obj, const char* key, const char* label, bool* changed, bool* finished)
    {
        float c[3] = {0.0f, 0.0f, 0.0f};
        read_rgb (obj, key, c);
        if (ImGui::ColorEdit3 (label, c, ImGuiColorEditFlags_NoInputs))
        {
            obj[key]  = Value{c[0], c[1], c[2]};
            *changed  = true;
            *finished = true;
        }
    }

    /* One of a numbers-array param, e.g. "shadows": [c, m, y]. */
    void array_slider (
        Value& obj, const char* key, size_t index, const char* label, float lo, float hi, bool* changed, bool* finished)
    {
        Value& arr = obj[key];
        if (!arr.is_array () || arr.size () <= index)
        {
            return;
        }
        float v = arr[index].is_number () ? (float) arr[index].get<double> () : 0.0f;
        if (ImGui::SliderFloat (label, &v, lo, hi, "%.0f"))
        {
            arr[index] = v;
            *changed   = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit ())
        {
            *finished = true;
        }
    }

    /* The channel choice of Levels / Curves: the composite (absent in Lab) and the channel objects the
       params hold. Returns the key edited, "" for the composite. */
    const char* channel_combo (AdjustEditor* ed)
    {
        const char* keys[11];
        const char* labels[11];
        int n    = 0;
        bool lab = engine::param_present (ed->params, "lightness") && !engine::param_present (ed->params, "red");
        if (!lab)
        {
            keys[n]     = "";
            labels[n++] = engine::param_present (ed->params, "cyan") ? "CMYK" : "RGB";
        }
        for (int i = 0; i < 10; i++)
        {
            if (engine::param_present (ed->params, CHANNEL_KEYS[i]))
            {
                keys[n]     = CHANNEL_KEYS[i];
                labels[n++] = CHANNEL_LABELS[i];
            }
        }
        if (ed->channel >= n)
        {
            ed->channel = 0;
        }
        ImGui::Combo ("Channel", &ed->channel, labels, n);
        return keys[ed->channel];
    }

    void levels_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        const char* key = channel_combo (ed);
        Value& obj      = key[0] == 0 ? ed->params : ed->params[key];
        slider (obj, "inBlack", "Input Black", 0.0f, 253.0f, 0.0f, "%.0f", false, changed, finished);
        slider (obj, "gamma", "Midtones", 0.01f, 9.99f, 1.0f, "%.2f", true, changed, finished);
        slider (obj, "inWhite", "Input White", 2.0f, 255.0f, 255.0f, "%.0f", false, changed, finished);
        slider (obj, "outBlack", "Output Black", 0.0f, 255.0f, 0.0f, "%.0f", false, changed, finished);
        slider (obj, "outWhite", "Output White", 0.0f, 255.0f, 255.0f, "%.0f", false, changed, finished);
    }

    /* The curve editor: click to add a point, drag to move it, drag it off the graph to remove it.
       The first and last points stay. Points are [input, output] pairs in 0..255. */
    void curve_editor (AdjustEditor* ed, Value& arr, bool* changed, bool* finished)
    {
        const float side = fminf (ImGui::GetContentRegionAvail ().x, 256.0f);
        ImVec2 o         = ImGui::GetCursorScreenPos ();
        ImGui::InvisibleButton ("##curve", ImVec2 (side, side));
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled (o, ImVec2 (o.x + side, o.y + side), IM_COL32 (30, 30, 30, 255));
        for (int i = 1; i < 4; i++)
        {
            float t = side * (float) i / 4.0f;
            dl->AddLine (ImVec2 (o.x + t, o.y), ImVec2 (o.x + t, o.y + side), IM_COL32 (70, 70, 70, 255));
            dl->AddLine (ImVec2 (o.x, o.y + t), ImVec2 (o.x + side, o.y + t), IM_COL32 (70, 70, 70, 255));
        }
        dl->AddLine (ImVec2 (o.x, o.y + side), ImVec2 (o.x + side, o.y), IM_COL32 (90, 90, 90, 255));

        std::vector<doc::CurvePoint> pts;
        for (size_t i = 0; arr.is_array () && i < arr.size (); i++)
        {
            const Value& q = arr[i];
            if (!q.is_array () || q.size () != 2 || !q[0].is_number () || !q[1].is_number ())
            {
                continue;
            }
            doc::CurvePoint cp = {(float) q[0].get<double> () / 255.0f, (float) q[1].get<double> () / 255.0f};
            pts.push_back (cp);
        }
        if (pts.size () < 2)
        {
            pts = doc::identity_curve ();
        }
        std::vector<float> lut = compose::curve_lut (pts);
        for (int x = 0; x < (int) side; x++)
        {
            float a = compose::lut_sample (lut, (float) x / side),
                  b = compose::lut_sample (lut, (float) (x + 1) / side);
            dl->AddLine (ImVec2 (o.x + (float) x, o.y + side - a * side),
                ImVec2 (o.x + (float) (x + 1), o.y + side - b * side), IM_COL32 (230, 230, 230, 255), 1.5f);
        }
        for (size_t i = 0; i < pts.size (); i++)
        {
            ImVec2 c (o.x + pts[i].input * side, o.y + side - pts[i].output * side);
            dl->AddRect (ImVec2 (c.x - 3, c.y - 3), ImVec2 (c.x + 3, c.y + 3), IM_COL32 (255, 255, 255, 255));
        }

        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - o.x) / side, my = 1.0f - (io.MousePos.y - o.y) / side;
        if (ImGui::IsItemClicked (ImGuiMouseButton_Left))
        {
            ed->curve_drag = -1;
            for (size_t i = 0; i < pts.size (); i++)
            {
                if (fabsf (pts[i].input - mx) * side < 6.0f && fabsf (pts[i].output - my) * side < 6.0f)
                {
                    ed->curve_drag = (int) i;
                }
            }
            if (ed->curve_drag < 0)
            {
                doc::CurvePoint np = {fminf (fmaxf (mx, 0.0f), 1.0f), fminf (fmaxf (my, 0.0f), 1.0f)};
                size_t at          = 0;
                while (at < pts.size () && pts[at].input < np.input)
                {
                    at++;
                }
                pts.insert (pts.begin () + (ptrdiff_t) at, np);
                ed->curve_drag = (int) at;
                *changed       = true;
            }
        }
        bool remove = false;
        if (ed->curve_drag >= 0 && ed->curve_drag < (int) pts.size () && ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            int i          = ed->curve_drag;
            bool end_point = i == 0 || i == (int) pts.size () - 1;
            float lo       = i > 0 ? pts[(size_t) i - 1].input + 1.0f / 255.0f : 0.0f;
            float hi       = i + 1 < (int) pts.size () ? pts[(size_t) i + 1].input - 1.0f / 255.0f : 1.0f;
            bool outside   = mx < -0.1f || mx > 1.1f || my < -0.1f || my > 1.1f;
            if (outside && !end_point && pts.size () > 2)
            {
                remove = true;
            }
            else
            {
                pts[(size_t) i].input  = fminf (fmaxf (mx, lo), hi);
                pts[(size_t) i].output = fminf (fmaxf (my, 0.0f), 1.0f);
            }
            *changed = true;
        }
        if (ed->curve_drag >= 0 && !ImGui::IsMouseDown (ImGuiMouseButton_Left))
        {
            ed->curve_drag = -1;
            *finished      = true;
        }
        if (remove)
        {
            pts.erase (pts.begin () + ed->curve_drag);
            ed->curve_drag = -1;
            *finished      = true;
        }
        if (*changed)
        {
            Value out = Value::array ();
            for (size_t i = 0; i < pts.size (); i++)
            {
                out.push_back (
                    Value{roundf (pts[i].input * 25500.0f) / 100.0f, roundf (pts[i].output * 25500.0f) / 100.0f});
            }
            arr = out;
        }
    }

    void curves_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        const char* key = channel_combo (ed);
        Value& arr      = ed->params[key[0] == 0 ? "points" : key];
        curve_editor (ed, arr, changed, finished);
        if (ImGui::SmallButton ("Reset Curve"))
        {
            arr       = Value{Value{0, 0}, Value{255, 255}};
            *changed  = true;
            *finished = true;
        }
    }

    void hue_saturation_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        const char* labels[7] = {"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
        bool colorize         = get_bool (ed->params, "colorize", false);
        if (colorize)
        {
            ed->row = 0;
        }
        else
        {
            ImGui::Combo ("Range", &ed->row, labels, 7);
        }
        Value& obj = ed->row == 0 ? ed->params : ed->params[HUE_KEYS[ed->row - 1]];
        if (colorize)
        {
            slider (obj, "hue", "Hue", 0.0f, 360.0f, 0.0f, "%.0f", false, changed, finished);
        }
        else
        {
            slider (obj, "hue", "Hue", -180.0f, 180.0f, 0.0f, "%.0f", false, changed, finished);
        }
        slider (
            obj, "saturation", "Saturation", colorize ? 0.0f : -100.0f, 100.0f, 0.0f, "%.0f", false, changed, finished);
        slider (obj, "lightness", "Lightness", -100.0f, 100.0f, 0.0f, "%.0f", false, changed, finished);
        bool was = colorize;
        checkbox (ed->params, "colorize", "Colorize", false, changed, finished);
        /* Ticking Colorize starts at 25% saturation, as in Photoshop. */
        if (!was && get_bool (ed->params, "colorize", false) && get_num (ed->params, "saturation", 0.0f) < 25.0f)
        {
            ed->params["saturation"] = 25.0f;
        }
    }

    void color_balance_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        const char* tones[3] = {"shadows", "midtones", "highlights"};
        ImGui::RadioButton ("Shadows", &ed->tone, 0);
        ImGui::SameLine ();
        ImGui::RadioButton ("Midtones", &ed->tone, 1);
        ImGui::SameLine ();
        ImGui::RadioButton ("Highlights", &ed->tone, 2);
        const char* key = tones[ed->tone < 0 || ed->tone > 2 ? 1 : ed->tone];
        array_slider (ed->params, key, 0, "Cyan / Red", -100.0f, 100.0f, changed, finished);
        array_slider (ed->params, key, 1, "Magenta / Green", -100.0f, 100.0f, changed, finished);
        array_slider (ed->params, key, 2, "Yellow / Blue", -100.0f, 100.0f, changed, finished);
        checkbox (ed->params, "preserveLuminosity", "Preserve Luminosity", true, changed, finished);
    }

    void photo_filter_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        int preset = -1;
        float c[3];
        if (read_rgb (ed->params, "color", c))
        {
            for (size_t i = 0; i < engine::PHOTO_FILTER_COUNT; i++)
            {
                const uint8_t* pc = engine::PHOTO_FILTERS[i].color;
                if (fabsf (c[0] * 255.0f - pc[0]) < 0.5f && fabsf (c[1] * 255.0f - pc[1]) < 0.5f
                    && fabsf (c[2] * 255.0f - pc[2]) < 0.5f)
                {
                    preset = (int) i;
                }
            }
        }
        if (ImGui::BeginCombo ("Filter", preset >= 0 ? engine::PHOTO_FILTERS[preset].label : "Custom color"))
        {
            for (size_t i = 0; i < engine::PHOTO_FILTER_COUNT; i++)
            {
                if (ImGui::Selectable (engine::PHOTO_FILTERS[i].label, (int) i == preset))
                {
                    const uint8_t* pc   = engine::PHOTO_FILTERS[i].color;
                    ed->params["color"] = Value{pc[0] / 255.0, pc[1] / 255.0, pc[2] / 255.0};
                    *changed            = true;
                    *finished           = true;
                }
            }
            ImGui::EndCombo ();
        }
        color_edit (ed->params, "color", "Color", changed, finished);
        slider (ed->params, "density", "Density", 0.0f, 100.0f, 25.0f, "%.0f%%", false, changed, finished);
        checkbox (ed->params, "preserveLuminosity", "Preserve Luminosity", true, changed, finished);
    }

    void channel_mixer_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        bool mono             = get_bool (ed->params, "monochrome", false);
        const char* rows[3]   = {"red", "green", "blue"};
        const char* labels[3] = {"Red", "Green", "Blue"};
        if (mono)
        {
            ed->row = 0;
        }
        else
        {
            ImGui::Combo ("Output Channel", &ed->row, labels, 3);
        }
        const char* key = mono ? "gray" : rows[ed->row < 0 || ed->row > 2 ? 0 : ed->row];
        if (mono && !engine::param_present (ed->params, "gray"))
        {
            ed->params["gray"] = ed->params["red"];
        }
        array_slider (ed->params, key, 0, "Red", -200.0f, 200.0f, changed, finished);
        array_slider (ed->params, key, 1, "Green", -200.0f, 200.0f, changed, finished);
        array_slider (ed->params, key, 2, "Blue", -200.0f, 200.0f, changed, finished);
        array_slider (ed->params, key, 3, "Constant", -200.0f, 200.0f, changed, finished);
        checkbox (ed->params, "monochrome", "Monochrome", false, changed, finished);
    }

    void gradient_map_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        Value& stops = ed->params["stops"];
        if (stops.is_array () && stops.size () >= 2)
        {
            const char* labels[2] = {"Shadows color", "Highlights color"};
            Value* ends[2]        = {&stops[0], &stops[stops.size () - 1]};
            for (int k = 0; k < 2; k++)
            {
                Value& stop = *ends[k];
                if (!stop.is_array () || stop.size () != 2 || !stop[1].is_array () || stop[1].size () < 3)
                {
                    continue;
                }
                float c[3] = {(float) stop[1][0].get<double> (), (float) stop[1][1].get<double> (),
                    (float) stop[1][2].get<double> ()};
                if (ImGui::ColorEdit3 (labels[k], c, ImGuiColorEditFlags_NoInputs))
                {
                    stop[1]   = Value{c[0], c[1], c[2]};
                    *changed  = true;
                    *finished = true;
                }
            }
        }
        checkbox (ed->params, "reverse", "Reverse", false, changed, finished);
        checkbox (ed->params, "dither", "Dither", false, changed, finished);
    }

    void selective_color_controls (AdjustEditor* ed, bool* changed, bool* finished)
    {
        ImGui::Combo ("Colors", &ed->row, SELECTIVE_LABELS, 9);
        const char* key = SELECTIVE_KEYS[ed->row < 0 || ed->row > 8 ? 0 : ed->row];
        array_slider (ed->params, key, 0, "Cyan", -100.0f, 100.0f, changed, finished);
        array_slider (ed->params, key, 1, "Magenta", -100.0f, 100.0f, changed, finished);
        array_slider (ed->params, key, 2, "Yellow", -100.0f, 100.0f, changed, finished);
        array_slider (ed->params, key, 3, "Black", -100.0f, 100.0f, changed, finished);
        int relative   = 1;
        const Value* m = engine::param (ed->params, "method");
        if (m != NULL && m->is_string () && m->get<std::string> () == "absolute")
        {
            relative = 0;
        }
        bool picked = ImGui::RadioButton ("Relative", &relative, 1);
        ImGui::SameLine ();
        picked = ImGui::RadioButton ("Absolute", &relative, 0) || picked;
        if (picked)
        {
            ed->params["method"] = relative ? "relative" : "absolute";
            *changed             = true;
            *finished            = true;
        }
    }

    void adjust_editor_draw (AdjustEditor* ed, bool* changed, bool* finished)
    {
        *changed  = false;
        *finished = false;
        ImGui::PushItemWidth (-110.0f);
        switch (ed->kind)
        {
        case AdjustmentKind::BrightnessContrast:
            {
                bool legacy = get_bool (ed->params, "legacy", false);
                slider (
                    ed->params, "brightness", "Brightness", -150.0f, 150.0f, 0.0f, "%.0f", false, changed, finished);
                slider (ed->params, "contrast", "Contrast", legacy ? -100.0f : -50.0f, 100.0f, 0.0f, "%.0f", false,
                    changed, finished);
                checkbox (ed->params, "legacy", "Use Legacy", false, changed, finished);
                break;
            }
        case AdjustmentKind::Levels:
            levels_controls (ed, changed, finished);
            break;
        case AdjustmentKind::Curves:
            curves_controls (ed, changed, finished);
            break;
        case AdjustmentKind::Exposure:
            slider (ed->params, "exposure", "Exposure", -20.0f, 20.0f, 0.0f, "%.2f", false, changed, finished);
            slider (ed->params, "offset", "Offset", -0.5f, 0.5f, 0.0f, "%.4f", false, changed, finished);
            slider (ed->params, "gamma", "Gamma Correction", 0.01f, 9.99f, 1.0f, "%.2f", true, changed, finished);
            break;
        case AdjustmentKind::Vibrance:
            slider (ed->params, "vibrance", "Vibrance", -100.0f, 100.0f, 0.0f, "%.0f", false, changed, finished);
            slider (ed->params, "saturation", "Saturation", -100.0f, 100.0f, 0.0f, "%.0f", false, changed, finished);
            break;
        case AdjustmentKind::HueSaturation:
            hue_saturation_controls (ed, changed, finished);
            break;
        case AdjustmentKind::ColorBalance:
            color_balance_controls (ed, changed, finished);
            break;
        case AdjustmentKind::BlackWhite:
            for (int i = 0; i < 6; i++)
            {
                slider (
                    ed->params, HUE_KEYS[i], HUE_LABELS[i], -200.0f, 300.0f, 0.0f, "%.0f%%", false, changed, finished);
            }
            checkbox (ed->params, "tint", "Tint", false, changed, finished);
            if (get_bool (ed->params, "tint", false))
            {
                color_edit (ed->params, "tintColor", "Tint color", changed, finished);
            }
            break;
        case AdjustmentKind::PhotoFilter:
            photo_filter_controls (ed, changed, finished);
            break;
        case AdjustmentKind::ChannelMixer:
            channel_mixer_controls (ed, changed, finished);
            break;
        case AdjustmentKind::Posterize:
            slider_int (ed->params, "levels", "Levels", 2, 255, 4, changed, finished);
            break;
        case AdjustmentKind::Threshold:
            slider_int (ed->params, "level", "Threshold Level", 1, 255, 128, changed, finished);
            break;
        case AdjustmentKind::GradientMap:
            gradient_map_controls (ed, changed, finished);
            break;
        case AdjustmentKind::SelectiveColor:
            selective_color_controls (ed, changed, finished);
            break;
        case AdjustmentKind::Invert:
            ImGui::TextDisabled ("Invert has no settings.");
            break;
        default:
            ImGui::TextDisabled ("This adjustment can't be edited yet.");
            break;
        }
        ImGui::PopItemWidth ();
    }

} // namespace ui
