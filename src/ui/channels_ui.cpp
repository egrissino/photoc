#include "algo/selection.h"
#include "color/convert.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui
{

    /* The Channels panel: the document's colour channels with eyes and thumbnails, then its alpha
       channels. Showing one colour channel draws it in grayscale; showing some draws only those.
       Clicking an alpha channel shows it alone in gray; its eye alone lays it over the image as a
       red overlay on the masked (black) areas. Ctrl-click loads an alpha channel as the selection.
       Colour channels are a view: painting still applies to every channel (channel targets are not
       ported yet), and alpha channels change through Save Selection. RGB and grayscale documents
       list their colour channels; other modes show the composite. */

    int channel_count (const doc::Document& d)
    {
        if (d.mode == color::ColorMode::Rgb)
        {
            return 3;
        }
        if (d.mode == color::ColorMode::Grayscale)
        {
            return 1;
        }
        return 0;
    }

    /* The composite as the view shows it: hidden channels dropped, a single channel as gray, and
       the alpha channel being viewed alone or as an overlay. */
    void App::channel_filter (compose::Buffer* b)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        size_t n = b->pixel_count ();
        if (quick_mask_)
        {
            /* Quick Mask: 50% red over what isn't selected (none of it without a selection). */
            const doc::Document& shown = preview_doc_ ? *preview_doc_ : *d->doc;
            if (!shown.has_selection)
            {
                return;
            }
            std::vector<float> m;
            if (!shown.selection.read_region (b->rect, &m) || m.size () != n)
            {
                return;
            }
            for (size_t i = 0; i < n; i++)
            {
                float* p = &b->px[i * 4];
                float r  = 0.5f * (1.0f - m[i]);
                if (r <= 0.0f)
                {
                    continue;
                }
                for (int ch = 0; ch < 3; ch++)
                {
                    p[ch] = p[ch] * p[3] + (1.0f - p[3]);
                }
                p[0] = p[0] * (1.0f - r) + r;
                p[1] *= 1.0f - r;
                p[2] *= 1.0f - r;
                p[3] = 1.0f;
            }
            return;
        }
        if (alpha_view_ >= 0 && (size_t) alpha_view_ < d->doc->channels.size ())
        {
            std::vector<float> m;
            if (!d->doc->channels[(size_t) alpha_view_].surface.read_region (b->rect, &m) || m.size () != n)
            {
                return;
            }
            for (size_t i = 0; i < n; i++)
            {
                float* p = &b->px[i * 4];
                if (!alpha_overlay_)
                {
                    p[0] = p[1] = p[2] = m[i];
                }
                else
                {
                    float c[3];
                    for (int ch = 0; ch < 3; ch++)
                    {
                        c[ch] = p[ch] * p[3] + (1.0f - p[3]);
                    }
                    float r = 0.5f * (1.0f - m[i]);
                    p[0]    = c[0] * (1.0f - r) + r;
                    p[1]    = c[1] * (1.0f - r);
                    p[2]    = c[2] * (1.0f - r);
                }
                p[3] = 1.0f;
            }
            return;
        }
        if (channel_count (*d->doc) != 3)
        {
            return;
        }
        int shown = 0, only = -1;
        for (int c = 0; c < 3; c++)
        {
            if (channel_shown_[c])
            {
                shown++;
                only = c;
            }
        }
        if (shown == 3)
        {
            return;
        }
        for (size_t i = 0; i < n; i++)
        {
            float* p = &b->px[i * 4];
            if (shown == 1)
            {
                float v = p[only] * p[3] + (1.0f - p[3]);
                p[0] = p[1] = p[2] = v;
                p[3]               = 1.0f;
            }
            else
            {
                for (int c = 0; c < 3; c++)
                {
                    if (!channel_shown_[c])
                    {
                        p[c] = 0.0f;
                    }
                }
            }
        }
    }

    /* Gray thumbnails of the alpha channels, rebuilt when the document changes. */
    void App::update_alpha_thumbnails (const doc::Document& d, uint64_t revision)
    {
        if (alpha_tex_doc_ == d.id && alpha_tex_revision_ == revision && alpha_tex_.size () == d.channels.size ())
        {
            return;
        }
        alpha_tex_doc_      = d.id;
        alpha_tex_revision_ = revision;
        while (alpha_tex_.size () > d.channels.size ())
        {
            glDeleteTextures (1, &alpha_tex_.back ());
            alpha_tex_.pop_back ();
        }
        float scale = 96.0f / fmaxf ((float) d.size.width, (float) d.size.height);
        if (scale > 1.0f)
        {
            scale = 1.0f;
        }
        int w = (int) fmaxf (1.0f, roundf ((float) d.size.width * scale)),
            h = (int) fmaxf (1.0f, roundf ((float) d.size.height * scale));
        std::vector<uint8_t> px ((size_t) w * h * 4, 255);
        for (size_t c = 0; c < d.channels.size (); c++)
        {
            const raster::Surface& s = d.channels[c].surface;
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    float v = s.sample_channel (
                        (int32_t) (((float) x + 0.5f) / scale), (int32_t) (((float) y + 0.5f) / scale), 0);
                    uint8_t g  = (uint8_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 255.0f);
                    uint8_t* o = &px[((size_t) y * w + x) * 4];
                    o[0] = o[1] = o[2] = g;
                }
            }
            if (c >= alpha_tex_.size ())
            {
                unsigned int t = 0;
                glGenTextures (1, &t);
                alpha_tex_.push_back (t);
            }
            glBindTexture (GL_TEXTURE_2D, alpha_tex_[c]);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data ());
        }
    }

    /* A small copy of the composite (at most 256 px on a side) for the channel thumbnails, the
       Navigator and the Histogram, kept up to date from whatever part of the canvas was just
       rendered. */
    void App::update_small_preview (const doc::Document& d, const compose::Buffer& b)
    {
        float scale = 256.0f / fmaxf ((float) d.size.width, (float) d.size.height);
        if (scale > 1.0f)
        {
            scale = 1.0f;
        }
        int w = (int) fmaxf (1.0f, roundf ((float) d.size.width * scale)),
            h = (int) fmaxf (1.0f, roundf ((float) d.size.height * scale));
        if (w != small_w_ || h != small_h_ || small_.size () != (size_t) w * h * 4)
        {
            small_w_ = w;
            small_h_ = h;
            small_.assign ((size_t) w * h * 4, 255);
        }
        bool any = false;
        for (int y = 0; y < h; y++)
        {
            int32_t sy = (int32_t) (((float) y + 0.5f) / scale);
            if (sy < b.rect.y0 || sy >= b.rect.y1)
            {
                continue;
            }
            for (int x = 0; x < w; x++)
            {
                int32_t sx = (int32_t) (((float) x + 0.5f) / scale);
                if (sx < b.rect.x0 || sx >= b.rect.x1)
                {
                    continue;
                }
                const float* p = b.at (sx, sy);
                uint8_t* o     = &small_[((size_t) y * w + x) * 4];
                for (int c = 0; c < 3; c++)
                {
                    float v = p[c] * p[3] + (1.0f - p[3]);
                    o[c]    = (uint8_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 255.0f);
                }
                o[3] = 255;
                any  = true;
            }
        }
        if (any)
        {
            small_serial_++;
        }
    }

    /* Shows the colour channels (r = 0 the composite, else one channel) and leaves alpha view. */
    void App::show_color_channel (int r)
    {
        for (int c = 0; c < 3; c++)
        {
            channel_shown_[c] = r == 0 || c == r - 1;
        }
        alpha_view_         = -1;
        channel_view_dirty_ = true;
    }

    void App::channels_window ()
    {
        ImGui::Begin ("Channels");
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        /* Commands run from the rows replace the session's document: keep this one alive meanwhile. */
        std::shared_ptr<const doc::Document> doc = d->doc;
        if (alpha_view_ >= (int) doc->channels.size ())
        {
            alpha_view_         = -1;
            channel_view_dirty_ = true;
        }
        update_alpha_thumbnails (*doc, d->revision);
        int nch = channel_count (*doc);
        float k = ui_scale_;
        /* Thumbnails: the composite, then each channel in gray. */
        if (channel_tex_serial_ != small_serial_ && small_w_ > 0)
        {
            channel_tex_serial_ = small_serial_;
            std::vector<uint8_t> px (small_.size ());
            for (int t = 0; t < 4; t++)
            {
                for (size_t i = 0; i < small_.size () / 4; i++)
                {
                    for (int c = 0; c < 3; c++)
                    {
                        px[i * 4 + c] = t == 0 || nch < 3 ? small_[i * 4 + c] : small_[i * 4 + (size_t) (t - 1)];
                    }
                    px[i * 4 + 3] = 255;
                }
                if (channel_tex_[t] == 0)
                {
                    glGenTextures (1, &channel_tex_[t]);
                }
                glBindTexture (GL_TEXTURE_2D, channel_tex_[t]);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, small_w_, small_h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data ());
            }
        }
        const char* names[4] = {"RGB", "Red", "Green", "Blue"};
        const char* keys[4]  = {"Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5"};
        int rows             = nch == 3 ? 4 : 1;
        if (nch == 1)
        {
            names[0] = "Gray";
        }
        else if (nch == 0)
        {
            names[0] = doc->mode == color::ColorMode::Cmyk ? "CMYK"
                     : doc->mode == color::ColorMode::Lab  ? "Lab"
                                                           : "Composite";
        }
        bool alpha_only = alpha_view_ >= 0 && !alpha_overlay_;
        bool all        = channel_shown_[0] && channel_shown_[1] && channel_shown_[2] && !alpha_only;
        int alphas      = (int) doc->channels.size ();
        float buttons_h = 24.0f * k + ImGui::GetStyle ().ItemSpacing.y * 2;
        ImGui::PushStyleColor (ImGuiCol_ChildBg, theme_.field);
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, ImVec2 (0, 0));
        ImGui::BeginChild ("##channels", ImVec2 (0, -(buttons_h + (all ? 0 : ImGui::GetTextLineHeightWithSpacing ()))),
            ImGuiChildFlags_Borders);
        float rh = roundf (36 * k), thumb = roundf (28 * k), eye_w = 26 * k;
        int load_alpha = -1, delete_alpha = -1, duplicate_alpha = -1;
        for (int r = 0; r < rows + alphas; r++)
        {
            bool alpha = r >= rows;
            int ai     = r - rows;
            ImGui::PushID (r);
            ImVec2 p       = ImGui::GetCursorScreenPos ();
            float w        = ImGui::GetContentRegionAvail ().x;
            ImDrawList* dl = ImGui::GetWindowDrawList ();
            bool visible, selected;
            if (alpha)
            {
                visible  = alpha_view_ == ai;
                selected = alpha_view_ == ai && !alpha_overlay_;
            }
            else
            {
                visible  = !alpha_only && (r == 0 ? all : channel_shown_[r - 1]);
                selected = !alpha_only && (r == 0 ? all : (channel_shown_[r - 1] && !all));
            }
            if (selected)
            {
                dl->AddRectFilled (p, ImVec2 (p.x + w, p.y + rh), theme_.row_selected);
            }
            dl->AddLine (ImVec2 (p.x, p.y + rh - 1), ImVec2 (p.x + w, p.y + rh - 1), theme_.separator);
            if (ImGui::InvisibleButton ("##eye", ImVec2 (eye_w, rh)))
            {
                if (alpha)
                {
                    bool on        = alpha_view_ == ai;
                    alpha_view_    = on ? -1 : ai;
                    alpha_overlay_ = !on && !alpha_only;
                    if (on && alpha_only)
                    {
                        show_color_channel (0);
                    }
                    channel_view_dirty_ = true;
                }
                else if (nch == 3)
                {
                    if (r == 0 || alpha_only)
                    {
                        for (int c = 0; c < 3; c++)
                        {
                            channel_shown_[c] = r == 0 || c == r - 1;
                        }
                    }
                    else
                    {
                        channel_shown_[r - 1] = !channel_shown_[r - 1];
                        if (!channel_shown_[0] && !channel_shown_[1] && !channel_shown_[2])
                        {
                            channel_shown_[r - 1] = true;
                        }
                    }
                    if (alpha_only)
                    {
                        alpha_view_ = -1;
                    }
                    channel_view_dirty_ = true;
                }
                else if (alpha_only)
                {
                    show_color_channel (0);
                }
            }
            float g = 16 * k;
            if (visible)
            {
                draw_icon (
                    dl, &icons_, "eye", ImVec2 (p.x + (eye_w - g) * 0.5f, p.y + (rh - g) * 0.5f), g, theme_.icon);
            }
            dl->AddLine (ImVec2 (p.x + eye_w, p.y), ImVec2 (p.x + eye_w, p.y + rh), theme_.separator);
            ImGui::SetCursorScreenPos (ImVec2 (p.x + eye_w, p.y));
            bool editing = alpha && rename_channel_ == ai;
            if (!editing && ImGui::InvisibleButton ("##row", ImVec2 (w - eye_w, rh)))
            {
                if (alpha && ImGui::GetIO ().KeyCtrl)
                {
                    load_alpha = ai;
                }
                else if (alpha)
                {
                    alpha_view_         = ai;
                    alpha_overlay_      = false;
                    channel_view_dirty_ = true;
                }
                else if (nch == 3)
                {
                    show_color_channel (r);
                }
                else
                {
                    show_color_channel (0);
                }
            }
            if (alpha && !editing)
            {
                if (ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                {
                    rename_channel_ = ai;
                    rename_focus_   = true;
                    snprintf (rename_buf_, sizeof rename_buf_, "%s", doc->channels[(size_t) ai].name.c_str ());
                }
                if (ImGui::IsItemHovered ())
                {
                    ImGui::SetTooltip ("Ctrl-click to load as a selection");
                }
                if (ImGui::BeginPopupContextItem ("##chctx"))
                {
                    if (ImGui::MenuItem ("Duplicate Channel"))
                    {
                        duplicate_alpha = ai;
                    }
                    if (ImGui::MenuItem ("Delete Channel"))
                    {
                        delete_alpha = ai;
                    }
                    if (ImGui::MenuItem ("Rename Channel…"))
                    {
                        rename_channel_ = ai;
                        rename_focus_   = true;
                        snprintf (rename_buf_, sizeof rename_buf_, "%s", doc->channels[(size_t) ai].name.c_str ());
                    }
                    if (ImGui::MenuItem ("Load as Selection"))
                    {
                        load_alpha = ai;
                    }
                    ImGui::EndPopup ();
                }
            }
            float tx = p.x + eye_w + 8 * k, ty = p.y + (rh - thumb) * 0.5f;
            unsigned int tex = alpha ? (ai < (int) alpha_tex_.size () ? alpha_tex_[(size_t) ai] : 0) : channel_tex_[r];
            if (tex != 0 && small_w_ > 0)
            {
                float sc = thumb / fmaxf ((float) doc->size.width, (float) doc->size.height);
                float iw = (float) doc->size.width * sc, ih = (float) doc->size.height * sc;
                ImVec2 a (tx + (thumb - iw) * 0.5f, ty + (thumb - ih) * 0.5f);
                dl->AddImage ((ImTextureID) (intptr_t) tex, a, ImVec2 (a.x + iw, a.y + ih));
                dl->AddRect (ImVec2 (a.x - 1, a.y - 1), ImVec2 (a.x + iw + 1, a.y + ih + 1), theme_.card_border);
            }
            float th = ImGui::GetTextLineHeight ();
            float lx = tx + thumb + 10 * k;
            if (editing)
            {
                ImGui::SetCursorScreenPos (ImVec2 (lx, p.y + (rh - ImGui::GetFrameHeight ()) * 0.5f));
                ImGui::SetNextItemWidth (w - (lx - p.x) - 8 * k);
                if (rename_focus_)
                {
                    ImGui::SetKeyboardFocusHere ();
                    rename_focus_ = false;
                }
                bool done = ImGui::InputText ("##rename", rename_buf_, sizeof rename_buf_,
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (done && rename_buf_[0] != 0)
                {
                    run_command ("channel.rename", engine::Value{{"channel", ai}, {"name", rename_buf_}});
                }
                if (done || ImGui::IsItemDeactivated () || ImGui::IsKeyPressed (ImGuiKey_Escape))
                {
                    rename_channel_ = -1;
                }
            }
            else
            {
                const char* label = alpha ? doc->channels[(size_t) ai].name.c_str () : names[r];
                dl->AddText (ImVec2 (lx, p.y + (rh - th) * 0.5f), theme_.text, label);
            }
            char key[24] = "";
            if (!alpha && nch == 3)
            {
                snprintf (key, sizeof key, "%s", keys[r]);
            }
            else if (alpha && ai + 6 <= 9 && nch == 3)
            {
                snprintf (key, sizeof key, "Ctrl+%d", ai + 6);
            }
            if (key[0] != 0)
            {
                float kw = ImGui::CalcTextSize (key).x;
                dl->AddText (ImVec2 (p.x + w - kw - 10 * k, p.y + (rh - th) * 0.5f), theme_.text_faint, key);
            }
            ImGui::SetCursorScreenPos (ImVec2 (p.x, p.y + rh));
            ImGui::PopID ();
        }
        ImGui::EndChild ();
        ImGui::PopStyleVar ();
        ImGui::PopStyleColor ();
        if (!all && !alpha_only)
        {
            ImGui::TextColored (
                ImGui::ColorConvertU32ToFloat4 (theme_.warning), "Channel view only: edits apply to all channels.");
        }
        else if (alpha_only)
        {
            ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (theme_.text_dim),
                "Viewing an alpha channel. Ctrl+2 returns to the image.");
        }

        /* Load channel as selection, save selection as channel, new channel, delete channel. */
        float box = 24.0f * k, total = 4 * box + 3 * 4 * k;
        ImGui::SetCursorPosX (ImGui::GetCursorPosX () + fmaxf (ImGui::GetContentRegionAvail ().x - total, 0.0f));
        int current = alpha_view_ >= 0 ? alpha_view_ : alphas - 1;
        ImGui::BeginDisabled (current < 0);
        if (icon_button ("##loadch", "circle-dashed", box, false, "Load channel as selection"))
        {
            load_alpha = current;
        }
        ImGui::EndDisabled ();
        ImGui::SameLine (0, 4 * k);
        ImGui::BeginDisabled (!doc->has_selection);
        if (icon_button ("##savech", "square-dashed", box, false, "Save selection as channel"))
        {
            run_command ("select.save", engine::Value{{"channel", -1}});
        }
        ImGui::EndDisabled ();
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##newch", "plus", box, false, "Create new channel"))
        {
            run_command ("channel.new", engine::Value::object ());
        }
        ImGui::SameLine (0, 4 * k);
        ImGui::BeginDisabled (alpha_view_ < 0);
        if (icon_button ("##delch", "trash", box, false, "Delete current channel"))
        {
            delete_alpha = alpha_view_;
        }
        ImGui::EndDisabled ();
        ImGui::End ();

        if (load_alpha >= 0)
        {
            ImGuiIO& io      = ImGui::GetIO ();
            const char* mode = io.KeyShift && io.KeyAlt ? "intersect"
                             : io.KeyShift              ? "add"
                             : io.KeyAlt                ? "subtract"
                                                        : "replace";
            run_command ("select.load", engine::Value{{"channel", load_alpha}, {"mode", mode}});
        }
        if (duplicate_alpha >= 0)
        {
            run_command ("channel.duplicate", engine::Value{{"channel", duplicate_alpha}});
        }
        if (delete_alpha >= 0)
        {
            run_command ("channel.delete", engine::Value{{"channel", delete_alpha}});
            if (alpha_view_ == delete_alpha)
            {
                show_color_channel (0);
            }
        }

        /* Ctrl+2 shows the composite, Ctrl+3..5 one channel, Ctrl+6.. the alpha channels
           (Photoshop). */
        if (!ImGui::GetIO ().WantTextInput)
        {
            ImGuiKey keys2[8] = {
                ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9};
            for (int r = 0; r < 8; r++)
            {
                if (!ImGui::Shortcut (ImGuiMod_Ctrl | keys2[r], ImGuiInputFlags_RouteGlobal))
                {
                    continue;
                }
                if (r < 4 && (nch == 3 || r == 0))
                {
                    show_color_channel (r);
                }
                else if (r >= 4 && nch == 3 && r - 4 < alphas)
                {
                    alpha_view_         = r - 4;
                    alpha_overlay_      = false;
                    channel_view_dirty_ = true;
                }
            }
        }
    }

    /* The Histogram panel: the composite's red, green and blue levels drawn over each other (where
       they overlap the colours mix, as Photoshop's Colors view), and the luminosity statistics.
       It reads the small preview the channel thumbnails use, so it is an estimate for large images,
       like Photoshop's cached histogram. */
    void App::histogram_window ()
    {
        ImGui::Begin ("Histogram");
        const engine::DocState* d = session_.active ();
        if (d == NULL || small_w_ <= 0 || small_.empty ())
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        float hist[4][256] = {{0}};
        size_t count       = small_.size () / 4;
        double sum = 0, sum2 = 0;
        for (size_t i = 0; i < count; i++)
        {
            const uint8_t* p = &small_[i * 4];
            for (int c = 0; c < 3; c++)
            {
                hist[c][p[c]] += 1.0f;
            }
            int lum = (int) lroundf (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]);
            hist[3][lum] += 1.0f;
            sum += lum;
            sum2 += (double) lum * lum;
        }
        bool gray  = channel_count (*d->doc) != 3;
        float peak = 1.0f;
        for (int c = gray ? 3 : 0; c < 4; c++)
        {
            for (int i = 0; i < 256; i++)
            {
                peak = fmaxf (peak, hist[c][i]);
            }
        }
        float k = ui_scale_;
        float w = fmaxf (ImGui::GetContentRegionAvail ().x, 64 * k), h = 100 * k;
        ImVec2 a       = ImGui::GetCursorScreenPos ();
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        dl->AddRectFilled (a, ImVec2 (a.x + w, a.y + h), theme_.field);
        float bw = w / 256.0f;
        if (gray)
        {
            for (int i = 0; i < 256; i++)
            {
                float bh = hist[3][i] / peak * (h - 2);
                dl->AddRectFilled (
                    ImVec2 (a.x + i * bw, a.y + h - bh), ImVec2 (a.x + (i + 1) * bw, a.y + h), theme_.text_dim);
            }
        }
        else
        {
            const ImU32 cols[3] = {
                IM_COL32 (230, 60, 60, 255), IM_COL32 (60, 200, 80, 255), IM_COL32 (70, 110, 240, 255)};
            dl->PushClipRect (a, ImVec2 (a.x + w, a.y + h), true);
            for (int c = 0; c < 3; c++)
            {
                for (int i = 0; i < 256; i++)
                {
                    float bh = hist[c][i] / peak * (h - 2);
                    ImVec2 p0 (a.x + i * bw, a.y + h - bh), p1 (a.x + (i + 1) * bw, a.y + h);
                    ImU32 col = cols[c];
                    dl->AddRectFilled (p0, p1, (col & 0x00FFFFFFu) | (IM_COL32_A_MASK & IM_COL32 (0, 0, 0, 120)));
                }
            }
            dl->PopClipRect ();
        }
        dl->AddRect (a, ImVec2 (a.x + w, a.y + h), theme_.card_border);
        ImGui::Dummy (ImVec2 (w, h + 4 * k));
        double mean = count > 0 ? sum / (double) count : 0.0;
        double sd   = count > 0 ? sqrt (fmax (sum2 / (double) count - mean * mean, 0.0)) : 0.0;
        int median  = 0;
        float acc   = 0;
        while (median < 255 && acc + hist[3][median] <= (float) count * 0.5f)
        {
            acc += hist[3][median++];
        }
        ImGui::Text ("Mean: %.2f", mean);
        ImGui::SameLine (w * 0.5f);
        ImGui::Text ("Median: %d", median);
        ImGui::Text ("Std Dev: %.2f", sd);
        ImGui::SameLine (w * 0.5f);
        ImGui::Text ("Pixels: %llu", (unsigned long long) d->doc->size.width * d->doc->size.height);
        ImGui::End ();
    }

    /* The Info panel: the composite colour under the pointer (RGB, HSB as percent, CMYK), the
       pointer position, the selection's size and the document's. */
    void App::info_window ()
    {
        ImGui::Begin ("Info");
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        const doc::Document& doc = *d->doc;
        bool inside              = info_over_ && info_x_ >= 0 && info_y_ >= 0 && info_x_ < (int32_t) doc.size.width
                   && info_y_ < (int32_t) doc.size.height;
        if (inside
            && (info_doc_ != doc.id || info_revision_ != d->revision || info_sampled_[0] != info_x_
                || info_sampled_[1] != info_y_))
        {
            info_doc_                        = doc.id;
            info_revision_                   = d->revision;
            info_sampled_[0]                 = info_x_;
            info_sampled_[1]                 = info_y_;
            compose::RenderSettings settings = compose::default_settings ();
            settings.cmyk                    = session_.cmyk;
            compose::Buffer b = compose::render (doc, geom::Rect{info_x_, info_y_, info_x_ + 1, info_y_ + 1}, settings);
            if (b.px.size () >= 4)
            {
                for (int c = 0; c < 4; c++)
                {
                    info_px_[c] = b.px[(size_t) c];
                }
            }
        }
        float half = ImGui::GetContentRegionAvail ().x * 0.5f;
        if (inside)
        {
            float rgb[3];
            for (int c = 0; c < 3; c++)
            {
                rgb[c] = info_px_[c] * info_px_[3] + (1.0f - info_px_[3]);
            }
            float cmyk[4];
            color::rgb_to_cmyk (session_.cmyk, rgb, cmyk);
            float h, s, v;
            rgb_to_hsv (rgb, &h, &s, &v);
            ImGui::Text ("R: %3d", (int) lroundf (rgb[0] * 255));
            ImGui::SameLine (half);
            ImGui::Text ("C: %3d%%", (int) lroundf (cmyk[0] * 100));
            ImGui::Text ("G: %3d", (int) lroundf (rgb[1] * 255));
            ImGui::SameLine (half);
            ImGui::Text ("M: %3d%%", (int) lroundf (cmyk[1] * 100));
            ImGui::Text ("B: %3d", (int) lroundf (rgb[2] * 255));
            ImGui::SameLine (half);
            ImGui::Text ("Y: %3d%%", (int) lroundf (cmyk[2] * 100));
            ImGui::Text ("H: %3d°  S: %3d%%  B: %3d%%", (int) lroundf (h * 360), (int) lroundf (s * 100),
                (int) lroundf (v * 100));
            ImGui::SameLine (half);
            ImGui::Text ("K: %3d%%", (int) lroundf (cmyk[3] * 100));
        }
        else
        {
            ImGui::TextDisabled ("R:\nG:\nB:");
        }
        ImGui::Separator ();
        if (inside)
        {
            ImGui::Text ("X: %d px", info_x_);
        }
        else
        {
            ImGui::TextDisabled ("X:");
        }
        ImGui::SameLine (half);
        if (doc.has_selection)
        {
            geom::Rect r = doc.selection.content_bounds ().intersect (doc.bounds ());
            ImGui::Text ("W: %u px", r.width ());
        }
        else
        {
            ImGui::TextDisabled ("W:");
        }
        if (inside)
        {
            ImGui::Text ("Y: %d px", info_y_);
        }
        else
        {
            ImGui::TextDisabled ("Y:");
        }
        ImGui::SameLine (half);
        if (doc.has_selection)
        {
            geom::Rect r = doc.selection.content_bounds ().intersect (doc.bounds ());
            ImGui::Text ("H: %u px", r.height ());
        }
        else
        {
            ImGui::TextDisabled ("H:");
        }
        ImGui::Separator ();
        ImGui::Text ("Doc: %u x %u px, %.0f ppi", doc.size.width, doc.size.height, doc.resolution_dpi);
        ImGui::End ();
    }

    /* The Navigator: the whole image with the visible part framed in red. Clicking or dragging in it
       moves the view there; the slider and buttons zoom. */
    void App::navigator_window ()
    {
        ImGui::Begin ("Navigator");
        const engine::DocState* d = session_.active ();
        if (d == NULL || channel_tex_[0] == 0 || small_w_ <= 0 || zoom_ <= 0.0f)
        {
            ImGui::TextDisabled ("No document");
            ImGui::End ();
            return;
        }
        float k = ui_scale_;
        float W = (float) d->doc->size.width, H = (float) d->doc->size.height;
        ImVec2 avail = ImGui::GetContentRegionAvail ();
        float box_h  = fmaxf (avail.y - ImGui::GetFrameHeightWithSpacing () - 6 * k, 40 * k);
        float sc     = fminf (avail.x / W, box_h / H);
        float iw = W * sc, ih = H * sc;
        ImVec2 p = ImGui::GetCursorScreenPos ();
        ImVec2 a (p.x + (avail.x - iw) * 0.5f, p.y + (box_h - ih) * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        dl->AddImage ((ImTextureID) (intptr_t) channel_tex_[0], a, ImVec2 (a.x + iw, a.y + ih));
        /* The view in image coordinates. */
        float cx = W * 0.5f - pan_x_ / zoom_, cy = H * 0.5f - pan_y_ / zoom_;
        float hw = canvas_avail_x_ * 0.5f / zoom_, hh = canvas_avail_y_ * 0.5f / zoom_;
        ImVec2 r0 (a.x + fmaxf (cx - hw, 0.0f) * sc, a.y + fmaxf (cy - hh, 0.0f) * sc);
        ImVec2 r1 (a.x + fminf (cx + hw, W) * sc, a.y + fminf (cy + hh, H) * sc);
        dl->PushClipRect (p, ImVec2 (p.x + avail.x, p.y + box_h), true);
        dl->AddRect (r0, r1, IM_COL32 (230, 40, 40, 255), 0, 0, 2 * k);
        dl->PopClipRect ();
        ImGui::SetCursorScreenPos (p);
        ImGui::InvisibleButton ("##navview", ImVec2 (avail.x, box_h));
        if (ImGui::IsItemActive ())
        {
            ImVec2 m = ImGui::GetIO ().MousePos;
            float tx = fminf (fmaxf ((m.x - a.x) / sc, 0.0f), W), ty = fminf (fmaxf ((m.y - a.y) / sc, 0.0f), H);
            pan_x_    = (W * 0.5f - tx) * zoom_;
            pan_y_    = (H * 0.5f - ty) * zoom_;
            fit_view_ = false;
        }
        /* Zoom out, the zoom slider (log scale), zoom in. */
        ImGui::SetCursorScreenPos (ImVec2 (p.x, p.y + box_h + 6 * k));
        float box = ImGui::GetFrameHeight ();
        if (icon_button ("##navout", "minus", box, false, "Zoom Out"))
        {
            zoom_by (0.8f, canvas_center_x_, canvas_center_y_);
        }
        ImGui::SameLine (0, 4 * k);
        float pct = zoom_ * 100.0f;
        ImGui::SetNextItemWidth (fmaxf (avail.x - 2 * box - 8 * k, 40 * k));
        if (ImGui::SliderFloat ("##navzoom", &pct, 2.0f, 3200.0f, "%.1f%%",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
        {
            zoom_by (pct / 100.0f / zoom_, canvas_center_x_, canvas_center_y_);
        }
        ImGui::SameLine (0, 4 * k);
        if (icon_button ("##navin", "plus", box, false, "Zoom In"))
        {
            zoom_by (1.25f, canvas_center_x_, canvas_center_y_);
        }
        ImGui::End ();
    }

    /* Q: Quick Mask on or off. While on, the Brush, Pencil and Eraser paint the selection (white
       selects, black masks) and the canvas shows the unselected part in red. Leaving with everything
       still selected leaves no selection, as entering without one started. */
    void App::toggle_quick_mask ()
    {
        if (stroking_)
        {
            return;
        }
        quick_mask_               = !quick_mask_;
        channel_view_dirty_       = true;
        const engine::DocState* d = session_.active ();
        if (quick_mask_ || d == NULL || !d->doc->has_selection)
        {
            status_          = quick_mask_ ? "Quick Mask: paint black to mask, white to select" : "Quick Mask off";
            status_is_error_ = false;
            return;
        }
        std::vector<float> m = algo::mask_from_surface (&d->doc->selection, d->doc->bounds ());
        bool full            = !m.empty ();
        for (size_t i = 0; i < m.size () && full; i++)
        {
            full = m[i] >= 0.999f;
        }
        if (full)
        {
            run_command ("select.deselect", engine::Value::object ());
        }
    }

} // namespace ui
