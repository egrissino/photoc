#include "io/io.h"
#include "ui/app.h"
#include "ui/widgets.h"
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace ui
{

    /* The Actions panel: sets, their actions and each action's steps (a checkbox to skip a step,
       the dialog toggle for steps with settings), the record / stop / play buttons, and Batch.
       Actions live in actions.json beside the panel layout, saved whenever they change. A step
       with its dialog on pauses playback in that dialog (with the recorded settings); OK carries
       on, Cancel stops. */

    /* actions.json next to the layout file; empty when there is none (screenshots). */
    std::string actions_path ()
    {
        const char* ini = ImGui::GetIO ().IniFilename;
        if (ini == NULL)
        {
            return "";
        }
        return std::string (ini, (size_t) (io::path_base (ini) - ini)) + "actions.json";
    }

    /* Loads the saved actions (the Default Actions the first time), then saves them whenever they
       change. */
    void App::sync_actions ()
    {
        std::string path = actions_path ();
        if (path.empty ())
        {
            return;
        }
        engine::Value r;
        engine::Error e;
        if (!actions_loaded_)
        {
            actions_loaded_ = true;
            session_.execute (
                "actions.import", engine::Value{{"path", path}, {"replace", true}, {"defaults", true}}, &r, &e);
        }
        if (!session_.execute ("actions.list", engine::Value::object (), &r, &e))
        {
            return;
        }
        r.erase ("recording");
        size_t h = std::hash<std::string> () (r.dump ());
        if (h == actions_hash_)
        {
            return;
        }
        if (actions_hash_ != 0)
        {
            session_.execute ("actions.export", engine::Value{{"path", path}}, &r, &e);
        }
        actions_hash_ = h;
    }

    /* Plays an action from step `from`; a dialog step opens its dialog (filters) or, for other
       commands, runs with the recorded settings. */
    void App::play_action (int set, int action, int from)
    {
        if (session_.active () == NULL)
        {
            set_error ("open a document to play an action on");
            return;
        }
        for (int guard = 0; guard < 10000; guard++)
        {
            engine::Value r;
            engine::Error e;
            if (!session_.execute (
                    "actions.play", engine::Value{{"set", set}, {"action", action}, {"from", from}}, &r, &e))
            {
                set_error (e.message);
                return;
            }
            if (!r.contains ("paused"))
            {
                status_          = "Played action";
                status_is_error_ = false;
                return;
            }
            int step        = r["paused"].get<int> ();
            std::string cmd = r["command"].get<std::string> ();
            if (load_filter_dialog (cmd.c_str (), r["params"]))
            {
                filter_mode_ = FILTER_ACTION_RESUME;
                act_ref_[0]  = set;
                act_ref_[1]  = action;
                act_ref_[2]  = step;
                return;
            }
            /* No dialog here: the step runs as recorded. */
            if (!session_.execute (
                    "actions.play", engine::Value{{"set", set}, {"action", action}, {"step", step}}, &r, &e))
            {
                set_error (e.message);
                return;
            }
            from = step + 1;
        }
    }

    /* A short summary of a step's settings ("radius: 2, threshold: 0"). */
    std::string step_summary (const engine::Value& params)
    {
        std::string out;
        if (!params.is_object ())
        {
            return out;
        }
        for (engine::Value::const_iterator it = params.begin (); it != params.end (); ++it)
        {
            if (it.key () == "seed")
            {
                continue;
            }
            std::string v = it.value ().is_string () ? it.value ().get<std::string> () : it.value ().dump ();
            if (v.size () > 24)
            {
                v = v.substr (0, 21) + "...";
            }
            if (!out.empty ())
            {
                out += ", ";
            }
            out += it.key () + ": " + v;
            if (out.size () > 80)
            {
                break;
            }
        }
        return out;
    }

    void App::actions_window ()
    {
        sync_actions ();
        /* Docked beside History the first time, whatever layout was saved before. */
        ImGuiWindow* history = ImGui::FindWindowByName ("History");
        if (history != NULL && history->DockId != 0)
        {
            ImGui::SetNextWindowDockID (history->DockId, ImGuiCond_FirstUseEver);
        }
        ImGui::Begin ("Actions");
        engine::Value list;
        engine::Error e;
        session_.execute ("actions.list", engine::Value::object (), &list, &e);
        float k         = ui_scale_;
        bool recording  = session_.recording.on;
        float buttons_h = 24.0f * k + ImGui::GetStyle ().ItemSpacing.y * 2;
        ImGui::BeginChild ("##actions", ImVec2 (0, -buttons_h), ImGuiChildFlags_Borders);
        const engine::Value& sets = list["sets"];
        int del_set = -1, del_action = -1, del_step = -1;
        for (size_t si = 0; si < sets.size (); si++)
        {
            const engine::Value& set = sets[si];
            ImGui::PushID ((int) si);
            bool set_sel          = act_sel_[0] == (int) si && act_sel_[1] < 0;
            ImGuiTreeNodeFlags sf = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
                                  | (set_sel ? ImGuiTreeNodeFlags_Selected : 0);
            std::string set_name = set["name"].get<std::string> ();
            bool open            = ImGui::TreeNodeEx ("##set", sf, "%s", set_name.c_str ());
            if (ImGui::IsItemClicked () && !ImGui::IsItemToggledOpen ())
            {
                act_sel_[0] = (int) si;
                act_sel_[1] = act_sel_[2] = -1;
            }
            if (ImGui::BeginPopupContextItem ("##setctx"))
            {
                if (ImGui::MenuItem ("New Action…"))
                {
                    act_sel_[0] = (int) si;
                    act_naming_ = 2;
                    snprintf (act_name_, sizeof act_name_, "Action %zu", set["actions"].size () + 1);
                }
                if (ImGui::MenuItem ("Rename Set…"))
                {
                    act_sel_[0] = (int) si;
                    act_sel_[1] = -1;
                    act_naming_ = 3;
                    snprintf (act_name_, sizeof act_name_, "%s", set_name.c_str ());
                }
                if (ImGui::MenuItem ("Delete Set"))
                {
                    del_set = (int) si;
                }
                ImGui::EndPopup ();
            }
            if (open)
            {
                const engine::Value& acts = set["actions"];
                for (size_t ai = 0; ai < acts.size (); ai++)
                {
                    const engine::Value& ac = acts[ai];
                    ImGui::PushID ((int) ai);
                    bool rec_here = recording && list["recording"]["set"] == si && list["recording"]["action"] == ai;
                    bool act_sel  = act_sel_[0] == (int) si && act_sel_[1] == (int) ai && act_sel_[2] < 0;
                    ImGuiTreeNodeFlags af = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth
                                          | (act_sel ? ImGuiTreeNodeFlags_Selected : 0);
                    std::string name = ac["name"].get<std::string> ();
                    if (rec_here)
                    {
                        ImGui::PushStyleColor (ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4 (theme_.danger));
                    }
                    bool aopen =
                        ImGui::TreeNodeEx ("##action", af, "%s%s", name.c_str (), rec_here ? "  (recording)" : "");
                    if (rec_here)
                    {
                        ImGui::PopStyleColor ();
                    }
                    if (ImGui::IsItemClicked () && !ImGui::IsItemToggledOpen ())
                    {
                        act_sel_[0] = (int) si;
                        act_sel_[1] = (int) ai;
                        act_sel_[2] = -1;
                    }
                    if (ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
                    {
                        play_action ((int) si, (int) ai, 0);
                    }
                    if (ImGui::BeginPopupContextItem ("##actctx"))
                    {
                        act_sel_[0] = (int) si;
                        act_sel_[1] = (int) ai;
                        act_sel_[2] = -1;
                        if (ImGui::MenuItem ("Play"))
                        {
                            play_action ((int) si, (int) ai, 0);
                        }
                        if (ImGui::MenuItem ("Start Recording", NULL, false, !recording))
                        {
                            run_command ("actions.record", engine::Value{{"set", si}, {"action", ai}});
                        }
                        if (ImGui::MenuItem ("Duplicate"))
                        {
                            run_command ("actions.duplicate", engine::Value{{"set", si}, {"action", ai}});
                        }
                        if (ImGui::MenuItem ("Rename…"))
                        {
                            act_naming_ = 4;
                            snprintf (act_name_, sizeof act_name_, "%s", name.c_str ());
                        }
                        if (ImGui::MenuItem ("Delete"))
                        {
                            del_set    = (int) si;
                            del_action = (int) ai;
                        }
                        ImGui::EndPopup ();
                    }
                    if (aopen)
                    {
                        const engine::Value& steps = ac["steps"];
                        for (size_t ki = 0; ki < steps.size (); ki++)
                        {
                            const engine::Value& st = steps[ki];
                            ImGui::PushID ((int) ki);
                            bool enabled = st["enabled"].get<bool> (), dialog = st["dialog"].get<bool> ();
                            std::string cmd = st["command"].get<std::string> ();
                            if (ImGui::Checkbox ("##en", &enabled))
                            {
                                run_command ("actions.setStep",
                                    engine::Value{{"set", si}, {"action", ai}, {"step", ki}, {"enabled", enabled}});
                            }
                            if (ImGui::IsItemHovered ())
                            {
                                ImGui::SetTooltip ("Include this step when playing");
                            }
                            ImGui::SameLine ();
                            /* The dialog toggle, for steps that have one. */
                            bool has_dialog = has_filter_dialog (cmd.c_str ());
                            float box       = ImGui::GetFrameHeight ();
                            if (!has_dialog)
                            {
                                ImGui::BeginDisabled ();
                            }
                            if (icon_button ("##dlg", "app-window", box, dialog && has_dialog,
                                    has_dialog ? "Show the dialog when playing" : "This step has no dialog"))
                            {
                                run_command ("actions.setStep",
                                    engine::Value{{"set", si}, {"action", ai}, {"step", ki}, {"dialog", !dialog}});
                            }
                            if (!has_dialog)
                            {
                                ImGui::EndDisabled ();
                            }
                            ImGui::SameLine ();
                            bool step_sel =
                                act_sel_[0] == (int) si && act_sel_[1] == (int) ai && act_sel_[2] == (int) ki;
                            std::string label   = st["label"].get<std::string> ();
                            std::string summary = step_summary (st["params"]);
                            if (ImGui::Selectable (label.c_str (), step_sel, ImGuiSelectableFlags_AllowDoubleClick))
                            {
                                act_sel_[0] = (int) si;
                                act_sel_[1] = (int) ai;
                                act_sel_[2] = (int) ki;
                                if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left) && has_dialog
                                    && load_filter_dialog (cmd.c_str (), st["params"]))
                                {
                                    filter_mode_ = FILTER_STEP_EDIT;
                                    act_ref_[0]  = (int) si;
                                    act_ref_[1]  = (int) ai;
                                    act_ref_[2]  = (int) ki;
                                }
                            }
                            if (!summary.empty () && ImGui::IsItemHovered ())
                            {
                                ImGui::SetTooltip ("%s", summary.c_str ());
                            }
                            if (ImGui::BeginPopupContextItem ("##stepctx"))
                            {
                                if (ImGui::MenuItem ("Play Step"))
                                {
                                    engine::Value r;
                                    engine::Error pe;
                                    if (!session_.execute ("actions.play",
                                            engine::Value{{"set", si}, {"action", ai}, {"step", ki}}, &r, &pe))
                                    {
                                        set_error (pe.message);
                                    }
                                }
                                if (ImGui::MenuItem ("Edit Settings…", NULL, false, has_dialog)
                                    && load_filter_dialog (cmd.c_str (), st["params"]))
                                {
                                    filter_mode_ = FILTER_STEP_EDIT;
                                    act_ref_[0]  = (int) si;
                                    act_ref_[1]  = (int) ai;
                                    act_ref_[2]  = (int) ki;
                                }
                                if (ImGui::MenuItem ("Move Up", NULL, false, ki > 0))
                                {
                                    run_command ("actions.moveStep",
                                        engine::Value{{"set", si}, {"action", ai}, {"step", ki}, {"to", ki - 1}});
                                }
                                if (ImGui::MenuItem ("Move Down", NULL, false, ki + 1 < steps.size ()))
                                {
                                    run_command ("actions.moveStep",
                                        engine::Value{{"set", si}, {"action", ai}, {"step", ki}, {"to", ki + 1}});
                                }
                                if (ImGui::MenuItem ("Delete Step"))
                                {
                                    del_set    = (int) si;
                                    del_action = (int) ai;
                                    del_step   = (int) ki;
                                }
                                ImGui::EndPopup ();
                            }
                            if (!summary.empty ())
                            {
                                ImGui::SameLine ();
                                ImGui::TextDisabled ("%s", summary.c_str ());
                            }
                            ImGui::PopID ();
                        }
                        ImGui::TreePop ();
                    }
                    ImGui::PopID ();
                }
                ImGui::TreePop ();
            }
            ImGui::PopID ();
        }
        ImGui::EndChild ();
        if (del_step >= 0)
        {
            run_command ("actions.delete", engine::Value{{"set", del_set}, {"action", del_action}, {"step", del_step}});
        }
        else if (del_action >= 0)
        {
            run_command ("actions.delete", engine::Value{{"set", del_set}, {"action", del_action}});
        }
        else if (del_set >= 0)
        {
            run_command ("actions.delete", engine::Value{{"set", del_set}});
        }
        if (del_set >= 0)
        {
            act_sel_[0] = act_sel_[1] = act_sel_[2] = -1;
        }

        /* Stop, Record, Play, New Set, New Action, Delete. */
        float box        = 24.0f * k;
        bool have_action = act_sel_[0] >= 0 && act_sel_[1] >= 0;
        if (!recording)
        {
            ImGui::BeginDisabled ();
        }
        if (icon_button ("##stop", "square", box, false, "Stop playing/recording"))
        {
            run_command ("actions.stop", engine::Value::object ());
        }
        if (!recording)
        {
            ImGui::EndDisabled ();
        }
        ImGui::SameLine ();
        if (recording || !have_action)
        {
            ImGui::BeginDisabled ();
        }
        ImVec2 rp = ImGui::GetCursorScreenPos ();
        if (icon_button ("##record", "circle", box, recording, "Begin recording"))
        {
            run_command ("actions.record", engine::Value{{"set", act_sel_[0]}, {"action", act_sel_[1]}});
        }
        if (recording)
        {
            ImGui::GetWindowDrawList ()->AddCircleFilled (
                ImVec2 (rp.x + box * 0.5f, rp.y + box * 0.5f), box * 0.22f, theme_.danger);
        }
        if (recording || !have_action)
        {
            ImGui::EndDisabled ();
        }
        ImGui::SameLine ();
        if (!have_action || recording)
        {
            ImGui::BeginDisabled ();
        }
        if (icon_button ("##play", "play", box, false, "Play selection"))
        {
            play_action (act_sel_[0], act_sel_[1], act_sel_[2] >= 0 ? act_sel_[2] : 0);
        }
        if (!have_action || recording)
        {
            ImGui::EndDisabled ();
        }
        ImGui::SameLine (0, 12 * k);
        if (icon_button ("##newset", "folder-plus", box, false, "Create new set"))
        {
            act_naming_ = 1;
            snprintf (act_name_, sizeof act_name_, "Set %zu", sets.size () + 1);
        }
        ImGui::SameLine ();
        if (act_sel_[0] < 0)
        {
            ImGui::BeginDisabled ();
        }
        if (icon_button ("##newaction", "file-plus", box, false, "Create new action"))
        {
            act_naming_ = 2;
            size_t n    = act_sel_[0] >= 0 && (size_t) act_sel_[0] < sets.size ()
                            ? sets[(size_t) act_sel_[0]]["actions"].size ()
                            : 0;
            snprintf (act_name_, sizeof act_name_, "Action %zu", n + 1);
        }
        if (act_sel_[0] < 0)
        {
            ImGui::EndDisabled ();
        }
        ImGui::SameLine ();
        if (act_sel_[0] < 0)
        {
            ImGui::BeginDisabled ();
        }
        if (icon_button ("##delete", "trash", box, false, "Delete"))
        {
            engine::Value p = {{"set", act_sel_[0]}};
            if (act_sel_[1] >= 0)
            {
                p["action"] = act_sel_[1];
            }
            if (act_sel_[2] >= 0)
            {
                p["step"] = act_sel_[2];
            }
            run_command ("actions.delete", p);
            if (act_sel_[2] >= 0)
            {
                act_sel_[2] = -1;
            }
            else if (act_sel_[1] >= 0)
            {
                act_sel_[1] = -1;
            }
            else
            {
                act_sel_[0] = -1;
            }
        }
        if (act_sel_[0] < 0)
        {
            ImGui::EndDisabled ();
        }
        ImGui::End ();
        action_name_dialog ();
    }

    /* Names for New Set (1), New Action (2, then recording), Rename Set (3) and Rename Action (4). */
    void App::action_name_dialog ()
    {
        if (act_naming_ == 0)
        {
            return;
        }
        const char* titles[5] = {"", "New Set", "New Action", "Rename Set", "Rename Action"};
        char title[64];
        snprintf (title, sizeof title, "%s###actname", titles[act_naming_]);
        ImGui::OpenPopup ("###actname");
        if (!ImGui::BeginPopupModal (title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        if (ImGui::IsWindowAppearing ())
        {
            ImGui::SetKeyboardFocusHere ();
        }
        ImGui::SetNextItemWidth (260 * k);
        bool enter = ImGui::InputText ("Name", act_name_, sizeof act_name_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing ();
        bool ok = ImGui::Button (act_naming_ == 2 ? "Record" : "OK", ImVec2 (90 * k, 0)) || enter;
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok && act_name_[0] != 0)
        {
            engine::Value r;
            engine::Error e;
            if (act_naming_ == 1 && session_.execute ("actions.newSet", engine::Value{{"name", act_name_}}, &r, &e))
            {
                act_sel_[0] = r["set"].get<int> ();
                act_sel_[1] = act_sel_[2] = -1;
            }
            else if (act_naming_ == 2
                     && session_.execute (
                         "actions.newAction", engine::Value{{"set", act_sel_[0]}, {"name", act_name_}}, &r, &e))
            {
                act_sel_[1] = r["action"].get<int> ();
                act_sel_[2] = -1;
                status_     = "Recording…";
            }
            else if (act_naming_ == 3)
            {
                run_command ("actions.rename", engine::Value{{"set", act_sel_[0]}, {"name", act_name_}});
            }
            else if (act_naming_ == 4)
            {
                run_command ("actions.rename",
                    engine::Value{{"set", act_sel_[0]}, {"action", act_sel_[1]}, {"name", act_name_}});
            }
        }
        if ((ok && act_name_[0] != 0) || cancel)
        {
            act_naming_ = 0;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    void folder_dialog_done (void* userdata, const char* const* files, int filter)
    {
        DialogResult r;
        r.kind   = ((App*) userdata)->pending_folder_kind ();
        r.filter = filter;
        if (files == NULL)
        {
            r.error = std::string ("the folder dialog failed: ") + SDL_GetError ();
        }
        else if (files[0] != NULL)
        {
            r.path = files[0];
        }
        ((App*) userdata)->dialog_finished (r);
    }

    /* File > Automate > Batch: an action over every image in a folder, saved into another folder
       (or over the Photoshop originals). */
    void App::batch_dialog ()
    {
        if (!show_batch_)
        {
            return;
        }
        ImGui::OpenPopup ("Batch###batch");
        if (!ImGui::BeginPopupModal ("Batch###batch", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        engine::Value list;
        engine::Error e;
        session_.execute ("actions.list", engine::Value::object (), &list, &e);
        const engine::Value& sets = list["sets"];
        if (batch_set_ >= (int) sets.size ())
        {
            batch_set_ = 0;
        }
        ImGui::SetNextItemWidth (260 * k);
        if (ImGui::BeginCombo (
                "Set", sets.empty () ? "" : sets[(size_t) batch_set_]["name"].get<std::string> ().c_str ()))
        {
            for (size_t i = 0; i < sets.size (); i++)
            {
                if (ImGui::Selectable (sets[i]["name"].get<std::string> ().c_str (), (int) i == batch_set_))
                {
                    batch_set_    = (int) i;
                    batch_action_ = 0;
                }
            }
            ImGui::EndCombo ();
        }
        size_t nacts = sets.empty () ? 0 : sets[(size_t) batch_set_]["actions"].size ();
        if (batch_action_ >= (int) nacts)
        {
            batch_action_ = 0;
        }
        ImGui::SetNextItemWidth (260 * k);
        if (ImGui::BeginCombo ("Action", nacts == 0
                                             ? ""
                                             : sets[(size_t) batch_set_]["actions"][(size_t) batch_action_]["name"]
                                                   .get<std::string> ()
                                                   .c_str ()))
        {
            for (size_t i = 0; i < nacts; i++)
            {
                if (ImGui::Selectable (sets[(size_t) batch_set_]["actions"][i]["name"].get<std::string> ().c_str (),
                        (int) i == batch_action_))
                {
                    batch_action_ = (int) i;
                }
            }
            ImGui::EndCombo ();
        }
        ImGui::Separator ();
        ImGui::TextUnformatted ("Source folder:");
        ImGui::TextDisabled ("%s", batch_source_.empty () ? "(none chosen)" : batch_source_.c_str ());
        if (ImGui::Button ("Choose…##src") && begin_file_dialog ())
        {
            pending_dialog_ = DialogKind::BatchSource;
            SDL_ShowOpenFolderDialog (folder_dialog_done, this, NULL, NULL, false);
        }
        ImGui::Separator ();
        ImGui::TextUnformatted ("Destination folder:");
        ImGui::TextDisabled ("%s", batch_dest_.empty ()
                                       ? "(none: save over Photoshop files, others as .psd beside them)"
                                       : batch_dest_.c_str ());
        if (ImGui::Button ("Choose…##dst") && begin_file_dialog ())
        {
            pending_dialog_ = DialogKind::BatchDest;
            SDL_ShowOpenFolderDialog (folder_dialog_done, this, NULL, NULL, false);
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Clear"))
        {
            batch_dest_.clear ();
        }
        const char* formats[] = {"psd", "png", "jpg", "tif", "webp", "bmp", "gif", "tga"};
        ImGui::SetNextItemWidth (120 * k);
        ImGui::Combo ("Save As", &batch_format_, "Photoshop (.psd)\0PNG\0JPEG\0TIFF\0WebP\0BMP\0GIF\0Targa\0");
        ImGui::Spacing ();
        bool can = !batch_source_.empty () && nacts > 0;
        if (!can)
        {
            ImGui::BeginDisabled ();
        }
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0));
        if (!can)
        {
            ImGui::EndDisabled ();
        }
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            engine::Value p = {{"set", batch_set_}, {"action", batch_action_}, {"source", batch_source_},
                {"format", formats[batch_format_]}};
            if (!batch_dest_.empty ())
            {
                p["destination"] = batch_dest_;
            }
            engine::Value r;
            if (session_.execute ("file.automate.batch", p, &r, &e))
            {
                char msg[256];
                snprintf (
                    msg, sizeof msg, "Batch: %d of %d files done", r["processed"].get<int> (), r["files"].get<int> ());
                status_          = msg;
                status_is_error_ = !r["errors"].empty ();
                for (size_t i = 0; i < r["errors"].size () && i < 3; i++)
                {
                    status_ += " (" + r["errors"][i].get<std::string> () + ")";
                }
            }
            else
            {
                set_error (e.message);
            }
        }
        if (ok || cancel)
        {
            show_batch_ = false;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

} // namespace ui
