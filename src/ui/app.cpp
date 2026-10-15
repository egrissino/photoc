#include "ui/app.h"

#include "codecs/codecs.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "compose/masks.h"
#include "engine/adjust.h"
#include "engine/brush.h"
#include "io/io.h"
#include <GL/gl.h>
#include <ctype.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace ui
{

    /* The editor shell. It owns no document state: every action is an engine command, run through
       run_command(), and every panel reads the session. Open and Save are UI commands (they need
       dialogs) that go through io. The canvas is the CPU composite uploaded to an OpenGL texture
       whenever the document's revision moves. */

    const char* const MENU_ORDER[] = {
        "File", "Edit", "Image", "Layer", "Select", "Filter", "View", "Window", "Help", NULL};

    struct UiCommand
    {
        const char* id;
        const char* label;
        const char* shortcut;
        const char* menu;
    };

    /* View and file commands live in the shell: they need dialogs or touch view state only. */
    const UiCommand UI_COMMANDS[] = {
        {"file.open", "Open…", "Cmd+O", "File"},
        {"file.save", "Save", "Cmd+S", "File"},
        {"file.saveAs", "Save As…", "Cmd+Shift+S", "File"},
        {"edit.freeTransform", "Free Transform", "Cmd+T", "Edit"},
        {"edit.transform.scale", "Scale", "", "Edit/Transform"},
        {"edit.transform.rotate", "Rotate", "", "Edit/Transform"},
        {"edit.transform.skew", "Skew", "", "Edit/Transform"},
        {"edit.transform.distort", "Distort", "", "Edit/Transform"},
        {"edit.transform.perspective", "Perspective", "", "Edit/Transform"},
        {"edit.transform.warp", "Warp", "", "Edit/Transform"},
        {"layer.layerStyle.blendingOptions", "Blending Options…", "", "Layer/Layer Style"},
        {"filter.lastFilter", "Last Filter", "Cmd+Alt+F", "Filter"},
        {"select.quickMask", "Edit in Quick Mask Mode", "Q", "Select"},
        {"view.zoomIn", "Zoom In", "Cmd+=", ""},
        {"view.zoomOut", "Zoom Out", "Cmd+-", ""},
        {"view.fitOnScreen", "Fit on Screen", "Cmd+0", ""},
        {"view.actualPixels", "100%", "Cmd+1", ""},
    };
    const size_t UI_COMMAND_COUNT = sizeof UI_COMMANDS / sizeof UI_COMMANDS[0];

    /* The dialog filter arrays must outlive the asynchronous dialogs: constant data. */
    const SDL_DialogFileFilter OPEN_FILTERS[] = {
        {"Images",
            "psd;psb;png;jpg;jpeg;jpe;jfif;tif;tiff;webp;gif;bmp;dib;tga;ico;qoi;hdr;pcx;ppm;pgm;pbm;pnm;pam;pfm"},
        {"All files", "*"},
    };
    const SDL_DialogFileFilter SAVE_FILTERS[] = {
        {"Photoshop", "psd"},
        {"PNG", "png"},
        {"JPEG", "jpg;jpeg"},
        {"TIFF", "tif;tiff"},
        {"WebP", "webp"},
        {"GIF", "gif"},
        {"BMP", "bmp"},
        {"Targa", "tga"},
        {"ICO", "ico"},
        {"QOI", "qoi"},
        {"Radiance HDR", "hdr"},
        {"PCX", "pcx"},
        {"Netpbm", "ppm;pgm;pam;pfm"},
    };
    const char* const SAVE_FILTER_EXT[] = {
        ".psd", ".png", ".jpg", ".tif", ".webp", ".gif", ".bmp", ".tga", ".ico", ".qoi", ".hdr", ".pcx", ".ppm"};

    /* Preferences kept in the layout file: [PhotoC][Prefs] (theme, status readout) and
       [PhotoC][Swatches] (the swatch library). */
    void* ini_open (ImGuiContext* ctx, ImGuiSettingsHandler* handler, const char* name)
    {
        App* app = (App*) handler->UserData;
        if (strcmp (name, "Prefs") == 0)
        {
            return app;
        }
        if (strcmp (name, "Swatches") == 0)
        {
            return app->begin_swatch_settings ();
        }
        return NULL;
    }

    void ini_line (ImGuiContext* ctx, ImGuiSettingsHandler* handler, void* entry, const char* line)
    {
        App* app = (App*) handler->UserData;
        app->settings_line (entry == app ? "Prefs" : "Swatches", line);
    }

    void ini_write (ImGuiContext* ctx, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf)
    {
        App* app = (App*) handler->UserData;
        app->write_settings (handler->TypeName, buf);
    }

    /* A saved library replaces the default one; the returned marker tells its lines apart. */
    void* App::begin_swatch_settings ()
    {
        session_.swatches.clear ();
        return &swatch_section_;
    }

    void App::settings_line (const char* section, const char* line)
    {
        char value[64];
        ThemeKind kind;
        if (strcmp (section, "Prefs") == 0)
        {
            if (sscanf (line, "Theme=%63s", value) == 1 && theme_from_id (value, &kind))
            {
                set_theme (kind);
            }
            if (sscanf (line, "StatusInfo=%63s", value) == 1)
            {
                status_info_ = value;
            }
            unsigned flags;
            if (sscanf (line, "View=%x", &flags) == 1)
            {
                bool* f[13] = {&view_rulers_, &view_extras_, &view_guides_, &view_grid_, &view_sel_edges_,
                    &view_layer_edges_, &view_pixel_grid_, &view_snap_, &snap_guides_, &snap_grid_, &snap_layers_,
                    &snap_bounds_, &view_lock_guides_};
                for (int i = 0; i < 13; i++)
                {
                    *f[i] = (flags >> i) & 1u;
                }
            }
            float spacing;
            int subs;
            if (sscanf (line, "Grid=%f %d", &spacing, &subs) == 2)
            {
                grid_spacing_      = fminf (fmaxf (spacing, 1.0f), 10000.0f);
                grid_subdivisions_ = subs < 1 ? 1 : subs > 100 ? 100 : subs;
            }
            return;
        }
        if (strncmp (line, "Group=", 6) == 0)
        {
            engine::SwatchGroup g;
            g.name = line + 6;
            session_.swatches.push_back (g);
            return;
        }
        unsigned r, g, b;
        int used = 0;
        if (strncmp (line, "Swatch=#", 8) == 0 && sscanf (line + 8, "%02x%02x%02x %n", &r, &g, &b, &used) == 3
            && used > 0)
        {
            if (session_.swatches.empty ())
            {
                engine::SwatchGroup grp;
                grp.name = "Swatches";
                session_.swatches.push_back (grp);
            }
            engine::Swatch sw;
            sw.name   = line + 8 + used;
            sw.rgb[0] = (float) r / 255.0f;
            sw.rgb[1] = (float) g / 255.0f;
            sw.rgb[2] = (float) b / 255.0f;
            session_.swatches.back ().items.push_back (sw);
        }
    }

    void App::write_settings (const char* type, ImGuiTextBuffer* buf)
    {
        bool f[13]     = {view_rulers_, view_extras_, view_guides_, view_grid_, view_sel_edges_, view_layer_edges_,
                view_pixel_grid_, view_snap_, snap_guides_, snap_grid_, snap_layers_, snap_bounds_, view_lock_guides_};
        unsigned flags = 0;
        for (int i = 0; i < 13; i++)
        {
            flags |= (f[i] ? 1u : 0u) << i;
        }
        buf->appendf ("[%s][Prefs]\nTheme=%s\nStatusInfo=%s\nView=%x\nGrid=%g %d\n\n", type, theme_id (theme_kind_),
            status_info_.c_str (), flags, grid_spacing_, grid_subdivisions_);
        buf->appendf ("[%s][Swatches]\n", type);
        for (size_t g = 0; g < session_.swatches.size (); g++)
        {
            buf->appendf ("Group=%s\n", session_.swatches[g].name.c_str ());
            for (size_t i = 0; i < session_.swatches[g].items.size (); i++)
            {
                const engine::Swatch& sw = session_.swatches[g].items[i];
                buf->appendf ("Swatch=#%02x%02x%02x %s\n", (int) lroundf (sw.rgb[0] * 255.0f),
                    (int) lroundf (sw.rgb[1] * 255.0f), (int) lroundf (sw.rgb[2] * 255.0f), sw.name.c_str ());
            }
        }
        buf->append ("\n");
    }

    App::App (engine::Session& session, SDL_Window* window)
        : session_ (session), window_ (window), quit_ (false), status_is_error_ (false), texture_ (0), texture_doc_ (0),
          texture_revision_ (0), texture_width_ (0), texture_height_ (0), selection_built_ (false),
          selection_preview_serial_ (0), sel_edge_w_ (0), sel_edge_h_ (0), selection_doc_ (0), selection_revision_ (0),
          selection_visible_ (false), zoom_ (0.0f), opacity_editing_ (false), opacity_edit_ (100.0f), pan_x_ (0.0f),
          pan_y_ (0.0f), canvas_center_x_ (0.0f), canvas_center_y_ (0.0f), canvas_avail_x_ (0.0f),
          canvas_avail_y_ (0.0f), panning_ (false), stroking_ (false), stroke_mask_ (false),
          stroke_layer_ (doc::NO_LAYER), stroke_lock_ (false), stroke_seed_ (0), stroke_revision_ (0),
          stroke_counter_ (0), moving_ (false), move_x0_ (0.0f), move_y0_ (0.0f), move_dx_ (0), move_dy_ (0),
          brush_editing_ (false), pencil_auto_erase_ (false), eraser_mode_ (0), mask_target_layer_ (doc::NO_LAYER),
          rename_layer_ (doc::NO_LAYER), rename_focus_ (false), tool_ (Tool::Brush), brush_tool_ (Tool::Brush),
          dragging_ (false), drag_x0_ (0), drag_y0_ (0), drag_x1_ (0), drag_y1_ (0), reset_layout_ (false),
          preview_serial_ (0), texture_preview_serial_ (0), show_adjust_dialog_ (false), adjust_dialog_preview_ (true),
          props_layer_ (doc::NO_LAYER), props_revision_ (0), props_editing_ (false), mask_editing_ (false),
          mask_density_ (100.0f), mask_feather_ (0.0f), show_new_dialog_ (false), new_width_ (1920), new_height_ (1080),
          new_background_ (0)
    {
        snprintf (new_name_, sizeof new_name_, "Untitled");
        /* The look: Photoshop's default brightness unless the layout file says otherwise (read on the
           first frame, through the settings handler below). */
        ui_scale_ = SDL_GetWindowDisplayScale (window);
        if (!(ui_scale_ > 0.5f && ui_scale_ < 4.0f))
        {
            ui_scale_ = 1.0f;
        }
        load_fonts (&fonts_);
        theme_kind_  = ThemeKind::ProMedium;
        theme_dirty_ = true;
        ImGuiSettingsHandler handler;
        handler.TypeName   = "PhotoC";
        handler.TypeHash   = ImHashStr ("PhotoC");
        handler.ReadOpenFn = ini_open;
        handler.ReadLineFn = ini_line;
        handler.WriteAllFn = ini_write;
        handler.UserData   = this;
        ImGui::AddSettingsHandler (&handler);
        rename_buf_[0] = 0;
        tab_active_    = (size_t) -1;
        xform_editing_ = false;
        for (int i = 0; i < 4; i++)
        {
            channel_shown_[i] = true;
            channel_tex_[i]   = 0;
        }
        channel_view_dirty_ = false;
        alpha_view_         = -1;
        alpha_overlay_      = false;
        alpha_tex_doc_      = 0;
        alpha_tex_revision_ = 0;
        rename_channel_     = -1;
        picker_open_        = false;
        picker_bg_          = false;
        show_fill_          = false;
        fill_contents_      = 0;
        fill_color_[0] = fill_color_[1] = fill_color_[2] = 0.0f;
        fill_mode_                                       = 0;
        fill_dialog_opacity_                             = 100.0f;
        fill_preserve_                                   = false;
        show_stroke_                                     = false;
        stroke_width_                                    = 1;
        stroke_location_                                 = 1;
        stroke_mode_                                     = 0;
        stroke_opacity_                                  = 100.0f;
        stroke_preserve_                                 = false;
        show_save_selection_                             = false;
        save_channel_                                    = 0;
        save_operation_                                  = 0;
        channel_name_[0]                                 = 0;
        show_load_selection_                             = false;
        load_channel_                                    = 0;
        load_invert_                                     = false;
        load_operation_                                  = 0;
        show_filter_                                     = false;
        show_fade_                                       = false;
        fade_opacity_                                    = 100.0f;
        fade_mode_                                       = 0;
        info_over_                                       = false;
        info_x_ = info_y_ = 0;
        info_doc_         = 0;
        info_revision_    = 0;
        info_px_[0] = info_px_[1] = info_px_[2] = info_px_[3] = 0.0f;
        filter_dialog_                                        = -1;
        filter_mode_                                          = FILTER_RUN;
        actions_loaded_                                       = false;
        actions_hash_                                         = 0;
        act_sel_[0] = act_sel_[1] = act_sel_[2] = -1;
        act_ref_[0] = act_ref_[1] = act_ref_[2] = -1;
        act_naming_                             = 0;
        act_name_[0]                            = 0;
        show_batch_                             = false;
        batch_set_ = batch_action_ = batch_format_ = 0;
        filter_edit_layer_                         = doc::NO_LAYER;
        filter_edit_index_                         = -1;
        blend_edit_layer_                          = doc::NO_LAYER;
        blend_edit_index_                          = -1;
        blend_edit_mode_                           = color::BlendMode::Normal;
        blend_edit_opacity_                        = 100.0f;
        filter_preview_                            = true;
        filter_preview_dirty_                      = false;
        small_w_ = small_h_ = 0;
        small_serial_       = 0;
        channel_tex_serial_ = (uint64_t) -1;
        color_bg_active_    = false;
        color_hue_          = 0.0f;
        color_sat_          = 0.0f;
        swatch_buf_[0]      = 0;
        status_info_        = "dimensions";
        fill_editing_       = false;
        fill_edit_          = 100.0f;
        fit_view_           = true;
        fit_avail_x_ = fit_avail_y_ = 0.0f;
        close_request_              = -1;
        pending_dialog_             = DialogKind::Open;
        memset (&xf_, 0, sizeof xf_);
        pw_.active    = false;
        pw_.drag      = -1;
        pw_.mode      = 1;
        pw_.show_mesh = true;
        memset (&vw_, 0, sizeof vw_);
        memset (&apply_image_, 0, sizeof apply_image_);
        sam_.active          = false;
        sam_.used            = false;
        sam_.remember        = false;
        sam_.doc             = 0;
        sam_.history_at      = 0;
        sam_.seen_revision   = 0;
        sam_.seen_serial     = 0;
        sam_.saved_tool      = Tool::Move;
        sam_.tool            = 0;
        sam_.refine_size     = 40.0f;
        sam_.refine_subtract = false;
        sam_.brush_size      = 30.0f;
        sam_.brush_subtract  = false;
        sam_.painting        = false;
        sam_.paint_subtract  = false;
        sam_.dirty           = false;
        sam_.editing         = false;
        sam_.show_edge = sam_.show_original = sam_.hide_view = false;
        sam_.strokes                                         = engine::Value::array ();
        file_dialogs_open_                                   = 0;
        show_color_table_                                    = false;
        fill_layer_ed_.open                                  = false;
        fill_layer_ed_.fresh                                 = false;
        fill_layer_ed_.layer                                 = 0;
        fill_layer_ed_.stop                                  = 0;
        fill_layer_ed_.drag                                  = -1;
        fill_layer_ed_.pattern_index                         = 0;
        fill_layer_ed_.dirty                                 = false;
        bi_drag_bar_                                         = -1;
        bi_drag_handle_                                      = -1;
        bi_drag_split_                                       = false;
        bi_drag_from_                                        = 0.0f;
        memset (bi_drag_start_, 0, sizeof bi_drag_start_);
        bi_channel_    = 0;
        style_blend_   = color::BlendMode::Normal;
        style_opacity_ = style_fill_ = 1.0f;
        style_channels_[0] = style_channels_[1] = style_channels_[2] = true;
        style_blend_if_                                              = doc::BlendIf::make ();
        ct_transparent_                                              = -1;
        ct_edit_                                                     = -1;
        quick_sent_                                                  = 0;
        quick_mode_now_                                              = "replace";
        file_dialog_since_                                           = 0.0;
        time_next_render_                                            = false;
        lasso_tool_                                                  = Tool::Lasso;
        for (int i = 0; i < SLOT_COUNT; i++)
        {
            slot_last_[i] = Tool::Move;
        }
        slot_last_[1]      = Tool::RectMarquee;
        slot_last_[2]      = Tool::Lasso;
        slot_last_[3]      = Tool::QuickSelection;
        quick_size_        = 30.0f;
        quick_active_      = false;
        quick_all_layers_  = false;
        slot_last_[4]      = Tool::Crop;
        slot_last_[5]      = Tool::Eyedropper;
        slot_last_[6]      = Tool::SpotHealing;
        slot_last_[7]      = Tool::Brush;
        slot_last_[8]      = Tool::CloneStamp;
        slot_last_[9]      = Tool::HistoryBrush;
        slot_last_[10]     = Tool::Eraser;
        slot_last_[11]     = Tool::Gradient;
        slot_last_[12]     = Tool::Blur;
        slot_last_[13]     = Tool::Dodge;
        slot_last_[14]     = Tool::Pen;
        slot_last_[15]     = Tool::Type;
        slot_last_[16]     = Tool::PathSelect;
        slot_last_[17]     = Tool::RectShape;
        slot_last_[18]     = Tool::Hand;
        slot_last_[19]     = Tool::Zoom;
        path_sel_          = -3;
        path_hidden_layer_ = doc::NO_LAYER;
        pen_mode_          = 0;
        shape_mode_        = 0;
        path_op_           = 0;
        pen_path_          = doc::Path::make ();
        pen_has_target_    = false;
        pen_doc_           = 0;
        pen_revision_      = 0;
        pen_sub_           = -1;
        pen_drag_          = 0;
        pen_drag_tool_     = Tool::Pen;
        pen_drag_sub_      = 0;
        pen_drag_knot_     = 0;
        pen_press_[0] = pen_press_[1] = pen_last_[0] = pen_last_[1] = 0.0f;
        pen_moved_                                                  = false;
        shape_dragging_                                             = false;
        shape_from_[0] = shape_from_[1] = shape_to_[0] = shape_to_[1] = 0.0f;
        shape_shift_                                                  = false;
        shape_alt_                                                    = false;
        shape_fill_[0] = shape_fill_[1] = shape_fill_[2] = 0.0f;
        shape_fill_on_                                   = true;
        shape_stroke_[0] = shape_stroke_[1] = shape_stroke_[2] = 0.0f;
        shape_stroke_on_                                       = false;
        shape_stroke_w_                                        = 3.0f;
        shape_radius_                                          = 0.0f;
        shape_sides_                                           = 5;
        shape_star_                                            = 0.0f;
        shape_weight_                                          = 1.0f;
        shape_editing_                                         = false;
        shape_dirty_                                           = false;
        shape_props_editing_                                   = false;
        memset (shape_props_, 0, sizeof shape_props_);
        vmask_editing_    = false;
        vmask_props_[0]   = 100.0f;
        vmask_props_[1]   = 0.0f;
        rename_path_      = -1;
        eyedrop_size_     = 0;
        eyedrop_layers_   = 2;
        eyedropping_      = false;
        eyedrop_bg_       = false;
        picker_focus_     = false;
        quick_mask_       = false;
        type_active_      = false;
        view_rulers_      = false;
        view_extras_      = true;
        view_guides_      = true;
        view_grid_        = false;
        view_sel_edges_   = true;
        view_layer_edges_ = false;
        view_pixel_grid_  = true;
        view_snap_        = true;
        snap_guides_ = snap_grid_ = snap_layers_ = snap_bounds_ = true;
        view_lock_guides_                                       = false;
        view_proof_                                             = false;
        view_gamut_                                             = false;
        screen_mode_                                            = 0;
        panels_hidden_                                          = false;
        redock_canvas_                                          = false;
        grid_spacing_                                           = 100.0f;
        grid_subdivisions_                                      = 4;
        guide_drag_                                             = -1;
        move_has_box_                                           = false;
        memset (move_box_, 0, sizeof move_box_);
        guide_drag_vertical_   = true;
        guide_drag_pos_        = 0.0f;
        ruler_px_              = 0.0f;
        snap_skip_layer_       = doc::NO_LAYER;
        main_dock_             = 0;
        show_new_guide_        = false;
        show_guide_layout_     = false;
        show_grid_options_     = false;
        guide_dialog_vertical_ = 0;
        guide_dialog_pos_      = 0.0f;
        layout_cols_           = 4;
        layout_rows_           = 0;
        layout_col_gutter_     = 20.0f;
        layout_row_gutter_     = 20.0f;
        memset (layout_margin_, 0, sizeof layout_margin_);
        layout_clear_  = false;
        type_new_      = false;
        type_layer_    = doc::NO_LAYER;
        type_doc_      = 0;
        type_text_     = doc::TextLayer::make ();
        type_original_ = type_text_;
        type_caret_ = type_anchor_ = 0;
        type_selecting_            = false;
        type_handle_               = -1;
        type_blink_                = 0.0;
        type_dragging_             = false;
        memset (type_drag_, 0, sizeof type_drag_);
        type_style_      = doc::CharStyle::make ();
        type_para_       = doc::ParaStyle::make ();
        type_aa_         = doc::AntiAlias::Smooth;
        type_filter_[0]  = 0;
        type_drag_id_    = 0;
        type_drag_value_ = 0.0f;
        register_type_fonts ();
        show_style_        = false;
        style_layer_       = doc::NO_LAYER;
        style_sel_         = -1;
        style_preview_     = true;
        style_dirty_       = false;
        style_light_[0]    = 120.0f;
        style_light_[1]    = 30.0f;
        stroke_quick_      = false;
        selection_mode_    = 0;
        lasso_active_      = false;
        sel_feather_       = 0.0f;
        sel_anti_alias_    = true;
        wand_tolerance_    = 32;
        wand_contiguous_   = true;
        wand_all_layers_   = false;
        modify_radius_     = 1.0f;
        modify_at_bounds_  = false;
        show_color_range_  = false;
        range_select_      = 0;
        range_fuzziness_   = 40.0f;
        range_invert_      = false;
        fill_tool_         = Tool::Gradient;
        gradient_dragging_ = false;
        gradient_style_    = 0;
        gradient_reverse_  = false;
        gradient_dither_   = true;
        fill_opacity_      = 100.0f;
        bucket_tolerance_  = 32;
        bucket_contiguous_ = true;
        bucket_all_layers_ = false;
        crop_active_       = false;
        crop_drag_         = -1;
        crop_delete_       = true;
        show_image_size_   = false;
        size_constrain_    = true;
        size_method_       = 0;
        show_canvas_size_  = false;
        canvas_relative_   = false;
        canvas_anchor_     = 4;
        canvas_extension_  = 0;
        heal_tool_         = Tool::SpotHealing;
        tone_tool_         = Tool::Dodge;
        retouching_        = false;
        has_clone_source_  = false;
        has_clone_offset_  = false;
        clone_aligned_     = true;
        clone_sample_      = 0;
        tone_range_        = 1;
        tone_exposure_     = 50.0f;
        tone_protect_      = true;
        sponge_saturate_   = false;
        sponge_vibrance_   = true;
        focus_strength_    = 50.0f;
        focus_all_layers_  = false;
        smudge_finger_     = false;
        spot_all_layers_   = false;
        spot_type_         = 0;
        remove_all_layers_ = true;
        sample_mode_       = 0;
        limits_mode_       = 0;
        replace_mode_      = 2;
        bg_tolerance_      = 50.0f;
        cr_tolerance_      = 30.0f;
        bg_protect_        = false;
        mixer_wet_         = 50.0f;
        mixer_load_        = 50.0f;
        mixer_mix_         = 50.0f;
        mixer_loaded_      = true;
        mixer_all_layers_  = false;
        pattern_index_     = 0;
        pattern_aligned_   = true;
        art_style_         = 0;
        art_area_          = 50.0f;
        art_tolerance_     = 0.0f;
        redeye_pupil_      = 50.0f;
        redeye_darken_     = 50.0f;
        patch_dest_        = false;
        cam_extend_        = false;
        heal_drag_         = 0;
        heal_press_[0] = heal_press_[1] = heal_last_[0] = heal_last_[1] = 0.0f;
        fill_pattern_                                                   = 0;
        xf_.drag                                                        = TransformDrag::None;
        clip_png_ready_                                                 = false;
        os_clip_new_                                                    = true;
        clip_external_                                                  = false;
    }

    bool is_paint_tool (Tool t)
    {
        return t == Tool::Brush || t == Tool::Pencil || t == Tool::Eraser;
    }

    /* Tools that paint with the brush's size and hardness: the paint tools and the retouching tools. */
    bool uses_brush (Tool t)
    {
        return is_paint_tool (t) || (t >= Tool::CloneStamp && t <= Tool::Smudge)
            || (t >= Tool::ColorReplacement && t <= Tool::Remove);
    }

    void App::set_theme (ThemeKind kind)
    {
        theme_kind_  = kind;
        theme_dirty_ = true;
        ImGui::MarkIniSettingsDirty ();
    }

    ThemeKind App::theme_kind () const
    {
        return theme_kind_;
    }

    App::~App ()
    {
        /* The layout is saved now, while the settings handler's App still exists; ImGui's own save
           when its context is destroyed would call into a deleted App. */
        ImGuiIO& io = ImGui::GetIO ();
        if (io.IniFilename != NULL)
        {
            ImGui::SaveIniSettingsToDisk (io.IniFilename);
        }
        io.IniFilename = NULL;
        ImGui::RemoveSettingsHandler ("PhotoC");
        for (size_t i = 0; i < thumbs_.size (); i++)
        {
            glDeleteTextures (1, &thumbs_[i].tex);
        }
        for (int i = 0; i < 4; i++)
        {
            if (channel_tex_[i] != 0)
            {
                glDeleteTextures (1, &channel_tex_[i]);
            }
        }
        if (texture_ != 0)
        {
            glDeleteTextures (1, &texture_);
        }
    }

    bool App::quit_requested () const
    {
        return quit_;
    }

    /* A file dialog has been asked for and hasn't answered: the window idles meanwhile. */
    bool App::waiting_on_dialog () const
    {
        return file_dialogs_open_ > 0;
    }

    void App::set_error (const std::string& message)
    {
        status_          = message;
        status_is_error_ = true;
    }

    const char* const CLIP_MIME[] = {"image/png"};

    const void* clipboard_callback (void* userdata, const char* mime_type, size_t* size)
    {
        return ((App*) userdata)->clipboard_data (mime_type, size);
    }

    /* After an in-app Copy: other apps can paste it as PNG, encoded only when one asks. */
    void App::publish_clipboard ()
    {
        clip_png_.clear ();
        clip_png_ready_ = false;
        clip_external_  = false;
        os_clip_new_    = false;
        SDL_SetClipboardData (clipboard_callback, NULL, this, CLIP_MIME, 1);
    }

    const void* App::clipboard_data (const char* mime, size_t* size)
    {
        *size = 0;
        if (mime == NULL || strcmp (mime, "image/png") != 0 || !session_.clipboard.full)
        {
            return NULL;
        }
        if (!clip_png_ready_)
        {
            clip_png_ready_               = true;
            const engine::Clipboard& clip = session_.clipboard;
            geom::Rect b                  = clip.bounds;
            color::PixelFormat fmt        = clip.surface.format ();
            geom::Size dims               = {(uint32_t) b.width (), (uint32_t) b.height ()};
            doc::Document d               = doc::Document::make (session_.ids, "clipboard", dims, fmt.mode, fmt.sample);
            doc::Layer l                  = doc::Layer::raster (session_.ids, "Layer 1", fmt);
            l.pixels                      = clip.surface.translated (-b.x0, -b.y0, b);
            d.layers.push_back (l);
            compose::RenderSettings settings = compose::default_settings ();
            settings.cmyk                    = session_.cmyk;
            std::vector<std::string> warnings;
            char err[256];
            if (!io::export_bytes (d, codecs::Format::Png, codecs::EncodeOptions::defaults (), settings, &clip_png_,
                    &warnings, err, sizeof err))
            {
                clip_png_.clear ();
            }
        }
        *size = clip_png_.size ();
        return clip_png_.empty () ? NULL : clip_png_.data ();
    }

    /* SDL reports every clipboard change; one we didn't make may hold an image to paste. */
    void App::clipboard_changed (bool owner)
    {
        if (!owner)
        {
            os_clip_new_ = true;
        }
    }

    /* An image another app copied since our last Copy becomes the session clipboard. */
    void App::import_os_clipboard ()
    {
        if (!os_clip_new_)
        {
            return;
        }
        os_clip_new_ = false;
        if (!SDL_HasClipboardData ("image/png"))
        {
            return;
        }
        size_t len = 0;
        void* data = SDL_GetClipboardData ("image/png", &len);
        if (data == NULL)
        {
            return;
        }
        io::ImportResult imported;
        char err[256];
        bool ok = io::import_bytes (session_.ids, "clipboard", (const uint8_t*) data, len, &imported, err, sizeof err);
        SDL_free (data);
        if (!ok)
        {
            set_error (std::string ("could not read the clipboard image: ") + err);
            return;
        }
        const doc::Document& d           = imported.document;
        color::PixelFormat fmt           = d.pixel_format ();
        fmt.alpha                        = true;
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = session_.cmyk;
        session_.clipboard.surface       = compose::composite_to_surface (d, d.bounds (), fmt, NULL, settings);
        session_.clipboard.bounds        = d.bounds ();
        session_.clipboard.full          = true;
        clip_external_                   = true;
    }

    /* Photoshop pastes in place while the copied area is in view, else in the middle of the view;
       images from other apps always go in the middle. */
    engine::Value App::paste_params ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || zoom_ <= 0.0f || !session_.clipboard.full)
        {
            return engine::Value::object ();
        }
        double cx = d->doc->size.width / 2.0 - pan_x_ / zoom_, cy = d->doc->size.height / 2.0 - pan_y_ / zoom_;
        double hw = canvas_avail_x_ / 2.0 / zoom_, hh = canvas_avail_y_ / 2.0 / zoom_;
        geom::Rect view = {(int32_t) (cx - hw), (int32_t) (cy - hh), (int32_t) (cx + hw), (int32_t) (cy + hh)};
        bool visible    = !clip_external_ && !session_.clipboard.bounds.intersect (view).is_empty ();
        if (visible)
        {
            return engine::Value::object ();
        }
        return engine::Value{{"center", {cx, cy}}};
    }

    void open_dialog_done (void* userdata, const char* const* files, int filter);
    void save_dialog_done (void* userdata, const char* const* files, int filter);

    void App::run_command (const char* id, const engine::Value& params)
    {
        /* Another command while type is being edited commits the type first. */
        if (type_active_ && strncmp (id, "view.", 5) != 0)
        {
            commit_type_edit ();
        }
        /* Select > Modify and Color Range from the menu ask for their settings first. */
        if ((params.is_null () || (params.is_object () && params.empty ())) && session_.active () != NULL)
        {
            if (strncmp (id, "select.modify.", 14) == 0)
            {
                modify_command_ = id;
                return;
            }
            if (strcmp (id, "select.colorRange") == 0)
            {
                show_color_range_ = true;
                return;
            }
            if (strcmp (id, "file.automate.batch") == 0)
            {
                show_batch_ = true;
                return;
            }
            /* Place Embedded, Replace and Export Contents ask for the file first. */
            if (strcmp (id, "file.placeEmbedded") == 0 || strcmp (id, "layer.smartObject.replaceContents") == 0)
            {
                if (!begin_file_dialog ())
                {
                    return;
                }
                pending_dialog_ = strcmp (id, "file.placeEmbedded") == 0 ? DialogKind::Place : DialogKind::Replace;
                SDL_ShowOpenFileDialog (open_dialog_done, this, NULL, OPEN_FILTERS, 2, NULL, false);
                return;
            }
            if (strcmp (id, "layer.smartObject.exportContents") == 0)
            {
                engine::Value info;
                engine::Error e;
                std::string name = "contents.psd";
                if (session_.execute ("layer.smartObject.info", engine::Value::object (), &info, &e)
                    && info.contains ("filename"))
                {
                    name = info["filename"].get<std::string> ();
                }
                if (!begin_file_dialog ())
                {
                    return;
                }
                pending_dialog_ = DialogKind::Export;
                SDL_ShowSaveFileDialog (save_dialog_done, this, NULL, NULL, 0, name.c_str ());
                return;
            }
            if (strcmp (id, "edit.fill") == 0)
            {
                show_fill_ = true;
                memcpy (fill_color_, session_.tools.foreground, sizeof fill_color_);
                return;
            }
            if (strcmp (id, "edit.fade") == 0)
            {
                std::string why;
                const engine::CommandSpec* spec = session_.find_command (id);
                if (spec != NULL && !session_.command_enabled (*spec, &why))
                {
                    set_error (why);
                    return;
                }
                show_fade_    = true;
                fade_opacity_ = 100.0f;
                return;
            }
            if (strcmp (id, "edit.stroke") == 0)
            {
                show_stroke_ = true;
                memcpy (stroke_color_, session_.tools.foreground, sizeof stroke_color_);
                return;
            }
            if (strcmp (id, "select.save") == 0)
            {
                show_save_selection_ = true;
                save_channel_        = 0;
                channel_name_[0]     = 0;
                return;
            }
            if (strcmp (id, "select.load") == 0)
            {
                show_load_selection_ = true;
                return;
            }
            doc::FxKind fx_kind;
            if (strncmp (id, "layer.layerStyle.", 17) == 0 && doc::fx_kind_from_id (id + 17, &fx_kind))
            {
                open_layer_style (&fx_kind, -1);
                return;
            }
            if ((strncmp (id, "filter.", 7) == 0 || strncmp (id, "image.", 6) == 0) && open_filter_dialog (id))
            {
                return;
            }
            /* A new fill layer is made at once, then its dialog opens on it. */
            if (strncmp (id, "layer.newFillLayer.", 19) == 0)
            {
                run_command (id, engine::Value{{"opacity", 100}});
                const engine::DocState* nd = session_.active ();
                if (nd != NULL)
                {
                    const doc::Layer* nl = nd->doc->layer (nd->active_layer);
                    if (nl != NULL && nl->kind == doc::LayerKind::Fill)
                    {
                        open_fill_editor (nl->id, true);
                    }
                }
                return;
            }
            if (strcmp (id, "layer.layerStyle.scaleEffects") == 0 && open_filter_dialog (id))
            {
                return;
            }
            if (strcmp (id, "layer.layerStyle.blendingOptions") == 0)
            {
                open_layer_style (NULL, -2);
                return;
            }
            if (strcmp (id, "image.mode.colorTable") == 0)
            {
                open_color_table ();
                return;
            }
            if (strcmp (id, "select.selectAndMask") == 0)
            {
                open_select_and_mask ();
                return;
            }
            if (strcmp (id, "image.applyImage") == 0 || strcmp (id, "image.calculations") == 0)
            {
                open_apply_image (strcmp (id, "image.calculations") == 0);
                return;
            }
            const doc::Document& cur = *session_.active ()->doc;
            if (strcmp (id, "image.imageSize") == 0)
            {
                show_image_size_ = true;
                size_width_      = (int) cur.size.width;
                size_height_     = (int) cur.size.height;
                size_resolution_ = cur.resolution_dpi;
                return;
            }
            if (strcmp (id, "image.canvasSize") == 0)
            {
                show_canvas_size_ = true;
                canvas_width_     = canvas_relative_ ? 0 : (int) cur.size.width;
                canvas_height_    = canvas_relative_ ? 0 : (int) cur.size.height;
                return;
            }
        }
        /* Image > Adjustments run without params (menu, shortcut) open their dialog, as in Photoshop. */
        const char* prefix = "image.adjustments.";
        doc::AdjustmentKind kind;
        bool no_params = params.is_null () || (params.is_object () && params.empty ());
        if (no_params && strncmp (id, prefix, strlen (prefix)) == 0
            && doc::adjustment_kind_from_id (id + strlen (prefix), &kind) && adjust_has_dialog (kind))
        {
            open_adjust_dialog (kind, id);
            return;
        }
        /* Another command while transforming applies the transform first (undo throws it away). */
        if (xf_.active && strncmp (id, "view.", 5) != 0 && strcmp (id, "edit.transform") != 0)
        {
            if (strcmp (id, "edit.undo") == 0 || strcmp (id, "edit.freeTransform") == 0)
            {
                cancel_free_transform ();
                if (strcmp (id, "edit.undo") == 0)
                {
                    return;
                }
            }
            else
            {
                commit_free_transform ();
            }
        }
        if (warp_mode_active () && strncmp (id, "view.", 5) != 0)
        {
            if (strcmp (id, "edit.undo") == 0)
            {
                cancel_warp_mode ();
                return;
            }
            commit_warp_mode ();
            if (vw_.active)
            {
                cancel_warp_mode ();
            }
        }
        /* Puppet Warp, Perspective Warp and Content-Aware Scale from the menu work on the canvas. */
        if (no_params && strcmp (id, "edit.puppetWarp") == 0)
        {
            begin_puppet_warp ();
            return;
        }
        if (no_params && strcmp (id, "edit.perspectiveWarp") == 0)
        {
            begin_perspective_warp ();
            return;
        }
        if (no_params && strcmp (id, "edit.contentAwareScale") == 0)
        {
            begin_transform_mode (XF_CAS);
            return;
        }
        for (size_t i = 0; i < UI_COMMAND_COUNT; i++)
        {
            if (strcmp (UI_COMMANDS[i].id, id) == 0)
            {
                run_ui_command (id, params);
                return;
            }
        }
        /* The View menu's switches and views (window state, not the document). */
        if (strncmp (id, "view.", 5) == 0 && view_command (id))
        {
            return;
        }
        /* Pastes take an image another app put on the system clipboard; Paste goes in place when the
           copied area is in view, else in the middle of the view. With the mask targeted, Copy and
           Paste use it. */
        engine::Value p = params;
        bool paste = strcmp (id, "edit.paste") == 0, in_place = strcmp (id, "edit.pasteSpecial.pasteInPlace") == 0;
        bool any_paste = paste || in_place || strncmp (id, "edit.pasteSpecial.", 18) == 0
                      || strcmp (id, "file.newFromClipboard") == 0;
        if (any_paste)
        {
            import_os_clipboard ();
        }
        if (no_params && session_.active () != NULL)
        {
            if (paste)
            {
                p = paste_params ();
            }
            if (mask_targeted () && (paste || in_place || strcmp (id, "edit.copy") == 0))
            {
                if (!p.is_object ())
                {
                    p = engine::Value::object ();
                }
                p["target"] = "mask";
            }
        }
        engine::Value result;
        engine::Error err;
        if (session_.execute (id, p, &result, &err))
        {
            if (strcmp (id, "edit.copy") == 0 || strcmp (id, "edit.copyMerged") == 0 || strcmp (id, "edit.cut") == 0)
            {
                publish_clipboard ();
            }
            const engine::CommandSpec* spec = session_.find_command (id);
            status_                         = spec != NULL ? spec->label : id;
            if (strncmp (id, "filter.", 7) == 0)
            {
                last_filter_        = id;
                last_filter_params_ = p.is_object () ? p : engine::Value::object ();
            }
            status_is_error_ = false;
        }
        else
        {
            set_error (err.message);
        }
    }

    /* SDL may call this from another thread: only queue the result; frame() handles it. */
    void open_dialog_done (void* userdata, const char* const* files, int filter)
    {
        DialogResult r;
        /* Open, Place Embedded and Replace Contents share the open dialog. */
        r.kind   = ((App*) userdata)->pending_open_kind ();
        r.filter = filter;
        if (files == NULL)
        {
            r.error = std::string ("the file dialog failed: ") + SDL_GetError ();
        }
        else if (files[0] != NULL)
        {
            r.path = files[0];
        }
        ((App*) userdata)->dialog_finished (r);
    }

    void save_dialog_done (void* userdata, const char* const* files, int filter)
    {
        DialogResult r;
        r.kind   = ((App*) userdata)->pending_save_kind ();
        r.filter = filter;
        if (files == NULL)
        {
            r.error = std::string ("the file dialog failed: ") + SDL_GetError ();
        }
        else if (files[0] != NULL)
        {
            r.path = files[0];
        }
        ((App*) userdata)->dialog_finished (r);
    }

    DialogKind App::pending_open_kind () const
    {
        return pending_dialog_ == DialogKind::Place || pending_dialog_ == DialogKind::Replace ? pending_dialog_
                                                                                              : DialogKind::Open;
    }

    DialogKind App::pending_folder_kind () const
    {
        return pending_dialog_ == DialogKind::BatchDest ? DialogKind::BatchDest : DialogKind::BatchSource;
    }

    DialogKind App::pending_save_kind () const
    {
        return pending_dialog_ == DialogKind::Export ? DialogKind::Export : DialogKind::SaveAs;
    }

    /* A file dialog is about to open: false (with a message) while another is still open, since
       the dialogs no longer block the window. Its parent isn't given, so the desktop doesn't lock
       and dim the window behind it. */
    bool App::begin_file_dialog ()
    {
        if (file_dialogs_open_ > 0)
        {
            status_          = "A file dialog is already open";
            status_is_error_ = true;
            return false;
        }
        file_dialogs_open_++;
        file_dialog_since_ = engine::timing_now ();
        return true;
    }

    void App::dialog_finished (const DialogResult& result)
    {
        std::lock_guard<std::mutex> guard (dialog_lock_);
        dialog_results_.push_back (result);
    }

    void App::process_dialogs ()
    {
        std::vector<DialogResult> done;
        {
            std::lock_guard<std::mutex> guard (dialog_lock_);
            done.swap (dialog_results_);
        }
        for (size_t i = 0; i < done.size (); i++)
        {
            const DialogResult& r = done[i];
            if (file_dialogs_open_ > 0)
            {
                file_dialogs_open_--;
            }
            engine::timing_log (
                session_, "file dialog (open until closed)", engine::timing_now () - file_dialog_since_);
            if (!r.error.empty ())
            {
                set_error (r.error);
                continue;
            }
            if (r.path.empty ())
            {
                continue;
            }
            if (r.kind == DialogKind::Open)
            {
                open_file (r.path.c_str ());
                continue;
            }
            if (r.kind == DialogKind::BatchSource || r.kind == DialogKind::BatchDest)
            {
                (r.kind == DialogKind::BatchSource ? batch_source_ : batch_dest_) = r.path;
                continue;
            }
            if (r.kind == DialogKind::Place || r.kind == DialogKind::Replace || r.kind == DialogKind::Export)
            {
                const char* cmd = r.kind == DialogKind::Place   ? "file.placeEmbedded"
                                : r.kind == DialogKind::Replace ? "layer.smartObject.replaceContents"
                                                                : "layer.smartObject.exportContents";
                run_command (cmd, engine::Value{{"path", r.path}});
                continue;
            }
            /* A name typed without a known extension takes the one of the chosen filter. */
            std::string path = r.path;
            codecs::Format format;
            size_t plen  = path.size ();
            bool psd_ext = plen >= 4 && strcasecmp (path.c_str () + plen - 4, ".psd") == 0;
            if (!psd_ext && !codecs::from_extension (path.c_str (), &format))
            {
                int f = r.filter >= 0 && r.filter < (int) (sizeof SAVE_FILTER_EXT / sizeof SAVE_FILTER_EXT[0])
                          ? r.filter
                          : 0;
                path += SAVE_FILTER_EXT[f];
            }
            save_file (path.c_str ());
        }
    }

    /* Open, Save and Save As. Save writes back in place only to a layered format (PSD), as in the
       reference; anything else goes through Save As. A "path" param skips the dialog. */
    void App::run_ui_command (const char* id, const engine::Value& params)
    {
        if (strncmp (id, "view.", 5) == 0)
        {
            if (strcmp (id, "view.zoomIn") == 0)
            {
                zoom_by (1.25f, canvas_center_x_, canvas_center_y_);
            }
            else if (strcmp (id, "view.zoomOut") == 0)
            {
                zoom_by (0.8f, canvas_center_x_, canvas_center_y_);
            }
            else if (strcmp (id, "view.fitOnScreen") == 0)
            {
                zoom_ = 0.0f;
            }
            else if (strcmp (id, "view.actualPixels") == 0)
            {
                zoom_     = 1.0f;
                fit_view_ = false;
                pan_x_ = pan_y_ = 0.0f;
            }
            return;
        }
        if (strcmp (id, "edit.freeTransform") == 0)
        {
            begin_free_transform ();
            return;
        }
        const char* const XF_IDS[6] = {"edit.transform.scale", "edit.transform.rotate", "edit.transform.skew",
            "edit.transform.distort", "edit.transform.perspective", "edit.transform.warp"};
        for (int i = 0; i < 6; i++)
        {
            if (strcmp (id, XF_IDS[i]) == 0)
            {
                begin_transform_mode (XF_SCALE + i);
                return;
            }
        }
        if (strcmp (id, "select.quickMask") == 0)
        {
            toggle_quick_mask ();
            return;
        }
        if (strcmp (id, "filter.lastFilter") == 0)
        {
            if (last_filter_.empty ())
            {
                set_error ("no filter has been applied yet");
            }
            else if (session_.active () != NULL)
            {
                /* The same settings on the active layer, without the dialog (empty params would open it). */
                engine::Value p = last_filter_params_;
                p["layer"]      = session_.active ()->active_layer;
                run_command (last_filter_.c_str (), p);
            }
            return;
        }
        std::string path;
        bool has_path = engine::param_str (params, "path", &path);
        if (strcmp (id, "file.open") == 0)
        {
            if (has_path)
            {
                open_file (path.c_str ());
                return;
            }
            if (!begin_file_dialog ())
            {
                return;
            }
            pending_dialog_ = DialogKind::Open;
            SDL_ShowOpenFileDialog (open_dialog_done, this, NULL, OPEN_FILTERS, 2, NULL, false);
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            set_error ("no document open");
            return;
        }
        if (has_path)
        {
            save_file (path.c_str ());
            return;
        }
        /* A smart object's contents save back into their document. */
        if (strcmp (id, "file.save") == 0 && d->smart_parent != 0)
        {
            run_command ("layer.smartObject.commitContents", engine::Value::object ());
            return;
        }
        /* Save writes a PSD back in place. */
        size_t plen = d->path.size ();
        if (strcmp (id, "file.save") == 0 && plen >= 4 && strcasecmp (d->path.c_str () + plen - 4, ".psd") == 0)
        {
            std::string p = d->path;
            save_file (p.c_str ());
            return;
        }
        std::string suggested = d->path.empty () ? d->doc->name + ".psd" : d->path;
        if (!begin_file_dialog ())
        {
            return;
        }
        pending_dialog_ = DialogKind::SaveAs;
        SDL_ShowSaveFileDialog (save_dialog_done, this, NULL, SAVE_FILTERS,
            sizeof SAVE_FILTERS / sizeof SAVE_FILTERS[0], suggested.c_str ());
    }

    void App::open_file (const char* path)
    {
        io::ImportResult r;
        char err[512];
        double t0 = engine::timing_now ();
        if (!io::open_file (session_.ids, session_.fonts (), path, &r, err, sizeof err))
        {
            set_error (err);
            return;
        }
        double t1 = engine::timing_now ();
        session_.add_document (r.document, path);
        if (session_.timing)
        {
            engine::timing_log (session_, "open file", t1 - t0);
            engine::timing_log (session_, "  read from disk", r.timing.read);
            engine::timing_log (session_, "  decode", r.timing.decode);
            engine::timing_log (session_, "  build document", r.timing.build);
            engine::timing_log (session_, "  smart object contents", r.timing.smart);
            engine::timing_log (session_, "add document", engine::timing_now () - t1);
            time_next_render_ = true;
        }
        status_ = std::string ("Opened ") + path;
        for (size_t i = 0; i < r.warnings.size (); i++)
        {
            status_ += " (" + r.warnings[i] + ")";
        }
        status_is_error_ = !r.warnings.empty ();
    }

    void App::save_file (const char* path)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            set_error ("no document open");
            return;
        }
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = session_.cmyk;
        std::vector<std::string> warnings;
        char err[512];
        io::IoTiming t;
        double t0 = engine::timing_now ();
        if (!io::save_file_timed (*d->doc, session_.fonts (), path, NULL, codecs::EncodeOptions::defaults (), settings,
                &warnings, err, sizeof err, &t))
        {
            set_error (err);
            return;
        }
        if (session_.timing)
        {
            engine::timing_log (session_, "save file", engine::timing_now () - t0);
            engine::timing_log (session_, "  flatten", t.flatten);
            engine::timing_log (session_, "  encode", t.encode);
            engine::timing_log (session_, "  write to disk", t.write);
        }
        session_.mark_saved (path, io::file_stem (path));
        status_ = std::string ("Saved ") + path;
        for (size_t i = 0; i < warnings.size (); i++)
        {
            status_ += " (" + warnings[i] + ")";
        }
        status_is_error_ = false;
    }

    /* "Cmd+Shift+N" as ImGui sees it: Cmd is Ctrl on Linux. 0 when a key isn't mapped yet. */
    ImGuiKeyChord parse_shortcut (const char* s)
    {
        ImGuiKeyChord chord = 0;
        ImGuiKey key        = ImGuiKey_None;
        const char* p       = s;
        while (*p != 0)
        {
            const char* plus = strchr (p, '+');
            size_t n         = plus != NULL ? (size_t) (plus - p) : strlen (p);
            std::string part (p, n);
            if (part == "Cmd" || part == "Ctrl")
            {
                chord |= ImGuiMod_Ctrl;
            }
            else if (part == "Shift")
            {
                chord |= ImGuiMod_Shift;
            }
            else if (part == "Alt" || part == "Opt")
            {
                chord |= ImGuiMod_Alt;
            }
            else if (part.size () == 1 && isalpha ((unsigned char) part[0]))
            {
                key = (ImGuiKey) (ImGuiKey_A + (toupper ((unsigned char) part[0]) - 'A'));
            }
            else if (part.size () == 1 && isdigit ((unsigned char) part[0]))
            {
                key = (ImGuiKey) (ImGuiKey_0 + (part[0] - '0'));
            }
            else if (part == "[")
            {
                key = ImGuiKey_LeftBracket;
            }
            else if (part == "]")
            {
                key = ImGuiKey_RightBracket;
            }
            else if (part == "Delete")
            {
                key = ImGuiKey_Delete;
            }
            else if (part == "Backspace")
            {
                key = ImGuiKey_Backspace;
            }
            else if (part == "=")
            {
                key = ImGuiKey_Equal;
            }
            else if (part == "-")
            {
                key = ImGuiKey_Minus;
            }
            else if (part == ",")
            {
                key = ImGuiKey_Comma;
            }
            else if (part == "/")
            {
                key = ImGuiKey_Slash;
            }
            else if (part.size () >= 2 && part[0] == 'F' && isdigit ((unsigned char) part[1]))
            {
                int f = atoi (part.c_str () + 1);
                if (f >= 1 && f <= 12)
                {
                    key = (ImGuiKey) (ImGuiKey_F1 + f - 1);
                }
            }
            p += n;
            if (*p == '+')
            {
                p++;
            }
        }
        if (key == ImGuiKey_None)
        {
            return 0;
        }
        return chord | key;
    }

    /* The shortcut as the menu shows it on Linux. */
    std::string shortcut_label (const char* s)
    {
        std::string out = s;
        size_t at       = out.find ("Cmd");
        if (at != std::string::npos)
        {
            out.replace (at, 3, "Ctrl");
        }
        return out;
    }

    /* Engine commands whose menu path starts with `top`, nested by the rest of their path. ImGui
       appends to a menu opened twice under the same label, so commands sharing a submenu land in
       it whatever their order in the table. */
    void App::menu_bar ()
    {
        if (!ImGui::BeginMainMenuBar ())
        {
            return;
        }
        const std::vector<engine::CommandSpec>& all = session_.commands ();
        for (size_t m = 0; MENU_ORDER[m] != NULL; m++)
        {
            const char* top = MENU_ORDER[m];
            if (!ImGui::BeginMenu (top))
            {
                continue;
            }
            bool file = strcmp (top, "File") == 0;
            if (strcmp (top, "View") == 0)
            {
                view_menu ();
                ImGui::EndMenu ();
                continue;
            }
            if (file)
            {
                if (ImGui::MenuItem ("New…", "Ctrl+N"))
                {
                    show_new_dialog_ = true;
                }
                if (ImGui::MenuItem (UI_COMMANDS[0].label, "Ctrl+O"))
                {
                    run_command (UI_COMMANDS[0].id, engine::Value::object ());
                }
                ImGui::Separator ();
            }
            size_t top_len = strlen (top);
            for (size_t i = 0; i < all.size (); i++)
            {
                const engine::CommandSpec& c = all[i];
                if (strcmp (c.id, "file.new") == 0)
                {
                    continue;
                }
                if (strncmp (c.menu, top, top_len) != 0 || (c.menu[top_len] != 0 && c.menu[top_len] != '/'))
                {
                    continue;
                }
                std::vector<std::string> subs;
                const char* rest = c.menu + top_len;
                while (*rest == '/')
                {
                    rest++;
                    const char* slash = strchr (rest, '/');
                    size_t n          = slash ? (size_t) (slash - rest) : strlen (rest);
                    subs.push_back (std::string (rest, n));
                    rest += n;
                }
                size_t opened = 0;
                while (opened < subs.size () && ImGui::BeginMenu (subs[opened].c_str ()))
                {
                    opened++;
                }
                if (opened == subs.size ())
                {
                    std::string why;
                    bool enabled   = session_.command_enabled (c, &why);
                    std::string sc = c.shortcut != NULL ? shortcut_label (c.shortcut) : "";
                    if (ImGui::MenuItem (c.label, sc.empty () ? NULL : sc.c_str (), false, enabled))
                    {
                        run_command (c.id, engine::Value::object ());
                    }
                    if (!enabled && ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip ("%s", why.c_str ());
                    }
                }
                for (size_t k = 0; k < opened; k++)
                {
                    ImGui::EndMenu ();
                }
            }
            if (!file)
            {
                for (size_t i = 0; i < UI_COMMAND_COUNT; i++)
                {
                    if (strcmp (UI_COMMANDS[i].menu, top) != 0)
                    {
                        continue;
                    }
                    std::string sc = shortcut_label (UI_COMMANDS[i].shortcut);
                    if (ImGui::MenuItem (UI_COMMANDS[i].label, sc.c_str (), false, session_.has_active ()))
                    {
                        run_command (UI_COMMANDS[i].id, engine::Value::object ());
                    }
                }
            }
            if (strcmp (top, "Window") == 0)
            {
                if (ImGui::BeginMenu ("Theme"))
                {
                    for (int k = 0; k < THEME_COUNT; k++)
                    {
                        ThemeKind kind = (ThemeKind) k;
                        if (ImGui::MenuItem (theme_label (kind), NULL, theme_kind_ == kind))
                        {
                            set_theme (kind);
                        }
                    }
                    ImGui::EndMenu ();
                }
                if (ImGui::MenuItem ("Reset Panel Layout"))
                {
                    reset_layout_ = true;
                }
            }
            if (file)
            {
                bool has_doc = session_.has_active ();
                if (ImGui::MenuItem (UI_COMMANDS[1].label, "Ctrl+S", false, has_doc))
                {
                    run_command (UI_COMMANDS[1].id, engine::Value::object ());
                }
                if (ImGui::MenuItem (UI_COMMANDS[2].label, "Ctrl+Shift+S", false, has_doc))
                {
                    run_command (UI_COMMANDS[2].id, engine::Value::object ());
                }
                ImGui::Separator ();
                if (ImGui::MenuItem ("Exit", "Ctrl+Q"))
                {
                    quit_ = true;
                }
            }
            ImGui::EndMenu ();
        }
        ImGui::EndMainMenuBar ();
    }

    /* Single keys (M, X, D, Delete) belong to a text field while one is being typed in. */
    int slot_of (Tool t);
    const char* selection_mode (const ImGuiIO& io, int bar_mode);

    bool typing_blocks (ImGuiKeyChord chord)
    {
        return ImGui::GetIO ().WantTextInput && (chord & (ImGuiMod_Ctrl | ImGuiMod_Alt)) == 0;
    }

    void App::shortcuts ()
    {
        if (ImGui::Shortcut (ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
        {
            quit_ = true;
        }
        /* While type is being edited the keyboard belongs to it. */
        if (type_active_)
        {
            type_keys ();
            return;
        }
        view_shortcuts ();
        if (sam_.active)
        {
            sam_keys ();
            return;
        }
        if (!xf_.active && !warp_mode_active () && path_keys ())
        {
            return;
        }
        if (warp_mode_active () && !ImGui::GetIO ().WantTextInput)
        {
            if (ImGui::Shortcut (ImGuiKey_Enter, ImGuiInputFlags_RouteGlobal)
                || ImGui::Shortcut (ImGuiKey_KeypadEnter, ImGuiInputFlags_RouteGlobal))
            {
                commit_warp_mode ();
            }
            if (ImGui::Shortcut (ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal))
            {
                cancel_warp_mode ();
            }
        }
        if (xf_.active && !ImGui::GetIO ().WantTextInput)
        {
            if (ImGui::Shortcut (ImGuiKey_Enter, ImGuiInputFlags_RouteGlobal)
                || ImGui::Shortcut (ImGuiKey_KeypadEnter, ImGuiInputFlags_RouteGlobal))
            {
                commit_free_transform ();
            }
            if (ImGui::Shortcut (ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal))
            {
                cancel_free_transform ();
            }
        }
        for (size_t i = 0; i < UI_COMMAND_COUNT; i++)
        {
            ImGuiKeyChord chord = parse_shortcut (UI_COMMANDS[i].shortcut);
            if (chord != 0 && ImGui::Shortcut (chord, ImGuiInputFlags_RouteGlobal))
            {
                run_command (UI_COMMANDS[i].id, engine::Value::object ());
            }
        }
        const std::vector<engine::CommandSpec>& all = session_.commands ();
        for (size_t i = 0; i < all.size (); i++)
        {
            if (all[i].shortcut == NULL)
            {
                continue;
            }
            ImGuiKeyChord chord = parse_shortcut (all[i].shortcut);
            if (chord == 0 || typing_blocks (chord) || !ImGui::Shortcut (chord, ImGuiInputFlags_RouteGlobal))
            {
                continue;
            }
            if (strcmp (all[i].id, "file.new") == 0)
            {
                show_new_dialog_ = true;
                continue;
            }
            std::string why;
            if (session_.command_enabled (all[i], &why))
            {
                run_command (all[i].id, engine::Value::object ());
            }
        }
        /* Tool keys as in Photoshop. A key picks its group's last tool; with Shift it steps through
           the group (Shift+M switches the marquee's form). */
        if (typing_blocks (ImGuiKey_M))
        {
            return;
        }
        struct KeyTool
        {
            ImGuiKey key;
            Tool tools[6];
            int n;
        };
        const KeyTool keys[] = {
            {ImGuiKey_V, {Tool::Move}, 1},
            {ImGuiKey_M, {Tool::RectMarquee, Tool::EllipseMarquee}, 2},
            {ImGuiKey_L, {Tool::Lasso, Tool::PolygonLasso}, 2},
            {ImGuiKey_W, {Tool::QuickSelection, Tool::MagicWand}, 2},
            {ImGuiKey_C, {Tool::Crop}, 1},
            {ImGuiKey_I, {Tool::Eyedropper}, 1},
            {ImGuiKey_J,
                {Tool::SpotHealing, Tool::Remove, Tool::HealingBrush, Tool::Patch, Tool::ContentAwareMove,
                    Tool::RedEye},
                6},
            {ImGuiKey_B, {Tool::Brush, Tool::Pencil, Tool::ColorReplacement, Tool::MixerBrush}, 4},
            {ImGuiKey_S, {Tool::CloneStamp, Tool::PatternStamp}, 2},
            {ImGuiKey_Y, {Tool::HistoryBrush, Tool::ArtHistory}, 2},
            {ImGuiKey_E, {Tool::Eraser, Tool::BackgroundEraser, Tool::MagicEraser}, 3},
            {ImGuiKey_G, {Tool::Gradient, Tool::PaintBucket}, 2},
            {ImGuiKey_O, {Tool::Dodge, Tool::Burn, Tool::Sponge}, 3},
            {ImGuiKey_P, {Tool::Pen}, 1},
            {ImGuiKey_T, {Tool::Type}, 1},
            {ImGuiKey_A, {Tool::PathSelect, Tool::DirectSelect}, 2},
            {ImGuiKey_U, {Tool::RectShape, Tool::EllipseShape, Tool::PolygonShape, Tool::LineShape}, 4},
            {ImGuiKey_H, {Tool::Hand}, 1},
            {ImGuiKey_Z, {Tool::Zoom}, 1},
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        {
            const KeyTool& kt = keys[i];
            int n             = kt.n;
            int cur           = -1;
            for (int g = 0; g < n; g++)
            {
                if (kt.tools[g] == tool_)
                {
                    cur = g;
                }
            }
            if (ImGui::Shortcut (kt.key, ImGuiInputFlags_RouteGlobal))
            {
                int slot = slot_of (kt.tools[0]);
                select_tool (cur >= 0 ? tool_ : slot >= 0 ? slot_last_[slot] : kt.tools[0]);
            }
            if (n > 1 && ImGui::Shortcut (ImGuiMod_Shift | kt.key, ImGuiInputFlags_RouteGlobal))
            {
                select_tool (kt.tools[cur >= 0 ? (cur + 1) % n : 0]);
            }
        }
        /* [ and ] resize the brush in steps that grow with its size. */
        bool smaller = ImGui::Shortcut (ImGuiKey_LeftBracket, ImGuiInputFlags_RouteGlobal);
        bool larger  = ImGui::Shortcut (ImGuiKey_RightBracket, ImGuiInputFlags_RouteGlobal);
        if (smaller || larger)
        {
            bool quick = tool_ == Tool::QuickSelection;
            float size = quick ? quick_size_ : session_.tools.brush.size;
            float step = size < 10.0f  ? 1.0f
                       : size < 50.0f  ? 5.0f
                       : size < 100.0f ? 10.0f
                       : size < 200.0f ? 25.0f
                                       : 50.0f;
            size       = smaller ? fmaxf (1.0f, size - step) : fminf (paint::MAX_BRUSH_SIZE, size + step);
            if (quick)
            {
                quick_size_ = size;
            }
            else
            {
                run_command ("tools.setBrush", engine::Value{{"size", size}});
            }
        }
    }

    void upload_rgba (unsigned int* texture, const uint8_t* pixels, int width, int height, bool smooth)
    {
        if (*texture == 0)
        {
            glGenTextures (1, texture);
        }
        glBindTexture (GL_TEXTURE_2D, *texture);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D (
            GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) width, (GLsizei) height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    /* `rect` of the composite into the canvas texture, which must already have the canvas size. */
    void upload_region (unsigned int texture, const compose::Buffer& b)
    {
        raster::Rgba8Image img = b.to_rgba8 ();
        glBindTexture (GL_TEXTURE_2D, texture);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D (GL_TEXTURE_2D, 0, b.rect.x0, b.rect.y0, (GLsizei) img.width, (GLsizei) img.height, GL_RGBA,
            GL_UNSIGNED_BYTE, img.pixels.data ());
    }

    /* Whether two documents print the same Duotone inks (or neither is Duotone). */
    bool same_duotone (const doc::Document& a, const doc::Document& b)
    {
        if (a.has_duotone != b.has_duotone)
        {
            return false;
        }
        if (!a.has_duotone)
        {
            return true;
        }
        if (a.duotone.inks.size () != b.duotone.inks.size ())
        {
            return false;
        }
        for (size_t i = 0; i < a.duotone.inks.size (); i++)
        {
            const doc::DuotoneInk &x = a.duotone.inks[i], &y = b.duotone.inks[i];
            if (memcmp (x.color, y.color, sizeof x.color) != 0 || x.curve.size () != y.curve.size ())
            {
                return false;
            }
            for (size_t k = 0; k < x.curve.size (); k++)
            {
                if (x.curve[k].input != y.curve[k].input || x.curve[k].output != y.curve[k].output)
                {
                    return false;
                }
            }
        }
        return true;
    }

    /* The canvas shows the preview document while a dialog, a stroke or a drag previews an edit.
       Only what changed since the texture was drawn is composited again: the documents share
       unchanged tiles, so comparing them finds it. A live stroke paints into the same document
       object, so it reports its own damage. */
    void App::update_canvas_texture ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        if (texture_ != 0 && texture_doc_ == d->doc->id && texture_revision_ == d->revision
            && texture_preview_serial_ == preview_serial_ && !channel_view_dirty_)
        {
            return;
        }
        std::shared_ptr<const doc::Document> target = preview_doc_ ? preview_doc_ : d->doc;
        compose::RenderSettings settings            = compose::default_settings ();
        settings.cmyk                               = session_.cmyk;
        settings.masks                              = &mask_cache_;
        settings.inks                               = true;
        double render_start                         = engine::timing_now ();
        geom::Rect damage;
        bool partial = texture_ != 0 && shown_doc_ && texture_doc_ == d->doc->id
                    && texture_width_ == (int) target->size.width && texture_height_ == (int) target->size.height
                    && !channel_view_dirty_;
        /* The mode and Duotone inks aren't layer damage either: a change redraws everything. */
        if (partial && target != shown_doc_
            && (shown_doc_->mode != target->mode || !same_duotone (*shown_doc_, *target)))
        {
            partial = false;
        }
        /* Quick Mask shows the selection, which isn't part of the layer damage either. */
        if (partial && quick_mask_ && target != shown_doc_)
        {
            partial = compose::same_tiles (shown_doc_->selection, target->selection);
        }
        /* A viewed alpha channel isn't part of the layer damage: redraw everything when it changed. */
        if (partial && alpha_view_ >= 0)
        {
            size_t a = (size_t) alpha_view_;
            partial  = a < shown_doc_->channels.size () && a < target->channels.size ()
                   && compose::same_tiles (shown_doc_->channels[a].surface, target->channels[a].surface);
        }
        channel_view_dirty_ = false;
        if (partial && target == shown_doc_)
        {
            damage = stroke_damage_;
        }
        else if (partial)
        {
            partial = compose::composite_damage (*shown_doc_, *target, &damage);
        }
        stroke_damage_ = geom::Rect ();
        if (partial)
        {
            if (!damage.is_empty ())
            {
                compose::Buffer part = compose::render (*target, damage, settings);
                update_small_preview (*target, part);
                channel_filter (&part);
                proof_filter (&part);
                upload_region (texture_, part);
            }
        }
        else
        {
            compose::Buffer b = compose::flatten (*target, settings);
            small_.clear ();
            update_small_preview (*target, b);
            channel_filter (&b);
            proof_filter (&b);
            raster::Rgba8Image img = b.to_rgba8 ();
            upload_rgba (&texture_, img.pixels.data (), (int) img.width, (int) img.height, true);
            if (texture_doc_ != d->doc->id)
            {
                zoom_ = 0.0f;
            }
            texture_width_  = (int) img.width;
            texture_height_ = (int) img.height;
        }
        if (time_next_render_)
        {
            engine::timing_log (
                session_, partial ? "canvas update" : "canvas render (full)", engine::timing_now () - render_start);
            time_next_render_ = false;
        }
        shown_doc_              = target;
        texture_preview_serial_ = preview_serial_;
        texture_doc_            = d->doc->id;
        texture_revision_       = d->revision;
    }

    /* The selection's edge: where a pixel at least half selected meets one that isn't, as runs of
       pixel-boundary segments (horizontal runs as y, x0, x1; vertical as x, y0, y1). Worked out a
       tile at a time: a tile owns the boundaries along its top and left sides and inside it (and
       the canvas's bottom and right sides where it reaches them), so its runs depend only on it
       and its top and left neighbours. Tiles are shared until changed, so a cached tile whose three
       tiles are the same objects is still right, and a change only rescans the tiles it touched.
       selection_outline() draws the runs as marching ants. */

    /* The selection byte at (x, y) from a tile or the default; outside the canvas is unselected. */
    uint8_t sel_byte (const raster::Tile* t, geom::TileCoord c, uint8_t def, int32_t x, int32_t y, int32_t w, int32_t h)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
        {
            return 0;
        }
        if (t == NULL)
        {
            return def;
        }
        return t->data[(size_t) (y - c.ty * geom::TILE_SIZE) * (size_t) geom::TILE_SIZE
                       + (size_t) (x - c.tx * geom::TILE_SIZE)];
    }

    /* A tile and its top and left neighbours, for looking up pixels just across its sides. */
    struct SelLookup
    {
        const raster::Tile* ts;
        const raster::Tile* tt;
        const raster::Tile* tl;
        geom::TileCoord c;
        geom::TileCoord top;
        geom::TileCoord left;
        uint8_t def;
        int32_t w;
        int32_t h;
    };

    /* Whether (x, y) is selected (at least half), whichever of the three tiles holds it. */
    bool sel_in (const SelLookup& at, int32_t x, int32_t y)
    {
        if (y < at.c.ty * geom::TILE_SIZE)
        {
            return sel_byte (at.tt, at.top, at.def, x, y, at.w, at.h) >= 128;
        }
        if (x < at.c.tx * geom::TILE_SIZE)
        {
            return sel_byte (at.tl, at.left, at.def, x, y, at.w, at.h) >= 128;
        }
        return sel_byte (at.ts, at.c, at.def, x, y, at.w, at.h) >= 128;
    }

    void sel_tile_edges (const raster::Surface& sel, geom::TileCoord c, int32_t w, int32_t h, SelEdgeTile* out)
    {
        geom::TileCoord top = {c.tx, c.ty - 1}, left = {c.tx - 1, c.ty};
        out->h.clear ();
        out->v.clear ();
        geom::Rect r = c.rect ().intersect (geom::Rect{0, 0, w, h});
        if (r.is_empty ())
        {
            return;
        }
        SelLookup at  = {sel.tile (c), sel.tile (top), sel.tile (left), c, top, left, sel.default_bytes ()[0], w, h};
        int32_t ylast = r.y1 == h ? r.y1 : r.y1 - 1, xlast = r.x1 == w ? r.x1 : r.x1 - 1;
        for (int32_t y = r.y0; y <= ylast; y++)
        {
            int32_t run = -1;
            for (int32_t x = r.x0; x <= r.x1; x++)
            {
                bool edge = x < r.x1 && sel_in (at, x, y - 1) != sel_in (at, x, y);
                if (edge && run < 0)
                {
                    run = x;
                }
                if (!edge && run >= 0)
                {
                    out->h.push_back (y);
                    out->h.push_back (run);
                    out->h.push_back (x);
                    run = -1;
                }
            }
        }
        for (int32_t x = r.x0; x <= xlast; x++)
        {
            int32_t run = -1;
            for (int32_t y = r.y0; y <= r.y1; y++)
            {
                bool edge = y < r.y1 && sel_in (at, x - 1, y) != sel_in (at, x, y);
                if (edge && run < 0)
                {
                    run = y;
                }
                if (!edge && run >= 0)
                {
                    out->v.push_back (x);
                    out->v.push_back (run);
                    out->v.push_back (y);
                    run = -1;
                }
            }
        }
    }

    void App::update_selection_texture ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        if (selection_built_ && selection_doc_ == d->doc->id && selection_revision_ == d->revision
            && selection_preview_serial_ == preview_serial_)
        {
            return;
        }
        /* A preview (a Quick Selection stroke being dragged) shows its own selection. */
        const doc::Document& shown = preview_doc_ ? *preview_doc_ : *d->doc;
        selection_preview_serial_  = preview_serial_;
        int32_t w = (int32_t) shown.size.width, h = (int32_t) shown.size.height;
        if (selection_doc_ != d->doc->id || sel_edge_w_ != w || sel_edge_h_ != h)
        {
            sel_edges_.clear ();
        }
        selection_built_    = true;
        selection_doc_      = d->doc->id;
        selection_revision_ = d->revision;
        sel_edge_w_         = w;
        sel_edge_h_         = h;
        selection_visible_  = shown.has_selection;
        sel_hruns_.clear ();
        sel_vruns_.clear ();
        if (!selection_visible_)
        {
            sel_edges_.clear ();
            return;
        }
        raster::Surface sel = shown.selection;
        if (sel.format () != color::PixelFormat::gray8 ())
        {
            sel = sel.convert (color::PixelFormat::gray8 (), NULL);
        }
        bool def_in = sel.default_bytes ()[0] >= 128;
        /* Every tile over the canvas that can hold an edge: one with pixels, below or right of one,
           or anywhere when unstored tiles count as selected. */
        std::vector<geom::TileCoord> coords;
        geom::TileCoord last = geom::TileCoord::containing (w - 1, h - 1);
        for (int32_t ty = 0; ty <= last.ty; ty++)
        {
            for (int32_t tx = 0; tx <= last.tx; tx++)
            {
                geom::TileCoord c = {tx, ty}, top = {tx, ty - 1}, left = {tx - 1, ty};
                if (def_in || sel.tile (c) != NULL || sel.tile (top) != NULL || sel.tile (left) != NULL)
                {
                    coords.push_back (c);
                }
            }
        }
        std::map<geom::TileCoord, SelEdgeTile> next;
        std::vector<geom::TileCoord> todo;
        for (size_t i = 0; i < coords.size (); i++)
        {
            geom::TileCoord c = coords[i], top = {c.tx, c.ty - 1}, left = {c.tx - 1, c.ty};
            SelEdgeTile key;
            key.self                                             = sel.tile (c);
            key.top                                              = sel.tile (top);
            key.left                                             = sel.tile (left);
            key.def                                              = sel.default_bytes ()[0];
            std::map<geom::TileCoord, SelEdgeTile>::iterator old = sel_edges_.find (c);
            if (old != sel_edges_.end () && old->second.self == key.self && old->second.top == key.top
                && old->second.left == key.left && old->second.def == key.def)
            {
                next[c] = old->second;
                continue;
            }
            next[c] = key;
            todo.push_back (c);
        }
        /* The tiles that changed, in parallel (each writes only its own entry). */
        std::vector<SelEdgeTile*> slots (todo.size ());
        for (size_t i = 0; i < todo.size (); i++)
        {
            slots[i] = &next[todo[i]];
        }
#pragma omp parallel for schedule(dynamic, 1)
        for (ptrdiff_t i = 0; i < (ptrdiff_t) todo.size (); i++)
        {
            sel_tile_edges (sel, todo[(size_t) i], w, h, slots[(size_t) i]);
        }
        sel_edges_.swap (next);
        /* The cache keeps the tiles it was made from alive, so the pointers stay meaningful. */
        sel_edge_surface_ = sel;
        for (std::map<geom::TileCoord, SelEdgeTile>::const_iterator it = sel_edges_.begin (); it != sel_edges_.end ();
             ++it)
        {
            sel_hruns_.insert (sel_hruns_.end (), it->second.h.begin (), it->second.h.end ());
            sel_vruns_.insert (sel_vruns_.end (), it->second.v.begin (), it->second.v.end ());
        }
    }

    /* The stripes' grey at pattern position u (screen x + y + phase): black and white bands of
       `dash` pixels with a linear ramp of `ramp` pixels between them. */
    float ants_grey (float u, float dash, float ramp)
    {
        float m = fmodf (u, 2.0f * dash);
        if (m < 0.0f)
        {
            m += 2.0f * dash;
        }
        if (m < ramp)
        {
            return 1.0f - m / ramp;
        }
        if (m < dash)
        {
            return 0.0f;
        }
        if (m < dash + ramp)
        {
            return (m - dash) / ramp;
        }
        return 1.0f;
    }

    ImU32 ants_color (float grey, float alpha)
    {
        int v = (int) lroundf (grey * 255.0f);
        return IM_COL32 (v, v, v, (int) lroundf (alpha * 255.0f));
    }

    /* One run of the edge as ants: the edge is a window onto soft diagonal black and white
       stripes (constant screen x + y) that move with time, as Photoshop draws them. `a0`..`a1` is
       the run's clipped screen extent and `across` its screen position. Drawn a pixel row (or
       column) at a time so the stripes stay diagonal across the line; a fractional thickness
       draws its last row faded. */
    void draw_ants_run (
        ImDrawList* dl, bool horizontal, float a0, float a1, float across, float t, float dash, float phase)
    {
        float ramp  = dash * 0.4f;
        int rows    = (int) ceilf (t);
        float first = floorf (across - t * 0.5f);
        for (int r = 0; r < rows; r++)
        {
            float c     = first + (float) r;
            float alpha = fminf (t - (float) r, 1.0f);
            float base  = c + phase;
            /* Pieces between the pattern's breakpoints, each a linear gradient. */
            float period          = 2.0f * dash;
            float u               = a0 + base;
            float k               = floorf (u / period) * period;
            const float BREAKS[4] = {0.0f, ramp, dash, dash + ramp};
            float x               = a0;
            while (x < a1)
            {
                float next = a1;
                for (float p = k; p <= u + period * 2.0f && next == a1; p += period)
                {
                    for (int b = 0; b < 4; b++)
                    {
                        float at = p + BREAKS[b] - base;
                        if (at > x + 1e-3f && at < next)
                        {
                            next = at;
                            break;
                        }
                    }
                }
                ImU32 c0 = ants_color (ants_grey (x + base, dash, ramp), alpha),
                      c1 = ants_color (ants_grey (next + base, dash, ramp), alpha);
                if (horizontal)
                {
                    dl->AddRectFilledMultiColor (ImVec2 (x, c), ImVec2 (next, c + 1.0f), c0, c1, c1, c0);
                }
                else
                {
                    dl->AddRectFilledMultiColor (ImVec2 (c, x), ImVec2 (c + 1.0f, next), c0, c0, c1, c1);
                }
                x = next;
                u = x + base;
                k = floorf (u / period) * period;
            }
        }
    }

    /* Marching ants: the selection's edge in screen space at a fixed thickness whatever the zoom,
       stepping along a couple of pixels a few times a second. */
    void App::selection_outline (ImDrawList* dl, ImVec2 pos, ImVec2 clip0, ImVec2 clip1)
    {
        float t = fmaxf (1.5f, 1.5f * ui_scale_), dash = roundf (4.0f * ui_scale_);
        float step  = roundf (fmaxf (2.0f, 2.0f * ui_scale_));
        float phase = fmodf (floorf ((float) ImGui::GetTime () * 4.0f) * step, 2.0f * dash);
        for (size_t i = 0; i + 2 < sel_hruns_.size (); i += 3)
        {
            float y = roundf (pos.y + (float) sel_hruns_[i] * zoom_);
            if (y < clip0.y - t || y > clip1.y + t)
            {
                continue;
            }
            float x0 = fmaxf (floorf (pos.x + (float) sel_hruns_[i + 1] * zoom_ - t * 0.5f), clip0.x);
            float x1 = fminf (ceilf (pos.x + (float) sel_hruns_[i + 2] * zoom_ + t * 0.5f), clip1.x);
            if (x1 <= x0)
            {
                continue;
            }
            draw_ants_run (dl, true, x0, x1, y, t, dash, phase);
        }
        for (size_t i = 0; i + 2 < sel_vruns_.size (); i += 3)
        {
            float x = roundf (pos.x + (float) sel_vruns_[i] * zoom_);
            if (x < clip0.x - t || x > clip1.x + t)
            {
                continue;
            }
            float y0 = fmaxf (floorf (pos.y + (float) sel_vruns_[i + 1] * zoom_), clip0.y),
                  y1 = fminf (ceilf (pos.y + (float) sel_vruns_[i + 2] * zoom_), clip1.y);
            if (y1 <= y0)
            {
                continue;
            }
            draw_ants_run (dl, false, y0, y1, x, t, dash, phase);
        }
    }

    /* Zoom by `factor`, keeping the document point under (anchor_x, anchor_y) in place. */
    void App::zoom_by (float factor, float anchor_x, float anchor_y)
    {
        if (zoom_ <= 0.0f)
        {
            return;
        }
        fit_view_  = false;
        float next = fminf (fmaxf (zoom_ * factor, 0.02f), 32.0f);
        float cx = canvas_center_x_ + pan_x_, cy = canvas_center_y_ + pan_y_;
        pan_x_ += (anchor_x - cx) * (1.0f - next / zoom_);
        pan_y_ += (anchor_y - cy) * (1.0f - next / zoom_);
        zoom_ = next;
    }

    /* A brush or eraser stroke: rendered live onto a copy of the document (the canvas shows the real
       dabs), then committed as one paint.stroke with the same seed, which gives the same pixels. */
    /* The live stroke renders onto a copy of the document with the brush the command will use, so
       committing the same params, points and seed gives the same pixels. */
    void App::begin_stroke (float mx, float my)
    {
        /* The Eraser's Pencil and Block modes erase through the Pencil. */
        bool block = tool_ == Tool::Eraser && eraser_mode_ == 2;
        const char* cmd =
            tool_ == Tool::Pencil || (tool_ == Tool::Eraser && eraser_mode_ != 0) ? "paint.pencil" : "paint.stroke";
        const engine::CommandSpec* spec = session_.find_command (cmd);
        std::string why;
        if (spec == NULL || !session_.command_enabled (*spec, &why))
        {
            set_error (why);
            return;
        }
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d->doc->layer (d->active_layer);
        if (l == NULL)
        {
            return;
        }
        stroke_mask_    = mask_targeted ();
        bool auto_erase = tool_ == Tool::Pencil && pencil_auto_erase_;
        stroke_seed_    = paint::mix64 (++stroke_counter_ ^ SDL_GetTicksNS ());
        stroke_command_ = cmd;
        stroke_params_  = engine::Value{{"seed", stroke_seed_}, {"erase", tool_ == Tool::Eraser}, {"layer", l->id}};
        if (stroke_mask_)
        {
            stroke_params_["target"] = "mask";
        }
        stroke_quick_ = quick_mask_;
        if (stroke_quick_)
        {
            stroke_params_["target"] = "selection";
            stroke_mask_             = true;
        }
        if (auto_erase)
        {
            stroke_params_["autoErase"] = true;
        }
        if (block)
        {
            stroke_params_["block"] = true;
            stroke_params_["zoom"]  = zoom_;
        }
        paint::BrushSettings brush;
        engine::Error err;
        if (!engine::tool_brush (session_, cmd, stroke_params_, &brush, &err))
        {
            set_error (err.message);
            return;
        }
        stroke_doc_           = std::make_shared<doc::Document> (*d->doc);
        raster::Surface* surf = engine::stroke_surface (stroke_doc_.get (), l->id, stroke_params_, &stroke_lock_, &err);
        if (surf == NULL)
        {
            set_error (err.message);
            stroke_doc_.reset ();
            return;
        }
        paint::StrokePoint first = paint::StrokePoint::make (mx, my, 1.0f);
        engine::finish_brush (
            &brush, *surf, stroke_lock_, stroke_mask_, auto_erase, &first, session_.tools.background, session_.cmyk);
        stroke_layer_    = l->id;
        stroke_revision_ = d->revision;
        stroke_pre_      = *surf;
        stroke_renderer_.reset (new paint::StrokeRenderer (brush));
        stroke_points_.clear ();
        stroking_ = true;
        continue_stroke (mx, my);
    }

    void App::continue_stroke (float mx, float my)
    {
        if (!stroking_)
        {
            return;
        }
        paint::StrokePoint p = paint::StrokePoint::make (mx, my, 1.0f);
        if (!stroke_points_.empty () && fabs (stroke_points_.back ().x - p.x) < 1e-3
            && fabs (stroke_points_.back ().y - p.y) < 1e-3)
        {
            return;
        }
        stroke_points_.push_back (p);
        stroke_renderer_->push (&p, 1);
        doc::Layer* l = stroke_doc_->layer_mut (stroke_layer_);
        if (l == NULL)
        {
            return;
        }
        raster::Surface* surf = stroke_quick_ ? &stroke_doc_->selection : stroke_mask_ ? &l->mask.surface : &l->pixels;
        const raster::Surface* clip = stroke_doc_->has_selection && !stroke_quick_ ? &stroke_doc_->selection : NULL;
        geom::Rect dmg = stroke_renderer_->composite (stroke_pre_, surf, clip, stroke_lock_, false, session_.cmyk);
        if (stroke_mask_ && !stroke_quick_ && compose::has_feather (l->mask))
        {
            dmg = dmg.inflate ((int32_t) ceilf (compose::feather_sigma (l->mask.feather) * 3.0f) + 2);
        }
        /* A styled layer's effects reach past the dabs. */
        if (!dmg.is_empty () && compose::has_effects (*l))
        {
            dmg = dmg.inflate (2 * compose::effects_margin (*l));
        }
        stroke_damage_ = stroke_damage_.unite (dmg);
        preview_doc_   = stroke_doc_;
        preview_serial_++;
    }

    void App::end_stroke ()
    {
        if (!stroking_)
        {
            return;
        }
        stroking_         = false;
        engine::Value pts = engine::Value::array ();
        for (size_t i = 0; i < stroke_points_.size (); i++)
        {
            pts.push_back (engine::Value{stroke_points_[i].x, stroke_points_[i].y, stroke_points_[i].pressure});
        }
        engine::Value params = stroke_params_;
        params["points"]     = pts;
        /* The live pixels, finished as the command renders them, go with the commit. */
        doc::Layer* l             = stroke_doc_->layer_mut (stroke_layer_);
        const engine::DocState* d = session_.active ();
        if (l != NULL && d != NULL && !pts.empty ())
        {
            raster::Surface* surf = stroke_quick_ ? &stroke_doc_->selection
                                  : stroke_mask_  ? &l->mask.surface
                                                  : &l->pixels;
            stroke_renderer_->finish ();
            const raster::Surface* clip = stroke_doc_->has_selection && !stroke_quick_ ? &stroke_doc_->selection : NULL;
            stroke_renderer_->composite (stroke_pre_, surf, clip, stroke_lock_, false, session_.cmyk);
            engine::LiveStroke& live = session_.live_stroke;
            live.valid               = true;
            live.command             = stroke_command_;
            live.params              = params;
            live.doc                 = d->doc->id;
            live.revision            = stroke_revision_;
            live.surface             = *surf;
            live.bounds              = stroke_renderer_->bounds ();
        }
        clear_preview ();
        stroke_renderer_.reset ();
        stroke_doc_.reset ();
        stroke_pre_ = raster::Surface ();
        if (pts.empty ())
        {
            return;
        }
        run_command (stroke_command_.c_str (), params);
    }

    /* The Move tool's live preview: the selected layers (with their linked layers) moved on a copy,
       or an unlinked mask alone while the mask is targeted. A locked layer ends the drag. */
    void App::move_preview (int32_t dx, int32_t dy)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        engine::Error err;
        if (!engine::move_layers (copy.get (), d->selected (), dx, dy, mask_targeted (), &err))
        {
            set_error (err.message);
            moving_ = false;
            clear_preview ();
            return;
        }
        preview_doc_ = copy;
        preview_serial_++;
    }

    /* Tool input on the canvas item. ox, oy: the screen position of document pixel (0, 0). */
    void App::canvas_input (float ox, float oy)
    {
        ImGuiIO& io = ImGui::GetIO ();
        float mx    = (io.MousePos.x - ox) / zoom_;
        float my    = (io.MousePos.y - oy) / zoom_;
        if (ImGui::IsItemHovered () || ImGui::IsItemActive ())
        {
            info_x_    = (int32_t) floorf (mx);
            info_y_    = (int32_t) floorf (my);
            info_over_ = true;
        }
        else
        {
            info_over_ = false;
        }
        bool activated = ImGui::IsItemActivated () && ImGui::IsMouseClicked (ImGuiMouseButton_Left);
        bool active    = ImGui::IsItemActive () && ImGui::IsMouseDown (ImGuiMouseButton_Left);
        bool released  = ImGui::IsMouseReleased (ImGuiMouseButton_Left);

        /* Space-drag or a middle-button drag pans whatever the tool. */
        bool space = ImGui::IsKeyDown (ImGuiKey_Space) && !io.WantTextInput;
        if ((space && (activated || panning_))
            || (ImGui::IsItemHovered () && ImGui::IsMouseDragging (ImGuiMouseButton_Middle, 0.0f)))
        {
            panning_ = ImGui::IsMouseDown (ImGuiMouseButton_Left) || ImGui::IsMouseDown (ImGuiMouseButton_Middle);
            pan_x_ += io.MouseDelta.x;
            pan_y_ += io.MouseDelta.y;
            fit_view_ = false;
            return;
        }
        panning_ = false;
        if (xf_.active)
        {
            free_transform_input (ox, oy);
            return;
        }
        if (pw_.active)
        {
            puppet_input (ox, oy);
            return;
        }
        if (sam_.active && (sam_.tool == 1 || sam_.tool == 2))
        {
            sam_input (ox, oy);
            return;
        }
        if (vw_.active)
        {
            perspective_input (ox, oy);
            return;
        }
        if (eyedropper_input (mx, my))
        {
            return;
        }
        if (type_tool_input (ox, oy))
        {
            return;
        }
        if (path_tool_input (ox, oy))
        {
            return;
        }

        switch (tool_)
        {
        case Tool::Hand:
            /* Drag scrolls the view; a double-click fits the image on screen. */
            if (ImGui::IsItemHovered ())
            {
                ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
            }
            if (activated && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                run_command ("view.fitOnScreen", engine::Value::object ());
                return;
            }
            if (active)
            {
                pan_x_ += io.MouseDelta.x;
                pan_y_ += io.MouseDelta.y;
                fit_view_ = false;
            }
            return;
        case Tool::Zoom:
            /* Click zooms in at the pointer, Alt-click out; a double-click shows 100%. */
            if (activated && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                run_command ("view.actualPixels", engine::Value::object ());
                return;
            }
            if (activated)
            {
                zoom_by (io.KeyAlt ? 0.8f : 1.25f, io.MousePos.x, io.MousePos.y);
            }
            return;
        case Tool::Brush:
        case Tool::Pencil:
        case Tool::Eraser:
            if (activated)
            {
                begin_stroke (mx, my);
            }
            else if (active)
            {
                continue_stroke (mx, my);
            }
            if (stroking_ && released)
            {
                end_stroke ();
            }
            return;
        case Tool::Move:
            if (activated)
            {
                const engine::CommandSpec* spec = session_.find_command ("layer.translate");
                std::string why;
                if (spec == NULL || !session_.command_enabled (*spec, &why))
                {
                    set_error (why);
                    return;
                }
                moving_  = true;
                move_x0_ = mx;
                move_y0_ = my;
                move_dx_ = move_dy_ = 0;
                /* The moving layers' bounds, for snapping their edges. */
                const engine::DocState* ds = session_.active ();
                geom::Rect mb;
                std::vector<doc::LayerId> sel = ds->selected ();
                for (size_t i = 0; i < sel.size (); i++)
                {
                    const doc::Layer* ml = ds->doc->layer (sel[i]);
                    if (ml != NULL && ml->surface () != NULL)
                    {
                        mb = mb.unite (ml->surface ()->content_bounds ());
                    }
                }
                move_box_[0]     = (float) mb.x0;
                move_box_[1]     = (float) mb.y0;
                move_box_[2]     = (float) mb.x1;
                move_box_[3]     = (float) mb.y1;
                move_has_box_    = !mb.is_empty ();
                snap_skip_layer_ = ds->active_layer;
            }
            if (!moving_)
            {
                return;
            }
            if (active)
            {
                float fdx = mx - move_x0_, fdy = my - move_y0_;
                if (move_has_box_)
                {
                    snap_box_move (move_box_, &fdx, &fdy);
                }
                int32_t dx = (int32_t) lroundf (fdx), dy = (int32_t) lroundf (fdy);
                if (dx != move_dx_ || dy != move_dy_)
                {
                    move_dx_ = dx;
                    move_dy_ = dy;
                    move_preview (dx, dy);
                }
            }
            if (released)
            {
                moving_ = false;
                clear_preview ();
                engine::Value params = {{"dx", move_dx_}, {"dy", move_dy_}};
                if (mask_targeted ())
                {
                    params["target"] = "mask";
                }
                if (move_dx_ != 0 || move_dy_ != 0)
                {
                    run_command ("layer.translate", params);
                }
            }
            return;
        default:
            break;
        }

        if (select_tool_input (ox, oy))
        {
            return;
        }
        if (heal_tool_input (ox, oy))
        {
            return;
        }
        if (fill_tool_input (ox, oy))
        {
            return;
        }
        if (crop_tool_input (ox, oy))
        {
            return;
        }
        if (retouch_tool_input (ox, oy))
        {
            return;
        }

        /* The marquee: drag a rectangle or ellipse. Shift adds, Alt subtracts, both intersect. A
           click without a drag deselects. */
        if (activated)
        {
            dragging_        = true;
            snap_skip_layer_ = doc::NO_LAYER;
            snap_point (&mx, &my, true, true, false);
            drag_x0_ = drag_x1_ = mx;
            drag_y0_ = drag_y1_ = my;
        }
        if (!dragging_)
        {
            return;
        }
        snap_point (&mx, &my, true, true, false);
        drag_x1_ = mx;
        drag_y1_ = my;
        float ax = fminf (drag_x0_, drag_x1_), bx = fmaxf (drag_x0_, drag_x1_);
        float ay = fminf (drag_y0_, drag_y1_), by = fmaxf (drag_y0_, drag_y1_);
        int32_t x0 = (int32_t) floorf (ax + 0.5f), x1 = (int32_t) floorf (bx + 0.5f);
        int32_t y0 = (int32_t) floorf (ay + 0.5f), y1 = (int32_t) floorf (by + 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        ImVec2 a (ox + (float) x0 * zoom_, oy + (float) y0 * zoom_),
            b (ox + (float) x1 * zoom_, oy + (float) y1 * zoom_);
        if (tool_ == Tool::EllipseMarquee)
        {
            ImVec2 c ((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            dl->AddEllipse (
                c, ImVec2 ((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f), IM_COL32 (255, 255, 255, 255), 0.0f, 0, 1.0f);
        }
        else
        {
            dl->AddRect (a, b, IM_COL32 (255, 255, 255, 255));
        }
        if (!released)
        {
            return;
        }
        dragging_ = false;
        if (x1 - x0 < 1 || y1 - y0 < 1)
        {
            const engine::DocState* d = session_.active ();
            if (d != NULL && d->doc->has_selection)
            {
                run_command ("select.deselect", engine::Value::object ());
            }
            return;
        }
        const char* mode = selection_mode (io, selection_mode_);
        run_command ("select.rect",
            engine::Value{{"x", x0}, {"y", y0}, {"width", x1 - x0}, {"height", y1 - y0}, {"mode", mode},
                {"ellipse", tool_ == Tool::EllipseMarquee}, {"feather", sel_feather_}, {"antiAlias", sel_anti_alias_}});
    }

    /* The composite over a checkerboard (transparency). Fit on screen until zoomed; the wheel
       scrolls (Shift: sideways), Ctrl+wheel zooms at the pointer. */
    const char* mode_name (color::ColorMode m)
    {
        switch (m)
        {
        case color::ColorMode::Bitmap:
            return "Bitmap";
        case color::ColorMode::Grayscale:
            return "Gray";
        case color::ColorMode::Indexed:
            return "Index";
        case color::ColorMode::Rgb:
            return "RGB";
        case color::ColorMode::Cmyk:
            return "CMYK";
        case color::ColorMode::Lab:
            return "Lab";
        case color::ColorMode::Multichannel:
            return "Multichannel";
        case color::ColorMode::Duotone:
            return "Duotone";
        }
        return "";
    }

    /* One tab per open document, as Photoshop titles them: "name @ zoom% (layer, RGB/8)" with a
       star for unsaved changes. Closing an unsaved document asks first. */
    void App::document_tabs ()
    {
        size_t n = session_.doc_count ();
        if (n == 0)
        {
            return;
        }
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (10.0f * ui_scale_, 5.0f * ui_scale_));
        ImGui::PushStyleColor (ImGuiCol_TabSelected, theme_.card);
        if (ImGui::BeginTabBar (
                "##documents", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTabListScrollingButtons))
        {
            size_t active  = session_.active_index ();
            int select_now = -1;
            for (size_t i = 0; i < n; i++)
            {
                const engine::DocState* d = session_.doc_at (i);
                const doc::Layer* l       = d->doc->layer (d->active_layer);
                char label[512];
                unsigned bits = d->doc->depth == color::SampleType::U8  ? 8
                              : d->doc->depth == color::SampleType::U16 ? 16
                                                                        : 32;
                if (i == active)
                {
                    snprintf (label, sizeof label, "%s @ %.0f%% (%s, %s/%u)%s###doc%llu", d->doc->name.c_str (),
                        zoom_ * 100.0f, l != NULL ? l->name.c_str () : "", mode_name (d->doc->mode), bits,
                        d->dirty () ? "*" : "", (unsigned long long) d->doc->id);
                }
                else
                {
                    snprintf (label, sizeof label, "%s (%s/%u)%s###doc%llu", d->doc->name.c_str (),
                        mode_name (d->doc->mode), bits, d->dirty () ? "*" : "", (unsigned long long) d->doc->id);
                }
                bool open               = true;
                ImGuiTabItemFlags flags = i == active && tab_active_ != active ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem (label, &open, flags))
                {
                    if (i != active && tab_active_ == active)
                    {
                        select_now = (int) i;
                    }
                    ImGui::EndTabItem ();
                }
                if (!open)
                {
                    close_request_ = (int) i;
                }
            }
            tab_active_ = active;
            if (select_now >= 0)
            {
                session_.set_active ((size_t) select_now);
                tab_active_ = (size_t) select_now;
            }
            ImGui::EndTabBar ();
        }
        ImGui::PopStyleColor ();
        ImGui::PopStyleVar ();
    }

    /* "Close" on a tab: unsaved changes are confirmed first. */
    void App::close_prompt ()
    {
        if (close_request_ < 0)
        {
            return;
        }
        if ((size_t) close_request_ >= session_.doc_count ())
        {
            close_request_ = -1;
            return;
        }
        const engine::DocState* d = session_.doc_at ((size_t) close_request_);
        if (!d->dirty ())
        {
            run_command ("file.close", engine::Value{{"document", close_request_}});
            close_request_ = -1;
            return;
        }
        ImGui::OpenPopup ("Close Document###close");
        if (!ImGui::BeginPopupModal ("Close Document###close", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::Text ("\"%s\" has unsaved changes. Close it anyway?", d->doc->name.c_str ());
        ImGui::Spacing ();
        if (d->smart_parent != 0)
        {
            if (ImGui::Button ("Save to Smart Object"))
            {
                int index = close_request_;
                session_.set_active ((size_t) index);
                run_command ("layer.smartObject.commitContents", engine::Value::object ());
                if (!session_.doc_at ((size_t) index)->dirty ())
                {
                    run_command ("file.close", engine::Value{{"document", index}});
                }
                close_request_ = -1;
                ImGui::CloseCurrentPopup ();
                ImGui::EndPopup ();
                return;
            }
            ImGui::SameLine ();
        }
        if (ImGui::Button ("Close Without Saving"))
        {
            run_command ("file.close", engine::Value{{"document", close_request_}});
            close_request_ = -1;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape))
        {
            close_request_ = -1;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    void App::canvas_window ()
    {
        ImGuiWindowClass cls;
        cls.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
        ImGui::SetNextWindowClass (&cls);
        /* Tab: the canvas alone fills the window; back in the dock's centre afterwards. */
        if (panels_hidden_)
        {
            ImGuiViewport* vp = ImGui::GetMainViewport ();
            float status_h    = ImGui::GetFrameHeight () + 4 * ui_scale_;
            ImGui::SetNextWindowDockID (0, ImGuiCond_Always);
            ImGui::SetNextWindowPos (vp->WorkPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize (ImVec2 (vp->WorkSize.x, vp->WorkSize.y - status_h), ImGuiCond_Always);
        }
        else if (redock_canvas_ && main_dock_ != 0)
        {
            ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode (main_dock_);
            if (central != NULL)
            {
                ImGui::SetNextWindowDockID (central->ID, ImGuiCond_Always);
            }
            redock_canvas_ = false;
        }
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.canvas);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0, 0));
        ImGui::Begin ("Canvas###canvas", NULL,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                | (panels_hidden_ ? ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove : 0));
        ImGui::PopStyleVar ();
        ImGui::PopStyleColor ();
        ImGui::PushStyleColor (ImGuiCol_ChildBg, theme_.tab_strip);
        document_tabs ();
        ImGui::PopStyleColor ();
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            ImVec2 a        = ImGui::GetContentRegionAvail ();
            const char* msg = "No document open. File > New or File > Open.";
            ImVec2 ts       = ImGui::CalcTextSize (msg);
            ImGui::SetCursorPos (ImVec2 ((a.x - ts.x) * 0.5f, (a.y - ts.y) * 0.5f));
            ImGui::TextDisabled ("%s", msg);
            ImGui::End ();
            return;
        }
        update_canvas_texture ();
        update_selection_texture ();
        ImVec2 full_avail  = ImGui::GetContentRegionAvail ();
        ImVec2 full_origin = ImGui::GetCursorScreenPos ();
        /* Rulers take a band along the top and left. */
        ruler_px_ = view_rulers_ ? roundf (16.0f * ui_scale_) : 0.0f;
        ImVec2 avail (full_avail.x - ruler_px_, full_avail.y - ruler_px_);
        ImVec2 origin (full_origin.x + ruler_px_, full_origin.y + ruler_px_);
        if (avail.x < 1.0f || avail.y < 1.0f)
        {
            ImGui::End ();
            return;
        }
        canvas_avail_x_  = avail.x;
        canvas_avail_y_  = avail.y;
        canvas_center_x_ = origin.x + avail.x * 0.5f;
        canvas_center_y_ = origin.y + avail.y * 0.5f;
        /* Fitted on screen, the view follows the canvas area's size until zoomed or scrolled. */
        if (zoom_ > 0.0f && fit_view_ && (avail.x != fit_avail_x_ || avail.y != fit_avail_y_))
        {
            zoom_ = 0.0f;
        }
        if (zoom_ <= 0.0f && texture_width_ > 0 && texture_height_ > 0)
        {
            fit_view_    = true;
            fit_avail_x_ = avail.x;
            fit_avail_y_ = avail.y;
            float fx = avail.x / (float) texture_width_, fy = avail.y / (float) texture_height_;
            zoom_  = fminf (fminf (fx, fy), 1.0f);
            pan_x_ = pan_y_ = 0.0f;
        }

        ImGuiIO& io = ImGui::GetIO ();
        if (ImGui::IsWindowHovered () && io.MouseWheel != 0.0f)
        {
            if (io.KeyCtrl)
            {
                zoom_by (io.MouseWheel > 0.0f ? 1.25f : 0.8f, io.MousePos.x, io.MousePos.y);
            }
            else if (io.KeyShift)
            {
                pan_x_ += io.MouseWheel * 60.0f;
            }
            else
            {
                pan_y_ += io.MouseWheel * 60.0f;
            }
            fit_view_ = false;
        }
        if (ImGui::IsWindowHovered () && io.MouseWheelH != 0.0f)
        {
            pan_x_ += io.MouseWheelH * 60.0f;
        }

        ImVec2 size ((float) texture_width_ * zoom_, (float) texture_height_ * zoom_);
        ImVec2 pos (
            floorf (canvas_center_x_ - size.x * 0.5f + pan_x_), floorf (canvas_center_y_ - size.y * 0.5f + pan_y_));
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        dl->PushClipRect (origin, ImVec2 (origin.x + avail.x, origin.y + avail.y), true);
        ImVec2 vis0 (fmaxf (pos.x, origin.x), fmaxf (pos.y, origin.y));
        ImVec2 vis1 (fminf (pos.x + size.x, origin.x + avail.x), fminf (pos.y + size.y, origin.y + avail.y));
        const float cell = 8.0f;
        for (float y = floorf ((vis0.y - pos.y) / cell) * cell; pos.y + y < vis1.y; y += cell)
        {
            for (float x = floorf ((vis0.x - pos.x) / cell) * cell; pos.x + x < vis1.x; x += cell)
            {
                bool dark = ((int) (x / cell) + (int) (y / cell)) % 2 == 1;
                ImVec2 a (fmaxf (pos.x + x, vis0.x), fmaxf (pos.y + y, vis0.y));
                ImVec2 b (fminf (pos.x + x + cell, vis1.x), fminf (pos.y + y + cell, vis1.y));
                dl->AddRectFilled (a, b, dark ? IM_COL32 (204, 204, 204, 255) : IM_COL32 (255, 255, 255, 255));
            }
        }
        dl->AddImage ((ImTextureID) (intptr_t) texture_, pos, ImVec2 (pos.x + size.x, pos.y + size.y));
        view_overlays (pos.x, pos.y, origin, avail);
        if (selection_visible_ && !quick_mask_ && view_extras_ && view_sel_edges_)
        {
            selection_outline (dl, pos, vis0, vis1);
        }

        ImGui::SetCursorScreenPos (origin);
        ImGui::InvisibleButton (
            "##canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        if (!guide_input (pos.x, pos.y, origin, avail))
        {
            canvas_input (pos.x, pos.y);
        }

        /* The brush outline at the size where the tip covers 50%, as Photoshop's normal brush tip. */
        if (!xf_.active && !warp_mode_active () && uses_brush (tool_) && ImGui::IsItemHovered ()
            && !ImGui::IsKeyDown (ImGuiKey_Space))
        {
            const paint::BrushSettings& b = brush_editing_ ? brush_edit_ : session_.tools.brush;
            float r                       = paint::half_coverage_radius (b.size * 0.5f, b.hardness) * zoom_;
            if (tool_ == Tool::Eraser && eraser_mode_ == 2)
            {
                float h = roundf (16.0f / zoom_) * zoom_ * 0.5f;
                ImVec2 a (io.MousePos.x - h, io.MousePos.y - h), c (io.MousePos.x + h, io.MousePos.y + h);
                dl->AddRect (a, c, IM_COL32 (0, 0, 0, 200), 0.0f, 0, 1.5f);
            }
            else if (r >= 1.0f)
            {
                dl->AddCircle (io.MousePos, r, IM_COL32 (0, 0, 0, 200), 0, 1.5f);
                dl->AddCircle (io.MousePos, r - 1.0f, IM_COL32 (255, 255, 255, 200), 0, 1.0f);
            }
        }
        if (xf_.active)
        {
            free_transform_overlay (pos.x, pos.y);
        }
        else if (pw_.active)
        {
            puppet_overlay (pos.x, pos.y);
        }
        else if (vw_.active)
        {
            perspective_overlay (pos.x, pos.y);
        }
        else if (sam_.active && (sam_.tool == 1 || sam_.tool == 2))
        {
            sam_overlay (pos.x, pos.y);
        }
        else
        {
            select_tool_overlay (pos.x, pos.y);
            type_overlay (pos.x, pos.y);
            path_tool_overlay (pos.x, pos.y);
            fill_tool_overlay (pos.x, pos.y);
            crop_tool_overlay (pos.x, pos.y);
            retouch_tool_overlay (pos.x, pos.y);
            heal_tool_overlay (pos.x, pos.y);
        }
        dl->PopClipRect ();
        if (view_rulers_)
        {
            if (guide_drag_ == -2)
            {
                guide_input (pos.x, pos.y, origin, avail);
            }
            rulers (full_origin, full_avail, pos.x, pos.y);
        }
        ImGui::End ();
    }

    /* Painting goes to the active layer's mask once its mask thumbnail was clicked, until another
       layer (or the layer's name) is. */
    bool App::mask_targeted () const
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL || d->active_layer == doc::NO_LAYER || mask_target_layer_ != d->active_layer)
        {
            return false;
        }
        const doc::Layer* l = d->doc->layer (d->active_layer);
        return l != NULL && l->has_mask;
    }

    /* A menu item for an engine command, greyed out when the command is not available. */
    void App::command_item (const char* id, const char* label)
    {
        const engine::CommandSpec* spec = session_.find_command (id);
        if (spec == NULL)
        {
            return;
        }
        std::string why;
        bool enabled = session_.command_enabled (*spec, &why);
        if (ImGui::MenuItem (label != NULL ? label : spec->label,
                spec->shortcut != NULL ? shortcut_label (spec->shortcut).c_str () : NULL, false, enabled))
        {
            run_command (id, engine::Value::object ());
        }
    }

    void App::layer_context_menu ()
    {
        command_item ("layer.duplicate", NULL);
        command_item ("layer.delete", NULL);
        command_item ("layer.renameLayer", "Rename Layer…");
        ImGui::Separator ();
        command_item ("layer.groupLayers", NULL);
        command_item ("layer.ungroupLayers", NULL);
        command_item ("layer.new.layerViaCopy", NULL);
        command_item ("layer.new.layerFromBackground", NULL);
        ImGui::Separator ();
        command_item ("layer.layerMask.revealAll", "Add Layer Mask");
        command_item ("layer.layerMask.revealSelection", "Add Mask from Selection");
        command_item ("layer.layerMask.enabled", "Disable/Enable Layer Mask");
        command_item ("layer.layerMask.linked", "Unlink/Link Layer Mask");
        command_item ("layer.layerMask.apply", "Apply Layer Mask");
        command_item ("layer.layerMask.delete", "Delete Layer Mask");
        ImGui::Separator ();
        command_item ("layer.createClippingMask", NULL);
        command_item ("layer.releaseClippingMask", NULL);
        command_item ("layer.linkLayers", NULL);
        command_item ("layer.selectLinkedLayers", NULL);
        ImGui::Separator ();
        command_item ("layer.mergeLayers", NULL);
        command_item ("layer.mergeVisible", NULL);
        command_item ("layer.flattenImage", NULL);
    }

    void App::history_window ()
    {
        ImGui::Begin ("History");
        const engine::DocState* d = session_.active ();
        if (d != NULL)
        {
            std::vector<std::string> entries = d->history.entries ();
            /* Steps often share a label ("Fill", "Fill"): each row gets its own ID scope. */
            for (size_t i = 0; i < entries.size (); i++)
            {
                ImGui::PushID ((int) i);
                ImGui::Selectable (entries[i].c_str (), i + 1 == entries.size ());
                ImGui::PopID ();
            }
            std::vector<std::string> redo = d->history.redo_labels ();
            for (size_t i = 0; i < redo.size (); i++)
            {
                ImGui::TextDisabled ("%s", redo[i].c_str ());
            }
        }
        ImGui::End ();
    }

    void App::new_document_dialog ()
    {
        if (show_new_dialog_)
        {
            ImGui::OpenPopup ("New Document");
            show_new_dialog_ = false;
        }
        if (!ImGui::BeginPopupModal ("New Document", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::InputText ("Name", new_name_, sizeof new_name_);
        ImGui::InputInt ("Width (px)", &new_width_);
        ImGui::InputInt ("Height (px)", &new_height_);
        const char* backgrounds[] = {"white", "black", "transparent"};
        ImGui::Combo ("Background", &new_background_, backgrounds, 3);
        if (ImGui::Button ("Create"))
        {
            run_command ("file.new", engine::Value{{"name", new_name_}, {"width", new_width_}, {"height", new_height_},
                                         {"background", backgrounds[new_background_]}});
            if (!status_is_error_)
            {
                ImGui::CloseCurrentPopup ();
            }
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel"))
        {
            ImGui::CloseCurrentPopup ();
        }
        if (status_is_error_)
        {
            ImGui::TextColored (ImVec4 (1.0f, 0.4f, 0.4f, 1.0f), "%s", status_.c_str ());
        }
        ImGui::EndPopup ();
    }

    const char* const STATUS_INFO[][2] = {
        {"sizes", "Document Sizes"},
        {"profile", "Document Profile"},
        {"dimensions", "Document Dimensions"},
        {"tool", "Current Tool"},
        {"layers", "Layer Count"},
    };

    /* Photoshop's compact byte format: "10.3M", "412.5K". */
    void fmt_bytes (uint64_t b, char* out, size_t len)
    {
        double v = (double) b;
        if (v >= 1024.0 * 1024.0 * 1024.0)
        {
            snprintf (out, len, "%.1fG", v / (1024.0 * 1024.0 * 1024.0));
        }
        else if (v >= 1024.0 * 1024.0)
        {
            snprintf (out, len, "%.1fM", v / (1024.0 * 1024.0));
        }
        else
        {
            snprintf (out, len, "%.1fK", v / 1024.0);
        }
    }

    const char* tool_label (Tool t);

    /* The readout next to the zoom field, in Photoshop's wording. */
    std::string App::status_info (const doc::Document& d)
    {
        char buf[256];
        unsigned bits = d.depth == color::SampleType::U8 ? 8 : d.depth == color::SampleType::U16 ? 16 : 32;
        if (status_info_ == "sizes")
        {
            uint64_t bpc = bits / 8, ch = color::color_channels (d.mode);
            uint64_t flat = (uint64_t) d.size.width * d.size.height * ch * bpc, layered = 0;
            std::vector<doc::WalkEntry> walk = d.walk ();
            for (size_t i = 0; i < walk.size (); i++)
            {
                const raster::Surface* s = walk[i].layer->surface ();
                if (s == NULL)
                {
                    continue;
                }
                geom::Rect r = s->tile_bounds ().intersect (d.bounds ());
                layered += (uint64_t) r.width () * r.height () * (ch + 1) * bpc;
            }
            if (layered < flat)
            {
                layered = flat;
            }
            char a[32], b[32];
            fmt_bytes (flat, a, sizeof a);
            fmt_bytes (layered, b, sizeof b);
            snprintf (buf, sizeof buf, "Doc: %s/%s", a, b);
        }
        else if (status_info_ == "profile")
        {
            snprintf (buf, sizeof buf, "%s (%ubpc)", d.icc_profile ? "Embedded profile" : "sRGB IEC61966-2.1", bits);
        }
        else if (status_info_ == "tool")
        {
            snprintf (buf, sizeof buf, "%s", tool_label (tool_));
        }
        else if (status_info_ == "layers")
        {
            size_t n = d.walk ().size ();
            snprintf (buf, sizeof buf, "%zu Layer%s", n, n == 1 ? "" : "s");
        }
        else
        {
            snprintf (
                buf, sizeof buf, "%u px x %u px (%g ppi)", d.size.width, d.size.height, (double) d.resolution_dpi);
        }
        return buf;
    }

    /* The status bar: an editable zoom percentage, the chosen document readout (the arrow picks
       which), and the last message. */
    void App::status_bar ()
    {
        ImGuiViewport* vp = ImGui::GetMainViewport ();
        float k = ui_scale_, h = ImGui::GetFrameHeight () + 4 * k;
        ImGui::SetNextWindowPos (ImVec2 (vp->Pos.x, vp->Pos.y + vp->Size.y - h));
        ImGui::SetNextWindowSize (ImVec2 (vp->Size.x, h));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.chrome);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (8 * k, 2 * k));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin ("##status", NULL, flags);
        const engine::DocState* d = session_.active ();
        if (d != NULL)
        {
            float pct = zoom_ * 100.0f;
            ImGui::SetNextItemWidth (64 * k);
            if (ImGui::DragFloat ("##zoom", &pct, 0.5f, 2.0f, 3200.0f, "%.2f%%",
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)
                && zoom_ > 0.0f)
            {
                zoom_by (pct / 100.0f / zoom_, canvas_center_x_, canvas_center_y_);
            }
            ImGui::SameLine (0, 12 * k);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (theme_.text_dim), "%s", status_info (*d->doc).c_str ());
            ImGui::SameLine (0, 4 * k);
            ImVec2 p  = ImGui::GetCursorScreenPos ();
            float box = ImGui::GetFrameHeight ();
            if (ImGui::InvisibleButton ("##statusinfo", ImVec2 (box, box)))
            {
                ImGui::OpenPopup ("statusmenu");
            }
            if (ImGui::IsItemHovered ())
            {
                ImGui::GetWindowDrawList ()->AddRectFilled (
                    p, ImVec2 (p.x + box, p.y + box), theme_.hover, theme_.radius_sm);
            }
            float g = 11 * k;
            draw_icon (ImGui::GetWindowDrawList (), &icons_, "chevron-right",
                ImVec2 (p.x + (box - g) * 0.5f, p.y + (box - g) * 0.5f), g, theme_.text_dim);
            if (ImGui::BeginPopup ("statusmenu"))
            {
                for (size_t i = 0; i < sizeof STATUS_INFO / sizeof STATUS_INFO[0]; i++)
                {
                    if (ImGui::MenuItem (STATUS_INFO[i][1], NULL, status_info_ == STATUS_INFO[i][0]))
                    {
                        status_info_ = STATUS_INFO[i][0];
                    }
                }
                ImGui::EndPopup ();
            }
        }
        else
        {
            ImGui::AlignTextToFramePadding ();
            ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (theme_.text_dim), "No document");
        }
        if (!status_.empty ())
        {
            ImGui::SameLine (0, 8 * k);
            ImVec2 p = ImGui::GetCursorScreenPos ();
            float fh = ImGui::GetFrameHeight ();
            ImGui::GetWindowDrawList ()->AddLine (
                ImVec2 (p.x, p.y + 3 * k), ImVec2 (p.x, p.y + fh - 3 * k), theme_.separator);
            ImGui::Dummy (ImVec2 (1, fh));
            ImGui::SameLine (0, 8 * k);
            ImGui::AlignTextToFramePadding ();
            ImGui::TextColored (ImGui::ColorConvertU32ToFloat4 (status_is_error_ ? theme_.warning : theme_.text_faint),
                "%s", status_.c_str ());
        }
        ImGui::End ();
        ImGui::PopStyleVar (2);
        ImGui::PopStyleColor ();
    }

    void App::clear_preview ()
    {
        if (!preview_doc_)
        {
            return;
        }
        preview_doc_.reset ();
        preview_serial_++;
    }

    /* The dialog's adjustment applied to a copy of the document (tiles are shared, so only the
       tiles it changes are copied); the real document changes only on OK. */
    void App::preview_destructive ()
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        doc::Adjustment adj;
        engine::Error err;
        if (!engine::adjustment_from_params (dialog_.kind, dialog_.params, d->doc->mode, NULL, &adj, &err))
        {
            set_error (err.message);
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        doc::Layer* l                       = copy->layer_mut (d->active_layer);
        raster::Surface* surf               = l != NULL ? l->surface_mut () : NULL;
        if (surf == NULL)
        {
            return;
        }
        engine::adjust_surface (surf, adj, copy->has_selection ? &copy->selection : NULL, copy->mode, session_.cmyk);
        preview_doc_ = copy;
        preview_serial_++;
    }

    /* The Properties panel's values on a copy of the document while a slider is dragged. */
    void App::preview_properties (const doc::Layer& layer)
    {
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            return;
        }
        doc::Adjustment adj;
        engine::Error err;
        if (!engine::adjustment_from_params (props_.kind, props_.params, d->doc->mode, &layer.adjustment, &adj, &err))
        {
            return;
        }
        std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
        doc::Layer* l                       = copy->layer_mut (layer.id);
        if (l == NULL)
        {
            return;
        }
        l->adjustment = adj;
        preview_doc_  = copy;
        preview_serial_++;
    }

    void App::open_adjust_dialog (doc::AdjustmentKind kind, const char* command)
    {
        std::string why;
        const engine::CommandSpec* spec = session_.find_command (command);
        if (spec == NULL)
        {
            return;
        }
        if (!session_.command_enabled (*spec, &why))
        {
            set_error (why);
            return;
        }
        adjust_editor_init (&dialog_, doc::Adjustment::make (kind));
        dialog_command_     = command;
        std::string label   = spec->label;
        size_t dots         = label.find ("…");
        dialog_title_       = (dots == std::string::npos ? label : label.substr (0, dots)) + "###adjust";
        show_adjust_dialog_ = true;
        if (adjust_dialog_preview_)
        {
            preview_destructive ();
        }
    }

    void App::adjust_dialog ()
    {
        if (show_adjust_dialog_)
        {
            ImGui::OpenPopup (dialog_title_.c_str ());
            show_adjust_dialog_ = false;
        }
        ImGui::SetNextWindowSize (ImVec2 (380.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal (dialog_title_.c_str (), NULL, 0))
        {
            return;
        }
        bool changed, finished;
        adjust_editor_draw (&dialog_, &changed, &finished);
        if (changed && adjust_dialog_preview_)
        {
            preview_destructive ();
        }
        ImGui::Separator ();
        if (ImGui::Checkbox ("Preview", &adjust_dialog_preview_))
        {
            if (adjust_dialog_preview_)
            {
                preview_destructive ();
            }
            else
            {
                clear_preview ();
            }
        }
        if (ImGui::Button ("OK"))
        {
            clear_preview ();
            run_command (dialog_command_.c_str (), dialog_.params);
            ImGui::CloseCurrentPopup ();
        }
        ImGui::SameLine ();
        if (ImGui::Button ("Cancel") || ImGui::IsKeyPressed (ImGuiKey_Escape))
        {
            clear_preview ();
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

    /* Settings of the active adjustment layer. Dragging previews; releasing commits one
       layer.setAdjustment step. */
    /* Photoshop's default arrangement on first run (no saved layout): Tools on the left, the canvas
       in the middle, Properties, Layers and History down the right. A layout the user saved replaces it. */
    void default_layout (ImGuiID dock, ImVec2 size, bool reset)
    {
        if (reset)
        {
            ImGui::DockBuilderRemoveNode (dock);
        }
        else if (ImGui::DockBuilderGetNode (dock) != NULL)
        {
            return;
        }
        ImGui::DockBuilderAddNode (dock, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize (dock, size);
        ImGuiID center = dock;
        float right_w  = size.x > 1200.0f ? 300.0f / size.x : 0.25f;
        ImGuiID right  = ImGui::DockBuilderSplitNode (center, ImGuiDir_Right, right_w, NULL, &center);
        /* Photoshop's right column: Color / Swatches, Properties / History, Layers / Channels. */
        ImGuiID rest = 0, bottom = 0;
        ImGuiID top    = ImGui::DockBuilderSplitNode (right, ImGuiDir_Up, 0.26f, NULL, &rest);
        ImGuiID middle = ImGui::DockBuilderSplitNode (rest, ImGuiDir_Up, 0.42f, NULL, &bottom);
        ImGui::DockBuilderDockWindow ("###canvas", center);
        ImGui::DockBuilderDockWindow ("Color", top);
        ImGui::DockBuilderDockWindow ("Swatches", top);
        ImGui::DockBuilderDockWindow ("Histogram", top);
        ImGui::DockBuilderDockWindow ("Navigator", top);
        ImGui::DockBuilderDockWindow ("Properties", middle);
        ImGui::DockBuilderDockWindow ("History", middle);
        ImGui::DockBuilderDockWindow ("Actions", middle);
        ImGui::DockBuilderDockWindow ("Info", middle);
        ImGui::DockBuilderDockWindow ("Character", middle);
        ImGui::DockBuilderDockWindow ("Paragraph", middle);
        ImGui::DockBuilderDockWindow ("Layers", bottom);
        ImGui::DockBuilderDockWindow ("Channels", bottom);
        ImGui::DockBuilderDockWindow ("Paths", bottom);
        ImGui::DockBuilderFinish (dock);
    }

    void App::frame ()
    {
        if (theme_dirty_)
        {
            theme_ = tokens_for (theme_kind_);
            apply_theme (theme_, ui_scale_);
            ImGui::GetStyle ().FontScaleMain = ui_scale_;
            theme_dirty_                     = false;
        }
        process_dialogs ();
        /* A transform box belongs to the document state it opened on. */
        const engine::DocState* cur = session_.active ();
        if (xf_.active && (cur == NULL || cur->doc->id != xf_.doc || cur->revision != xf_.revision))
        {
            cancel_free_transform ();
        }
        if (pw_.active && (cur == NULL || cur->doc->id != pw_.doc || cur->revision != pw_.revision))
        {
            cancel_warp_mode ();
        }
        if (vw_.active && (cur == NULL || cur->doc->id != vw_.doc || cur->revision != vw_.revision))
        {
            cancel_warp_mode ();
        }
        sam_frame ();
        shortcuts ();
        menu_bar ();
        /* Options bar along the top, toolbar down the left, status bar along the bottom; the docked
           panels and the canvas fill the rest. */
        ImGuiViewport* vp = ImGui::GetMainViewport ();
        float k           = ui_scale_;
        float opt_h = roundf (36.0f * k), bar_w = roundf (40.0f * k), status_h = ImGui::GetFrameHeight () + 4 * k;
        ImVec2 top = vp->WorkPos;
        if (panels_hidden_)
        {
            canvas_window ();
            frame_dialogs ();
            status_bar ();
            return;
        }
        options_bar (top, ImVec2 (vp->WorkSize.x, opt_h));
        ImVec2 body (top.x, top.y + opt_h + 1);
        float body_h = vp->WorkSize.y - opt_h - 1 - status_h;
        if (sam_.active)
        {
            sam_toolbar (body, ImVec2 (bar_w, body_h));
        }
        else
        {
            toolbar (body, ImVec2 (bar_w, body_h));
        }
        ImVec2 size (vp->WorkSize.x - bar_w - 1, body_h);
        ImGui::SetNextWindowPos (ImVec2 (body.x + bar_w + 1, body.y));
        ImGui::SetNextWindowSize (size);
        ImGuiWindowFlags host = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                              | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
                              | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0, 0));
        ImGui::PushStyleColor (ImGuiCol_WindowBg, theme_.dock);
        ImGui::Begin ("##dockhost", NULL, host);
        ImGui::PopStyleColor ();
        ImGui::PopStyleVar ();
        ImGuiID dock = ImGui::GetID ("MainDock");
        main_dock_   = dock;
        default_layout (dock, size, reset_layout_);
        reset_layout_ = false;
        ImGui::DockSpace (dock, ImVec2 (0, 0), ImGuiDockNodeFlags_NoWindowMenuButton);
        ImGui::End ();
        canvas_window ();
        color_window ();
        swatches_window ();
        histogram_window ();
        character_window ();
        paragraph_window ();
        navigator_window ();
        info_window ();
        properties_window ();
        layers_window ();
        channels_window ();
        paths_window ();
        history_window ();
        actions_window ();
        /* Select and Mask's Properties panel covers the panels down the right. */
        if (sam_.active)
        {
            float pw = roundf (320.0f * k);
            sam_properties (ImVec2 (vp->WorkPos.x + vp->WorkSize.x - pw, body.y), ImVec2 (pw, body_h));
        }
        frame_dialogs ();
        status_bar ();
    }

    /* The dialogs, whichever layout is showing. */
    void App::frame_dialogs ()
    {
        adjust_dialog ();
        modify_dialog ();
        image_size_dialog ();
        canvas_size_dialog ();
        color_range_dialog ();
        fill_dialog ();
        filter_dialog ();
        apply_image_dialog ();
        color_table_dialog ();
        fill_editor_dialog ();
        smart_filter_blending_dialog ();
        batch_dialog ();
        stroke_dialog ();
        fade_dialog ();
        layer_style_dialog ();
        save_selection_dialog ();
        load_selection_dialog ();
        color_picker_dialog ();
        new_guide_dialog ();
        guide_layout_dialog ();
        grid_options_dialog ();
        new_document_dialog ();
        close_prompt ();
    }

} // namespace ui
