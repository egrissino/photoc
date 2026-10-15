#include "engine/text.h"
#include "text/fonts.h"
#include "text/render.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <algorithm>
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char pc_font_inter_regular[], pc_font_inter_regular_end[];
extern const unsigned char pc_font_inter_medium[], pc_font_inter_medium_end[];
extern const unsigned char pc_font_inter_semibold[], pc_font_inter_semibold_end[];
extern const unsigned char pc_font_mono[], pc_font_mono_end[];

namespace ui
{

    /* The Horizontal Type tool. Click for point text, drag for a text box, click type to edit it.
       While editing, the text is typed straight onto the canvas (a working copy previewed live), with
       a caret and a selection; Enter starts a paragraph, Ctrl+Enter (or the tick, or a click outside
       the text) commits as one step, Escape cancels. The options bar and the Character and
       Paragraph panels set the selected text's style (all of it when nothing is selected), or the
       active type layer's, or the style new type starts with. */

    const char TYPE_AA_LABELS[]      = "None\0Sharp\0Crisp\0Strong\0Smooth\0";
    const char* const TYPE_AA_IDS[5] = {"none", "sharp", "crisp", "strong", "smooth"};

    /* The app's own fonts join the system ones. */
    void App::register_type_fonts ()
    {
        text::FontDb* db = session_.fonts ();
        db->add_memory_font (pc_font_inter_regular, (size_t) (pc_font_inter_regular_end - pc_font_inter_regular));
        db->add_memory_font (pc_font_inter_medium, (size_t) (pc_font_inter_medium_end - pc_font_inter_medium));
        db->add_memory_font (pc_font_inter_semibold, (size_t) (pc_font_inter_semibold_end - pc_font_inter_semibold));
        db->add_memory_font (pc_font_mono, (size_t) (pc_font_mono_end - pc_font_mono));
        type_families_                  = db->families ();
        type_style_.family              = db->default_family ();
        std::vector<std::string> styles = db->styles (type_style_.family);
        type_style_.style = std::find (styles.begin (), styles.end (), "Regular") != styles.end () || styles.empty ()
                              ? "Regular"
                              : styles[0];
    }

    /* Document point to text space (inverse of the layer's transform). */
    void doc_to_text (const double m[6], double x, double y, float* tx, float* ty)
    {
        double det = m[0] * m[3] - m[1] * m[2];
        if (fabs (det) < 1e-12)
        {
            det = 1e-12;
        }
        double dx = x - m[4], dy = y - m[5];
        *tx = (float) ((m[3] * dx - m[2] * dy) / det);
        *ty = (float) ((-m[1] * dx + m[0] * dy) / det);
    }

    ImVec2 text_to_screen (const double m[6], float x, float y, float ox, float oy, float zoom)
    {
        double dx = m[0] * x + m[2] * y + m[4], dy = m[1] * x + m[3] * y + m[5];
        return ImVec2 (ox + (float) dx * zoom, oy + (float) dy * zoom);
    }

    /* The topmost visible type layer under document point (x, y), or NO_LAYER. */
    doc::LayerId App::type_layer_at (float x, float y)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return doc::NO_LAYER;
        }
        std::vector<doc::WalkEntry> walk = d->doc->walk ();
        for (size_t i = walk.size (); i-- > 0;)
        {
            const doc::Layer* l = walk[i].layer;
            if (l->kind != doc::LayerKind::Text || !l->visible)
            {
                continue;
            }
            text::Layout lay;
            text::layout_text (l->text, session_.fonts (), d->doc->resolution_dpi, &lay);
            float tx, ty;
            doc_to_text (l->text.transform, x, y, &tx, &ty);
            float b[4] = {lay.bounds[0], lay.bounds[1], lay.bounds[2], lay.bounds[3]};
            if (l->text.box)
            {
                b[0] = fminf (b[0], l->text.box_rect[0]);
                b[1] = fminf (b[1], l->text.box_rect[1]);
                b[2] = fmaxf (b[2], l->text.box_rect[0] + l->text.box_rect[2]);
                b[3] = fmaxf (b[3], l->text.box_rect[1] + l->text.box_rect[3]);
            }
            float pad = 4.0f / (zoom_ > 0.0f ? zoom_ : 1.0f);
            if (tx >= b[0] - pad && tx <= b[2] + pad && ty >= b[1] - pad && ty <= b[3] + pad)
            {
                return l->id;
            }
        }
        return doc::NO_LAYER;
    }

    void App::begin_type_edit (doc::LayerId id, size_t caret, bool is_new)
    {
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (id) : NULL;
        if (l == NULL || l->kind != doc::LayerKind::Text)
        {
            return;
        }
        if (d->active_layer != id)
        {
            run_command ("layer.select", engine::Value{{"layer", id}});
        }
        type_active_   = true;
        type_new_      = is_new;
        type_layer_    = id;
        type_doc_      = d->doc->id;
        type_text_     = l->text;
        type_original_ = l->text;
        type_caret_ = type_anchor_ = std::min (caret, type_text_.text.size ());
        type_selecting_            = false;
        type_changed ();
    }

    /* Layout and preview of the working copy. */
    void App::type_changed ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        text::layout_text (type_text_, session_.fonts (), d->doc->resolution_dpi, &type_layout_);
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        doc::Layer* l                       = copy->layer_mut (type_layer_);
        if (l == NULL)
        {
            return;
        }
        l->text = type_text_;
        l->pixels =
            text::render_text (type_text_, type_layout_, session_.fonts (), copy->pixel_format (), session_.cmyk);
        preview_doc_ = copy;
        preview_serial_++;
        type_blink_ = ImGui::GetTime ();
    }

    void App::commit_type_edit ()
    {
        if (!type_active_)
        {
            return;
        }
        type_active_ = false;
        if (!ImGui::GetIO ().WantTextInput)
        {
            SDL_StopTextInput (window_);
        }
        clear_preview ();
        const engine::DocState* d = session_.active ();
        if (d == NULL || d->doc->id != type_doc_ || d->doc->layer (type_layer_) == NULL)
        {
            return;
        }
        if (type_text_.text.empty ())
        {
            /* Empty type is no type: a new layer goes away again, an emptied one is deleted. */
            if (type_new_)
            {
                run_command ("edit.undo", engine::Value::object ());
            }
            else
            {
                run_command ("layer.delete", engine::Value{{"layer", type_layer_}});
            }
            return;
        }
        if (!doc::same_text (type_text_, type_original_))
        {
            run_command ("type.edit", engine::Value{{"layer", type_layer_}, {"text", engine::text_json (type_text_)}});
        }
    }

    void App::cancel_type_edit ()
    {
        if (!type_active_)
        {
            return;
        }
        type_active_ = false;
        if (!ImGui::GetIO ().WantTextInput)
        {
            SDL_StopTextInput (window_);
        }
        clear_preview ();
        const engine::DocState* d = session_.active ();
        if (type_new_ && d != NULL && d->doc->id == type_doc_)
        {
            run_command ("edit.undo", engine::Value::object ());
        }
    }

    void type_selection (size_t caret, size_t anchor, size_t* a, size_t* b)
    {
        *a = caret < anchor ? caret : anchor;
        *b = caret < anchor ? anchor : caret;
    }

    /* Replaces the selection with s (typing, pasting, deleting). */
    void App::type_insert (const std::string& s)
    {
        size_t a, b;
        type_selection (type_caret_, type_anchor_, &a, &b);
        doc::text_replace (&type_text_, a, b, s);
        type_caret_ = type_anchor_ = a + s.size ();
        type_changed ();
    }

    size_t utf8_prev (const std::string& s, size_t i)
    {
        if (i == 0)
        {
            return 0;
        }
        i--;
        while (i > 0 && ((uint8_t) s[i] & 0xC0) == 0x80)
        {
            i--;
        }
        return i;
    }

    size_t utf8_after (const std::string& s, size_t i)
    {
        if (i >= s.size ())
        {
            return s.size ();
        }
        i++;
        while (i < s.size () && ((uint8_t) s[i] & 0xC0) == 0x80)
        {
            i++;
        }
        return i;
    }

    bool word_char (char c)
    {
        return ((uint8_t) c & 0x80) != 0 || isalnum ((unsigned char) c) || c == '_' || c == '\'';
    }

    /* Word boundaries for Ctrl+arrows and double-clicks. */
    size_t word_left (const std::string& s, size_t i)
    {
        while (i > 0 && !word_char (s[i - 1]))
        {
            i--;
        }
        while (i > 0 && word_char (s[i - 1]))
        {
            i--;
        }
        return i;
    }

    size_t word_right (const std::string& s, size_t i)
    {
        while (i < s.size () && !word_char (s[i]))
        {
            i++;
        }
        while (i < s.size () && word_char (s[i]))
        {
            i++;
        }
        return i;
    }

    /* Keys while editing type (other shortcuts are held back meanwhile). */
    void App::type_keys ()
    {
        if (!type_active_)
        {
            return;
        }
        ImGuiIO& io = ImGui::GetIO ();
        if (io.WantTextInput)
        {
            return;
        }
        /* SDL sends typed text only while text input is on (ImGui turns it on for its own fields
           only); the input method's window goes by the caret. */
        if (!SDL_TextInputActive (window_))
        {
            SDL_StartTextInput (window_);
        }
        const std::string& s = type_text_.text;
        bool shift = io.KeyShift, ctrl = io.KeyCtrl;
        if (ImGui::IsKeyPressed (ImGuiKey_Escape))
        {
            cancel_type_edit ();
            return;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_KeypadEnter) || (ctrl && ImGui::IsKeyPressed (ImGuiKey_Enter)))
        {
            commit_type_edit ();
            return;
        }
        size_t a, b;
        type_selection (type_caret_, type_anchor_, &a, &b);
        if (ctrl && ImGui::IsKeyPressed (ImGuiKey_A))
        {
            type_anchor_ = 0;
            type_caret_  = s.size ();
            type_blink_  = ImGui::GetTime ();
            return;
        }
        if (ctrl && (ImGui::IsKeyPressed (ImGuiKey_C) || ImGui::IsKeyPressed (ImGuiKey_X)) && b > a)
        {
            ImGui::SetClipboardText (s.substr (a, b - a).c_str ());
            if (ImGui::IsKeyPressed (ImGuiKey_X))
            {
                type_insert ("");
            }
            return;
        }
        if (ctrl && ImGui::IsKeyPressed (ImGuiKey_V))
        {
            const char* clip = ImGui::GetClipboardText ();
            if (clip != NULL)
            {
                std::string paste;
                for (const char* c = clip; *c != 0; c++)
                {
                    if (*c == '\r')
                    {
                        continue;
                    }
                    paste.push_back (*c);
                }
                type_insert (paste);
            }
            return;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_Enter))
        {
            type_insert ("\n");
            return;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_Backspace))
        {
            if (b == a && a > 0)
            {
                type_anchor_ = ctrl ? word_left (s, a) : utf8_prev (s, a);
            }
            type_insert ("");
            return;
        }
        if (ImGui::IsKeyPressed (ImGuiKey_Delete))
        {
            if (b == a && a < s.size ())
            {
                type_anchor_ = ctrl ? word_right (s, a) : utf8_after (s, a);
            }
            type_insert ("");
            return;
        }
        size_t to  = type_caret_;
        bool moved = true;
        if (ImGui::IsKeyPressed (ImGuiKey_LeftArrow))
        {
            to = !shift && b > a ? a : ctrl ? word_left (s, to) : utf8_prev (s, to);
        }
        else if (ImGui::IsKeyPressed (ImGuiKey_RightArrow))
        {
            to = !shift && b > a ? b : ctrl ? word_right (s, to) : utf8_after (s, to);
        }
        else if (ImGui::IsKeyPressed (ImGuiKey_UpArrow))
        {
            to = text::caret_move_vertical (type_layout_, to, -1);
        }
        else if (ImGui::IsKeyPressed (ImGuiKey_DownArrow))
        {
            to = text::caret_move_vertical (type_layout_, to, 1);
        }
        else if (ImGui::IsKeyPressed (ImGuiKey_Home))
        {
            to = ctrl ? 0 : text::line_home (type_layout_, to);
        }
        else if (ImGui::IsKeyPressed (ImGuiKey_End))
        {
            to = ctrl ? s.size () : text::line_end (type_layout_, to);
        }
        else
        {
            moved = false;
        }
        if (moved)
        {
            type_caret_ = to;
            if (!shift)
            {
                type_anchor_ = to;
            }
            type_blink_ = ImGui::GetTime ();
            return;
        }
        /* Typed characters. */
        if (!ctrl && io.InputQueueCharacters.Size > 0)
        {
            std::string typed;
            for (int i = 0; i < io.InputQueueCharacters.Size; i++)
            {
                ImWchar c = io.InputQueueCharacters[i];
                if (c < 0x20 || c == 0x7F)
                {
                    continue;
                }
                uint32_t cp = (uint32_t) c;
                if (cp < 0x80)
                {
                    typed.push_back ((char) cp);
                }
                else if (cp < 0x800)
                {
                    typed.push_back ((char) (0xC0 | cp >> 6));
                    typed.push_back ((char) (0x80 | (cp & 0x3F)));
                }
                else if (cp < 0x10000)
                {
                    typed.push_back ((char) (0xE0 | cp >> 12));
                    typed.push_back ((char) (0x80 | ((cp >> 6) & 0x3F)));
                    typed.push_back ((char) (0x80 | (cp & 0x3F)));
                }
                else
                {
                    typed.push_back ((char) (0xF0 | cp >> 18));
                    typed.push_back ((char) (0x80 | ((cp >> 12) & 0x3F)));
                    typed.push_back ((char) (0x80 | ((cp >> 6) & 0x3F)));
                    typed.push_back ((char) (0x80 | (cp & 0x3F)));
                }
            }
            if (!typed.empty ())
            {
                type_insert (typed);
            }
        }
    }

    /* The Type tool on the canvas; true when it handled the pointer. */
    bool App::type_tool_input (float ox, float oy)
    {
        if (tool_ != Tool::Type && !type_active_)
        {
            return false;
        }
        ImGuiIO& io = ImGui::GetIO ();
        float mx = (io.MousePos.x - ox) / zoom_, my = (io.MousePos.y - oy) / zoom_;
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetMouseCursor (ImGuiMouseCursor_TextInput);
        }
        if (type_active_)
        {
            const engine::DocState* d = session_.active ();
            const doc::Layer* l       = d != NULL ? d->doc->layer (type_layer_) : NULL;
            if (l == NULL || d->doc->id != type_doc_)
            {
                type_active_ = false;
                clear_preview ();
                return tool_ == Tool::Type;
            }
            float tx, ty;
            doc_to_text (type_text_.transform, mx, my, &tx, &ty);
            /* The box's handles: corners and edge midpoints resize it (text space). */
            if (type_text_.box)
            {
                float* r    = type_text_.box_rect;
                float hx[3] = {r[0], r[0] + r[2] * 0.5f, r[0] + r[2]}, hy[3] = {r[1], r[1] + r[3] * 0.5f, r[1] + r[3]};
                float grab = 6.0f / zoom_;
                int over   = -1;
                for (int j = 0; j < 3 && over < 0; j++)
                {
                    for (int i = 0; i < 3; i++)
                    {
                        if ((i == 1 && j == 1) || fabsf (tx - hx[i]) > grab || fabsf (ty - hy[j]) > grab)
                        {
                            continue;
                        }
                        over = j * 3 + i;
                        break;
                    }
                }
                if (over >= 0 || type_handle_ >= 0)
                {
                    int h = type_handle_ >= 0 ? type_handle_ : over;
                    ImGui::SetMouseCursor (h == 0 || h == 8   ? ImGuiMouseCursor_ResizeNWSE
                                           : h == 2 || h == 6 ? ImGuiMouseCursor_ResizeNESW
                                           : h == 1 || h == 7 ? ImGuiMouseCursor_ResizeNS
                                                              : ImGuiMouseCursor_ResizeEW);
                }
                if (activated && over >= 0)
                {
                    type_handle_ = over;
                    return true;
                }
                if (type_handle_ >= 0)
                {
                    if (active)
                    {
                        int i = type_handle_ % 3, j = type_handle_ / 3;
                        float x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3];
                        if (i == 0)
                        {
                            x0 = fminf (tx, x1 - 1.0f);
                        }
                        if (i == 2)
                        {
                            x1 = fmaxf (tx, x0 + 1.0f);
                        }
                        if (j == 0)
                        {
                            y0 = fminf (ty, y1 - 1.0f);
                        }
                        if (j == 2)
                        {
                            y1 = fmaxf (ty, y0 + 1.0f);
                        }
                        r[0] = x0;
                        r[1] = y0;
                        r[2] = x1 - x0;
                        r[3] = y1 - y0;
                        type_changed ();
                    }
                    if (released)
                    {
                        type_handle_ = -1;
                    }
                    return true;
                }
            }
            if (activated)
            {
                float pad  = 6.0f / zoom_;
                float b[4] = {
                    type_layout_.bounds[0], type_layout_.bounds[1], type_layout_.bounds[2], type_layout_.bounds[3]};
                if (type_text_.box)
                {
                    b[0] = fminf (b[0], type_text_.box_rect[0]);
                    b[1] = fminf (b[1], type_text_.box_rect[1]);
                    b[2] = fmaxf (b[2], type_text_.box_rect[0] + type_text_.box_rect[2]);
                    b[3] = fmaxf (b[3], type_text_.box_rect[1] + type_text_.box_rect[3]);
                }
                if (tx < b[0] - pad || tx > b[2] + pad || ty < b[1] - pad || ty > b[3] + pad)
                {
                    commit_type_edit ();
                    return true;
                }
                size_t pos = text::hit_test (type_layout_, tx, ty);
                if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                {
                    type_anchor_ = word_left (type_text_.text, pos);
                    type_caret_  = word_right (type_text_.text, pos);
                }
                else
                {
                    type_caret_ = pos;
                    if (!io.KeyShift)
                    {
                        type_anchor_ = pos;
                    }
                    type_selecting_ = true;
                }
                type_blink_ = ImGui::GetTime ();
            }
            else if (active && type_selecting_)
            {
                type_caret_ = text::hit_test (type_layout_, tx, ty);
            }
            if (released)
            {
                type_selecting_ = false;
            }
            return true;
        }
        /* Not editing: edit the type under the pointer, or start new type (drag: a box). */
        if (activated)
        {
            doc::LayerId hit = type_layer_at (mx, my);
            if (hit != doc::NO_LAYER)
            {
                const doc::Layer* l = session_.active ()->doc->layer (hit);
                text::Layout lay;
                text::layout_text (l->text, session_.fonts (), session_.active ()->doc->resolution_dpi, &lay);
                float tx, ty;
                doc_to_text (l->text.transform, mx, my, &tx, &ty);
                begin_type_edit (hit, text::hit_test (lay, tx, ty), false);
                return true;
            }
            type_dragging_ = true;
            type_drag_[0] = type_drag_[2] = mx;
            type_drag_[1] = type_drag_[3] = my;
        }
        if (!type_dragging_)
        {
            return true;
        }
        type_drag_[2] = mx;
        type_drag_[3] = my;
        if (!released)
        {
            return true;
        }
        type_dragging_ = false;
        if (session_.active () == NULL)
        {
            return true;
        }
        float x0 = fminf (type_drag_[0], type_drag_[2]), y0 = fminf (type_drag_[1], type_drag_[3]);
        float w = fabsf (type_drag_[2] - type_drag_[0]), h = fabsf (type_drag_[3] - type_drag_[1]);
        engine::Value p    = engine::char_style_json (type_style_);
        engine::Value para = engine::para_style_json (type_para_);
        for (engine::Value::iterator it = para.begin (); it != para.end (); ++it)
        {
            p[it.key ()] = it.value ();
        }
        char hex[8];
        const float* fg = session_.tools.foreground;
        snprintf (hex, sizeof hex, "#%02x%02x%02x", (int) lroundf (fg[0] * 255), (int) lroundf (fg[1] * 255),
            (int) lroundf (fg[2] * 255));
        p["color"]     = hex;
        p["antialias"] = TYPE_AA_IDS[(int) type_aa_];
        p["text"]      = "";
        if (w * zoom_ > 6.0f && h * zoom_ > 6.0f)
        {
            p["box"] = {x0, y0, w, h};
        }
        else
        {
            p["x"] = type_drag_[0];
            p["y"] = type_drag_[1];
        }
        run_command ("type.create", p);
        const engine::DocState* d = session_.active ();
        if (d != NULL && d->doc->layer (d->active_layer) != NULL
            && d->doc->layer (d->active_layer)->kind == doc::LayerKind::Text)
        {
            begin_type_edit (d->active_layer, 0, true);
        }
        return true;
    }

    /* The caret, the selection and the text box over the canvas. */
    void App::type_overlay (float ox, float oy)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        if (type_dragging_)
        {
            ImVec2 a (
                ox + fminf (type_drag_[0], type_drag_[2]) * zoom_, oy + fminf (type_drag_[1], type_drag_[3]) * zoom_);
            ImVec2 b (
                ox + fmaxf (type_drag_[0], type_drag_[2]) * zoom_, oy + fmaxf (type_drag_[1], type_drag_[3]) * zoom_);
            dl->AddRect (a, b, IM_COL32 (0, 0, 0, 200), 0, 0, 2.0f);
            dl->AddRect (a, b, IM_COL32 (255, 255, 255, 255));
        }
        if (!type_active_)
        {
            return;
        }
        const double* m = type_text_.transform;
        if (type_text_.box)
        {
            const float* r = type_text_.box_rect;
            ImVec2 q[4]    = {text_to_screen (m, r[0], r[1], ox, oy, zoom_),
                   text_to_screen (m, r[0] + r[2], r[1], ox, oy, zoom_),
                   text_to_screen (m, r[0] + r[2], r[1] + r[3], ox, oy, zoom_),
                   text_to_screen (m, r[0], r[1] + r[3], ox, oy, zoom_)};
            dl->AddPolyline (q, 4, IM_COL32 (0, 0, 0, 160), ImDrawFlags_Closed, 2.0f);
            dl->AddPolyline (q, 4, IM_COL32 (255, 255, 255, 255), ImDrawFlags_Closed, 1.0f);
            for (int j = 0; j < 3; j++)
            {
                for (int i = 0; i < 3; i++)
                {
                    if (i == 1 && j == 1)
                    {
                        continue;
                    }
                    ImVec2 c = text_to_screen (
                        m, r[0] + r[2] * 0.5f * (float) i, r[1] + r[3] * 0.5f * (float) j, ox, oy, zoom_);
                    dl->AddRectFilled (ImVec2 (c.x - 3.5f, c.y - 3.5f), ImVec2 (c.x + 3.5f, c.y + 3.5f),
                        IM_COL32 (255, 255, 255, 255));
                    dl->AddRect (
                        ImVec2 (c.x - 3.5f, c.y - 3.5f), ImVec2 (c.x + 3.5f, c.y + 3.5f), IM_COL32 (0, 0, 0, 255));
                }
            }
        }
        size_t a, b;
        type_selection (type_caret_, type_anchor_, &a, &b);
        if (b > a)
        {
            for (size_t i = 0; i < type_layout_.lines.size (); i++)
            {
                const text::Line& ln = type_layout_.lines[i];
                size_t s0 = std::max (a, ln.start), s1 = std::min (b, ln.end);
                if (s0 >= s1 && !(ln.start == ln.end && a <= ln.start && b > ln.start))
                {
                    continue;
                }
                float x0 = ln.right, x1 = ln.left;
                for (size_t c = 0; c < ln.carets.size (); c++)
                {
                    if (ln.carets[c].pos < s0 || ln.carets[c].pos > s1)
                    {
                        continue;
                    }
                    x0 = fminf (x0, ln.carets[c].x);
                    x1 = fmaxf (x1, ln.carets[c].x);
                }
                /* A selection running past the line's end covers the line to its right edge. */
                if (b > ln.end)
                {
                    x1 = fmaxf (x1, ln.right);
                }
                if (x1 <= x0)
                {
                    x1 = x0 + 3.0f;
                }
                float top = ln.baseline - ln.ascent, bot = ln.baseline + ln.descent;
                ImVec2 q[4] = {text_to_screen (m, x0, top, ox, oy, zoom_), text_to_screen (m, x1, top, ox, oy, zoom_),
                    text_to_screen (m, x1, bot, ox, oy, zoom_), text_to_screen (m, x0, bot, ox, oy, zoom_)};
                dl->AddConvexPolyFilled (q, 4, IM_COL32 (60, 130, 240, 90));
            }
        }
        /* The caret blinks, but stays on while it moves. */
        double t = ImGui::GetTime () - type_blink_;
        if (fmod (t, 1.06) < 0.62)
        {
            float x, top, bot;
            text::caret_at (type_layout_, type_caret_, &x, &top, &bot);
            if (bot <= top)
            {
                float size = text::pt_to_px (
                    doc::text_style_at (type_text_, type_caret_).size_pt, session_.active ()->doc->resolution_dpi);
                top = -size * 0.8f;
                bot = size * 0.2f;
            }
            ImVec2 p0 = text_to_screen (m, x, top, ox, oy, zoom_), p1 = text_to_screen (m, x, bot, ox, oy, zoom_);
            dl->AddLine (p0, p1, IM_COL32 (255, 255, 255, 255), 3.0f);
            dl->AddLine (p0, p1, IM_COL32 (0, 0, 0, 255), 1.5f);
        }
        {
            float x, top, bot;
            text::caret_at (type_layout_, type_caret_, &x, &top, &bot);
            ImVec2 c      = text_to_screen (m, x, bot, ox, oy, zoom_);
            SDL_Rect area = {(int) c.x, (int) c.y, 1, 1};
            SDL_SetTextInputArea (window_, &area, 0);
        }
    }

    /* The style the panels show: the selection's (or caret's) while editing, the active type
       layer's, else what new type starts with. */
    doc::CharStyle App::type_shown_style (doc::ParaStyle* para, doc::AntiAlias* aa)
    {
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        if (type_active_)
        {
            size_t a, b;
            type_selection (type_caret_, type_anchor_, &a, &b);
            std::vector<doc::ParaRun> pr = type_text_.para_runs ();
            size_t pi = 0, at = 0;
            while (pi + 1 < pr.size () && at + pr[pi].len <= a)
            {
                at += pr[pi++].len;
            }
            *para = pr[pi].style;
            *aa   = type_text_.antialias;
            return doc::text_style_at (type_text_, b > a ? a + 1 : a);
        }
        if (l != NULL && l->kind == doc::LayerKind::Text)
        {
            *para = l->text.para_runs ()[0].style;
            *aa   = l->text.antialias;
            return l->text.first_style ();
        }
        *para = type_para_;
        *aa   = type_aa_;
        return type_style_;
    }

    /* A character style change: on the selection being edited (all the text when nothing is
       selected), else on the active type layer, and always for new type. */
    void App::type_apply_style (const engine::Value& patch)
    {
        engine::Error err;
        engine::char_style_from (patch, &type_style_, "type", &err);
        std::string aa;
        if (engine::param_str (patch, "antialias", &aa))
        {
            for (int i = 0; i < 5; i++)
            {
                if (aa == TYPE_AA_IDS[i])
                {
                    type_aa_ = (doc::AntiAlias) i;
                }
            }
        }
        if (type_active_)
        {
            size_t a, b;
            type_selection (type_caret_, type_anchor_, &a, &b);
            if (a == b)
            {
                a = 0;
                b = type_text_.text.size ();
            }
            std::vector<doc::TextRun> runs = type_text_.char_runs ();
            size_t at                      = 0;
            std::vector<std::pair<std::pair<size_t, size_t>, doc::CharStyle>> changes;
            for (size_t i = 0; i < runs.size (); i++)
            {
                size_t ra = at, rb = at + runs[i].len;
                at        = rb;
                size_t x0 = std::max (ra, a), x1 = std::min (rb, b);
                if (x0 >= x1 && !type_text_.text.empty ())
                {
                    continue;
                }
                doc::CharStyle st = runs[i].style;
                engine::char_style_from (patch, &st, "type", &err);
                changes.push_back (std::make_pair (std::make_pair (x0, x1), st));
            }
            for (size_t i = 0; i < changes.size (); i++)
            {
                doc::text_set_style (&type_text_, changes[i].first.first, changes[i].first.second, changes[i].second);
            }
            if (!aa.empty ())
            {
                type_text_.antialias = type_aa_;
            }
            type_changed ();
            return;
        }
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        if (l != NULL && l->kind == doc::LayerKind::Text)
        {
            run_command ("type.setStyle", patch);
        }
    }

    void App::type_apply_para (const engine::Value& patch)
    {
        engine::Error err;
        engine::para_style_from (patch, &type_para_, "type", &err);
        if (type_active_)
        {
            size_t a, b;
            type_selection (type_caret_, type_anchor_, &a, &b);
            std::vector<doc::ParaRun> pr = type_text_.para_runs ();
            size_t at                    = 0;
            for (size_t i = 0; i < pr.size (); i++)
            {
                size_t pa = at, pb = at + pr[i].len;
                at = pb;
                if ((pa <= b && (pb > a || i + 1 == pr.size ())))
                {
                    engine::para_style_from (patch, &pr[i].style, "type", &err);
                }
            }
            type_text_.paras = pr;
            type_changed ();
            return;
        }
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        if (l != NULL && l->kind == doc::LayerKind::Text)
        {
            run_command ("type.setParagraph", patch);
        }
    }

    /* A dragged number of the type panels: the value shown while dragging, then applied as one
       step on release (while editing type it goes straight into the working copy). */
    float App::type_value (const char* id, float shown)
    {
        return type_drag_id_ == ImGui::GetID (id) ? type_drag_value_ : shown;
    }

    void App::type_drag (const char* id, const char* key, float v, bool changed, bool para)
    {
        ImGuiID gid         = ImGui::GetID (id);
        engine::Value patch = {{key, v}};
        if (ImGui::IsItemActive ())
        {
            type_drag_id_    = gid;
            type_drag_value_ = v;
        }
        if (type_active_)
        {
            if (changed)
            {
                if (para)
                {
                    type_apply_para (patch);
                }
                else
                {
                    type_apply_style (patch);
                }
            }
            if (!ImGui::IsItemActive () && type_drag_id_ == gid)
            {
                type_drag_id_ = 0;
            }
            return;
        }
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (d->active_layer) : NULL;
        bool layer                = l != NULL && l->kind == doc::LayerKind::Text;
        if (changed && ImGui::IsItemActive () && layer)
        {
            /* Preview on a copy of the layer. */
            std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
            doc::Layer* cl                      = copy->layer_mut (l->id);
            engine::Error err;
            if (para)
            {
                std::vector<doc::ParaRun> pr = cl->text.para_runs ();
                for (size_t i = 0; i < pr.size (); i++)
                {
                    engine::para_style_from (patch, &pr[i].style, "type", &err);
                }
                cl->text.paras = pr;
            }
            else
            {
                std::vector<doc::TextRun> runs = cl->text.char_runs ();
                for (size_t i = 0; i < runs.size (); i++)
                {
                    engine::char_style_from (patch, &runs[i].style, "type", &err);
                }
                cl->text.runs = runs;
            }
            text::render_layer_text (
                cl->text, session_.fonts (), copy->resolution_dpi, copy->pixel_format (), session_.cmyk, &cl->pixels);
            preview_doc_ = copy;
            preview_serial_++;
        }
        bool done = ImGui::IsItemDeactivatedAfterEdit () || (changed && !ImGui::IsItemActive ());
        if (done)
        {
            clear_preview ();
            if (para)
            {
                type_apply_para (patch);
            }
            else
            {
                type_apply_style (patch);
            }
        }
        if (!ImGui::IsItemActive () && type_drag_id_ == gid)
        {
            type_drag_id_ = 0;
        }
    }

    /* The font family combo with a filter, then the style combo; true when either changed. */
    void App::font_combos (const doc::CharStyle& st, float fam_w, float style_w, bool stacked)
    {
        float k = ui_scale_;
        ImGui::SetNextItemWidth (fam_w);
        if (ImGui::BeginCombo ("##typefamily", st.family.c_str (), ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::IsWindowAppearing ())
            {
                type_families_  = session_.fonts ()->families ();
                type_filter_[0] = 0;
                ImGui::SetKeyboardFocusHere ();
            }
            ImGui::SetNextItemWidth (-1);
            ImGui::InputTextWithHint ("##familyfilter", "Search fonts", type_filter_, sizeof type_filter_);
            for (size_t i = 0; i < type_families_.size (); i++)
            {
                const std::string& f = type_families_[i];
                if (type_filter_[0] != 0)
                {
                    std::string lf = f, q = type_filter_;
                    std::transform (lf.begin (), lf.end (), lf.begin (), ::tolower);
                    std::transform (q.begin (), q.end (), q.begin (), ::tolower);
                    if (lf.find (q) == std::string::npos)
                    {
                        continue;
                    }
                }
                if (ImGui::Selectable (f.c_str (), f == st.family))
                {
                    std::vector<std::string> styles = session_.fonts ()->styles (f);
                    std::string style               = st.style;
                    if (std::find (styles.begin (), styles.end (), style) == styles.end ())
                    {
                        style =
                            std::find (styles.begin (), styles.end (), "Regular") != styles.end () || styles.empty ()
                                ? "Regular"
                                : styles[0];
                    }
                    type_apply_style (engine::Value{{"font", f}, {"style", style}});
                }
            }
            ImGui::EndCombo ();
        }
        if (!stacked)
        {
            ImGui::SameLine (0, 4 * k);
        }
        ImGui::SetNextItemWidth (style_w);
        if (ImGui::BeginCombo ("##typestyle", st.style.c_str ()))
        {
            std::vector<std::string> styles = session_.fonts ()->styles (st.family);
            for (size_t i = 0; i < styles.size (); i++)
            {
                if (ImGui::Selectable (styles[i].c_str (), styles[i] == st.style))
                {
                    type_apply_style (engine::Value{{"style", styles[i]}});
                }
            }
            ImGui::EndCombo ();
        }
    }

    void App::type_options ()
    {
        if (tool_ != Tool::Type)
        {
            return;
        }
        float k = ui_scale_, box = ImGui::GetFrameHeight ();
        doc::ParaStyle para;
        doc::AntiAlias aa;
        doc::CharStyle st = type_shown_style (&para, &aa);
        font_combos (st, 180 * k, 110 * k, false);
        opt_gap ();
        float size = type_value ("##typesize", st.size_pt);
        ImGui::SetNextItemWidth (80 * k);
        bool ch = ImGui::DragFloat ("##typesize", &size, 0.25f, 1.0f, 1296.0f, "%.1f pt",
            ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        type_drag ("##typesize", "size", size, ch, false);
        opt_gap ();
        int aai = (int) aa;
        ImGui::SetNextItemWidth (90 * k);
        if (ImGui::Combo ("##typeaa", &aai, TYPE_AA_LABELS))
        {
            type_apply_style (engine::Value{{"antialias", TYPE_AA_IDS[aai]}});
        }
        opt_gap ();
        const char* icons[3] = {"align-left", "align-center", "align-right"};
        const char* tips[3]  = {"Left align text", "Center text", "Right align text"};
        const char* ids[3]   = {"left", "center", "right"};
        for (int i = 0; i < 3; i++)
        {
            char id[16];
            snprintf (id, sizeof id, "##talign%d", i);
            if (icon_button (id, icons[i], box, (int) para.align == i, tips[i]))
            {
                type_apply_para (engine::Value{{"align", ids[i]}});
            }
            ImGui::SameLine (0, 2 * k);
        }
        opt_gap ();
        float c[3] = {st.color[0], st.color[1], st.color[2]};
        if (ImGui::ColorEdit3 ("##typecolor", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
        {
            char hex[8];
            snprintf (hex, sizeof hex, "#%02x%02x%02x", (int) lroundf (c[0] * 255), (int) lroundf (c[1] * 255),
                (int) lroundf (c[2] * 255));
            type_apply_style (engine::Value{{"color", hex}});
        }
        if (ImGui::IsItemHovered ())
        {
            ImGui::SetTooltip ("Set the text color");
        }
        if (type_active_)
        {
            opt_gap ();
            if (icon_button ("##typecancel", "x", box, false, "Cancel all current edits  (Esc)"))
            {
                cancel_type_edit ();
            }
            ImGui::SameLine (0, 2 * k);
            if (icon_button ("##typecommit", "check", box, false, "Commit all current edits  (Ctrl+Enter)"))
            {
                commit_type_edit ();
            }
        }
    }

    /* Window > Character: the full character style. */
    void App::character_window ()
    {
        ImGui::Begin ("Character");
        float k = ui_scale_, half = (ImGui::GetContentRegionAvail ().x - 8 * k) * 0.5f;
        doc::ParaStyle para;
        doc::AntiAlias aa;
        doc::CharStyle st = type_shown_style (&para, &aa);
        font_combos (st, ImGui::GetContentRegionAvail ().x, ImGui::GetContentRegionAvail ().x, true);
        float v = type_value ("##csize", st.size_pt);
        ImGui::SetNextItemWidth (half);
        bool size_ch = ImGui::DragFloat ("##csize", &v, 0.25f, 1.0f, 1296.0f, "Size %.1f pt",
            ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        type_drag ("##csize", "size", v, size_ch, false);
        ImGui::SameLine (0, 8 * k);
        v = type_value ("##clead", st.leading_pt);
        ImGui::SetNextItemWidth (half);
        bool lch = ImGui::DragFloat ("##clead", &v, 0.25f, 0.0f, 5000.0f,
            v <= 0.0f ? "Leading (Auto)" : "Leading %.1f pt", ImGuiSliderFlags_AlwaysClamp);
        type_drag ("##clead", "leading", v, lch, false);
        v = type_value ("##ctrack", st.tracking);
        ImGui::SetNextItemWidth (half);
        bool track_ch =
            ImGui::DragFloat ("##ctrack", &v, 1.0f, -1000.0f, 10000.0f, "Tracking %.0f", ImGuiSliderFlags_AlwaysClamp);
        type_drag ("##ctrack", "tracking", v, track_ch, false);
        ImGui::SameLine (0, 8 * k);
        bool kern = st.kerning;
        if (ImGui::Checkbox ("Kerning", &kern))
        {
            type_apply_style (engine::Value{{"kerning", kern}});
        }
        v = type_value ("##cvs", st.vscale * 100.0f);
        ImGui::SetNextItemWidth (half);
        bool vscale_ch =
            ImGui::DragFloat ("##cvs", &v, 0.5f, 1.0f, 1000.0f, "Vertical %.0f%%", ImGuiSliderFlags_AlwaysClamp);
        type_drag ("##cvs", "verticalScale", v, vscale_ch, false);
        ImGui::SameLine (0, 8 * k);
        v = type_value ("##chs", st.hscale * 100.0f);
        ImGui::SetNextItemWidth (half);
        bool hscale_ch =
            ImGui::DragFloat ("##chs", &v, 0.5f, 1.0f, 1000.0f, "Horizontal %.0f%%", ImGuiSliderFlags_AlwaysClamp);
        type_drag ("##chs", "horizontalScale", v, hscale_ch, false);
        v = type_value ("##cshift", st.baseline_shift_pt);
        ImGui::SetNextItemWidth (half);
        bool shift_ch = ImGui::DragFloat (
            "##cshift", &v, 0.25f, -5000.0f, 5000.0f, "Baseline %.1f pt", ImGuiSliderFlags_AlwaysClamp);
        type_drag ("##cshift", "baselineShift", v, shift_ch, false);
        ImGui::SameLine (0, 8 * k);
        float c[3] = {st.color[0], st.color[1], st.color[2]};
        if (ImGui::ColorEdit3 ("Color##ccolor", c, ImGuiColorEditFlags_NoInputs))
        {
            char hex[8];
            snprintf (hex, sizeof hex, "#%02x%02x%02x", (int) lroundf (c[0] * 255), (int) lroundf (c[1] * 255),
                (int) lroundf (c[2] * 255));
            type_apply_style (engine::Value{{"color", hex}});
        }
        /* Faux Bold, Faux Italic, All Caps, Underline, Strikethrough. */
        const char* labels[5] = {"B##fb", "I##fi", "TT##caps", "U##ul", "S##st"};
        const char* keys[5]   = {"fauxBold", "fauxItalic", "allCaps", "underline", "strikethrough"};
        const char* tips[5]   = {"Faux Bold", "Faux Italic", "All Caps", "Underline", "Strikethrough"};
        bool on[5]            = {st.faux_bold, st.faux_italic, st.all_caps, st.underline, st.strikethrough};
        float bw              = 30 * k;
        for (int i = 0; i < 5; i++)
        {
            if (i > 0)
            {
                ImGui::SameLine (0, 4 * k);
            }
            if (on[i])
            {
                ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
            }
            ImVec2 p = ImGui::GetCursorScreenPos ();
            if (ImGui::Button (labels[i], ImVec2 (bw, 0)))
            {
                type_apply_style (engine::Value{{keys[i], !on[i]}});
            }
            if (on[i])
            {
                ImGui::PopStyleColor ();
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetTooltip ("%s", tips[i]);
            }
            ImDrawList* dl = ImGui::GetWindowDrawList ();
            float h        = ImGui::GetFrameHeight ();
            if (i == 3)
            {
                dl->AddLine (ImVec2 (p.x + 11 * k, p.y + h - 5 * k), ImVec2 (p.x + bw - 11 * k, p.y + h - 5 * k),
                    theme_.text, 1.0f * k);
            }
            if (i == 4)
            {
                dl->AddLine (ImVec2 (p.x + 10 * k, p.y + h * 0.5f + 1 * k),
                    ImVec2 (p.x + bw - 10 * k, p.y + h * 0.5f + 1 * k), theme_.text, 1.0f * k);
            }
        }
        int aai = (int) aa;
        ImGui::SetNextItemWidth (-1);
        if (ImGui::Combo ("##caa", &aai, TYPE_AA_LABELS))
        {
            type_apply_style (engine::Value{{"antialias", TYPE_AA_IDS[aai]}});
        }
        ImGui::End ();
    }

    /* Window > Paragraph: alignment, indents and spacing. */
    void App::paragraph_window ()
    {
        ImGui::Begin ("Paragraph");
        float k = ui_scale_, box = ImGui::GetFrameHeight ();
        doc::ParaStyle para;
        doc::AntiAlias aa;
        type_shown_style (&para, &aa);
        const char* icons[7] = {"align-left", "align-center", "align-right", "align-justify", "align-justify",
            "align-justify", "align-justify"};
        const char* tips[7]  = {"Left align text", "Center text", "Right align text", "Justify last left",
             "Justify last centered", "Justify last right", "Justify all"};
        const char* ids[7] = {"left", "center", "right", "justifyLeft", "justifyCenter", "justifyRight", "justifyAll"};
        for (int i = 0; i < 7; i++)
        {
            char id[16];
            snprintf (id, sizeof id, "##palign%d", i);
            if (i > 0)
            {
                ImGui::SameLine (0, 2 * k);
            }
            if (icon_button (id, icons[i], box, (int) para.align == i, tips[i]))
            {
                type_apply_para (engine::Value{{"align", ids[i]}});
            }
        }
        float half          = (ImGui::GetContentRegionAvail ().x - 8 * k) * 0.5f;
        const char* keys[5] = {"startIndent", "endIndent", "firstIndent", "spaceBefore", "spaceAfter"};
        const char* fmts[5] = {"Indent left %.1f pt", "Indent right %.1f pt", "First line %.1f pt",
            "Space before %.1f pt", "Space after %.1f pt"};
        float vals[5]       = {
            para.start_indent_pt, para.end_indent_pt, para.first_indent_pt, para.space_before_pt, para.space_after_pt};
        for (int i = 0; i < 5; i++)
        {
            if (i % 2 == 1)
            {
                ImGui::SameLine (0, 8 * k);
            }
            char id[16];
            snprintf (id, sizeof id, "##pv%d", i);
            ImGui::SetNextItemWidth (half);
            float v = type_value (id, vals[i]);
            bool ch = ImGui::DragFloat (
                id, &v, 0.25f, i == 2 ? -1296.0f : 0.0f, 1296.0f, fmts[i], ImGuiSliderFlags_AlwaysClamp);
            type_drag (id, keys[i], v, ch, true);
        }
        ImGui::End ();
    }

} // namespace ui
