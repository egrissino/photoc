#include "ui/app.h"
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace ui
{

    /* Image > Mode > Color Table: the Indexed image's palette as a grid of swatches. Click one to
       change it, right-click one to make it the transparent entry; Table loads a built-in table.
       OK sets the table (pixels keep their entry, so their colour follows). */

    const char* const CT_TABLE_IDS[7] = {
        "custom", "blackBody", "grayscale", "spectrum", "systemMac", "systemWindows", "web"};
    const char* const CT_TABLE_LABELS[7] = {
        "Custom", "Black Body", "Grayscale", "Spectrum", "System (Mac OS)", "System (Windows)", "Web"};

    void App::open_color_table ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        if (!d->doc->has_color_table)
        {
            set_error ("Color Table needs an Indexed Color image");
            return;
        }
        ct_rgb_           = d->doc->color_table.rgb;
        ct_transparent_   = d->doc->color_table.transparent;
        ct_edit_          = -1;
        show_color_table_ = true;
    }

    void App::color_table_dialog ()
    {
        if (!show_color_table_)
        {
            return;
        }
        float k = ui_scale_;
        ImGui::OpenPopup ("Color Table###colortable");
        if (!ImGui::BeginPopupModal ("Color Table###colortable", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::AlignTextToFramePadding ();
        ImGui::TextUnformatted ("Table:");
        ImGui::SameLine ();
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::BeginCombo ("##cttable", "Custom"))
        {
            for (int i = 1; i < 7; i++)
            {
                if (ImGui::Selectable (CT_TABLE_LABELS[i]))
                {
                    std::vector<uint8_t> t;
                    if (engine::color_table_preset (CT_TABLE_IDS[i], &t))
                    {
                        ct_rgb_ = t;
                        if (ct_transparent_ >= (int) (t.size () / 3))
                        {
                            ct_transparent_ = -1;
                        }
                    }
                }
            }
            ImGui::EndCombo ();
        }
        /* The grid: 16 to a row. The transparent entry shows a checkerboard with a red slash. */
        float cell     = 20.0f * k;
        size_t n       = ct_rgb_.size () / 3;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        for (size_t i = 0; i < n; i++)
        {
            if (i % 16 != 0)
            {
                ImGui::SameLine (0, 2 * k);
            }
            ImGui::PushID ((int) i);
            ImVec2 a = ImGui::GetCursorScreenPos (), b (a.x + cell, a.y + cell);
            if (ImGui::InvisibleButton ("##entry", ImVec2 (cell, cell)))
            {
                ct_edit_ = (int) i;
                ImGui::OpenPopup ("##ctpick");
            }
            if (ImGui::IsItemClicked (ImGuiMouseButton_Right))
            {
                ct_transparent_ = ct_transparent_ == (int) i ? -1 : (int) i;
            }
            const uint8_t* c = &ct_rgb_[i * 3];
            if ((int) i == ct_transparent_)
            {
                float h = cell * 0.5f;
                for (int q = 0; q < 4; q++)
                {
                    ImVec2 qa (a.x + (q % 2) * h, a.y + (q / 2) * h);
                    dl->AddRectFilled (qa, ImVec2 (qa.x + h, qa.y + h),
                        (q == 0 || q == 3) ? IM_COL32 (255, 255, 255, 255) : IM_COL32 (200, 200, 200, 255));
                }
                dl->AddLine (ImVec2 (a.x, b.y), ImVec2 (b.x, a.y), IM_COL32 (220, 30, 30, 255), 2.0f * k);
            }
            else
            {
                dl->AddRectFilled (a, b, IM_COL32 (c[0], c[1], c[2], 255));
            }
            dl->AddRect (a, b, ImGui::IsItemHovered () ? theme_.accent : IM_COL32 (0, 0, 0, 120));
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("Entry %zu: #%02x%02x%02x%s\nClick to change; right-click for transparent", i, c[0],
                    c[1], c[2], (int) i == ct_transparent_ ? " (transparent)" : "");
            }
            if (ct_edit_ == (int) i && ImGui::BeginPopup ("##ctpick"))
            {
                float f[3] = {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f};
                if (ImGui::ColorPicker3 ("##ctcolor", f, ImGuiColorEditFlags_NoSidePreview))
                {
                    for (int q = 0; q < 3; q++)
                    {
                        ct_rgb_[i * 3 + (size_t) q] = (uint8_t) lroundf (f[q] * 255.0f);
                    }
                }
                ImGui::EndPopup ();
            }
            ImGui::PopID ();
        }
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            show_color_table_ = false;
            ImGui::CloseCurrentPopup ();
            if (ok)
            {
                engine::Value colors = engine::Value::array ();
                for (size_t i = 0; i < n; i++)
                {
                    char hex[8];
                    snprintf (hex, sizeof hex, "#%02x%02x%02x", ct_rgb_[i * 3], ct_rgb_[i * 3 + 1], ct_rgb_[i * 3 + 2]);
                    colors.push_back (hex);
                }
                engine::Value p  = {{"colors", colors}};
                p["transparent"] = ct_transparent_ >= 0 ? engine::Value (ct_transparent_) : engine::Value ();
                run_command ("image.mode.colorTable", p);
            }
        }
        ImGui::EndPopup ();
    }

} // namespace ui
