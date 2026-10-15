#pragma once

#include "compose/compose.h"
#include "compose/masks.h"
#include "engine/retouch.h"
#include "engine/session.h"
#include "paint/paint.h"
#include "text/layout.h"
#include "ui/adjust_ui.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include <SDL3/SDL.h>
#include <map>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string>
#include <vector>

namespace ui
{

    enum class Tool
    {
        Move,
        RectMarquee,
        EllipseMarquee,
        Lasso,
        PolygonLasso,
        MagicWand,
        Gradient,
        PaintBucket,
        Crop,
        Brush,
        Pencil,
        Eraser,
        CloneStamp,
        SpotHealing,
        HealingBrush,
        HistoryBrush,
        Dodge,
        Burn,
        Sponge,
        Blur,
        Sharpen,
        Smudge,
        Hand,
        Zoom,
        MagicEraser,
        Eyedropper,
        QuickSelection,
        Type,
        Pen,
        PathSelect,
        DirectSelect,
        RectShape,
        EllipseShape,
        PolygonShape,
        LineShape,
        ColorReplacement,
        MixerBrush,
        PatternStamp,
        ArtHistory,
        BackgroundEraser,
        Remove,
        Patch,
        ContentAwareMove,
        RedEye,
    };

    const int TOOL_COUNT = (int) Tool::RedEye + 1;
    const int SLOT_COUNT = 20;

    enum class DialogKind
    {
        Open,
        SaveAs,
        Place,
        Replace,
        Export,
        BatchSource,
        BatchDest,
    };

    /* What the filter dialog's OK does: run the filter, change a smart filter, carry on a paused
       action, or change an action step's settings. */
    const int FILTER_RUN           = 0;
    const int FILTER_SMART_EDIT    = 1;
    const int FILTER_ACTION_RESUME = 2;
    const int FILTER_STEP_EDIT     = 3;

    enum class TransformDrag
    {
        None,
        Corner,
        Edge,
        Move,
        Rotate,
    };

    struct FreeTransform
    {
        bool active;
        doc::LayerId layer;
        bool mask;
        double rect[4];
        double quad[4][2];
        double start[4][2];
        TransformDrag drag;
        int handle;
        double press_x;
        double press_y;
        bool dirty;
        int interp;
        doc::DocId doc;
        uint64_t revision;
        int mode;
        double warp[16][2];
        double warp_start[16][2];
        int warp_style;
        float bend;
        float hdist;
        float vdist;
        bool warp_vertical;
        bool warp_custom;
        int cas_protect;
        bool cas_skin;
    };

    /* Free Transform's modes: Edit > Transform > Scale … Warp, and Content-Aware Scale. */
    const int XF_FREE        = 0;
    const int XF_SCALE       = 1;
    const int XF_ROTATE      = 2;
    const int XF_SKEW        = 3;
    const int XF_DISTORT     = 4;
    const int XF_PERSPECTIVE = 5;
    const int XF_WARP        = 6;
    const int XF_CAS         = 7;

    /* Edit > Puppet Warp: pins as [x, y, toX, toY] runs. */
    struct PuppetWarp
    {
        bool active;
        doc::LayerId layer;
        doc::DocId doc;
        uint64_t revision;
        double rect[4];
        std::vector<double> pins;
        int drag;
        double press_x;
        double press_y;
        double grab_x;
        double grab_y;
        int mode;
        bool show_mesh;
    };

    /* Edit > Perspective Warp: the plane laid out as a quad, then that quad pushed. */
    struct PerspectiveWarp
    {
        bool active;
        doc::LayerId layer;
        doc::DocId doc;
        uint64_t revision;
        bool warping;
        double from[4][2];
        double to[4][2];
        int drag;
        double press_x;
        double press_y;
        double grab_x;
        double grab_y;
    };

    /* Image > Apply Image / Calculations: a source row (document index, layer or 0 for merged,
       channel choice) and the dialog's settings. */
    struct CalcSource
    {
        int doc;
        doc::LayerId layer;
        int channel;
        bool invert;
    };

    /* Select and Mask's workspace: its tool and view, the refinement settings, the Refine Edge
       Brush strokes (as the command takes them) and the stroke being painted. */
    const int SAM_VIEW_COUNT = 7;

    struct SelectMask
    {
        bool active;
        bool used;
        bool remember;
        doc::DocId doc;
        size_t history_at;
        uint64_t seen_revision;
        uint64_t seen_serial;
        Tool saved_tool;
        int tool;
        int view;
        float opacity[SAM_VIEW_COUNT];
        bool show_edge;
        bool show_original;
        bool hide_view;
        float radius;
        bool smart_radius;
        float smooth;
        float feather;
        float contrast;
        float shift_edge;
        bool decontaminate;
        float amount;
        int output;
        float refine_size;
        bool refine_subtract;
        float brush_size;
        bool brush_subtract;
        engine::Value strokes;
        std::vector<float> paint_points;
        bool painting;
        bool paint_subtract;
        bool dirty;
        bool editing;
    };

    /* A fill layer's dialog: the layer, its settings being edited, the chosen gradient stop. */
    struct FillEditor
    {
        bool open;
        bool fresh;
        doc::LayerId layer;
        doc::FillLayer work;
        int stop;
        int drag;
        int pattern_index;
        bool dirty;
    };

    struct ApplyImageDialog
    {
        bool open;
        bool used;
        bool calc;
        CalcSource src[3];
        bool use_mask;
        int blend;
        float opacity;
        float scale;
        float offset;
        bool preserve;
        int result;
        bool preview;
        bool dirty;
    };

    /* The selection edge's runs over one tile, and the tiles they were found from. */
    struct SelEdgeTile
    {
        const raster::Tile* self;
        const raster::Tile* top;
        const raster::Tile* left;
        uint8_t def;
        std::vector<int32_t> h;
        std::vector<int32_t> v;
    };

    struct DialogResult
    {
        DialogKind kind;
        std::string path;
        std::string error;
        int filter;
    };

    class App
    {
    public:
        App (engine::Session& session, SDL_Window* window);
        ~App ();

        void frame ();
        bool quit_requested () const;
        bool waiting_on_dialog () const;
        void run_command (const char* id, const engine::Value& params);
        void run_ui_command (const char* id, const engine::Value& params);
        void open_file (const char* path);
        void save_file (const char* path);
        void dialog_finished (const DialogResult& result);
        bool begin_file_dialog ();
        DialogKind pending_open_kind () const;
        DialogKind pending_folder_kind () const;
        DialogKind pending_save_kind () const;
        void clipboard_changed (bool owner);
        const void* clipboard_data (const char* mime, size_t* size);
        void set_theme (ThemeKind kind);
        void* begin_swatch_settings ();
        void settings_line (const char* section, const char* line);
        void write_settings (const char* type, ImGuiTextBuffer* buf);
        ThemeKind theme_kind () const;

        App (const App&)            = delete;
        App& operator= (const App&) = delete;

    private:
        void menu_bar ();
        void shortcuts ();
        void toolbar (ImVec2 pos, ImVec2 size);
        void color_chips ();
        void options_bar (ImVec2 pos, ImVec2 size);
        void brush_options ();
        void brush_picker (bool pencil, bool* active, bool* done);
        void move_options ();
        void select_tool (Tool t);
        bool icon_button (const char* id, const char* icon, float box, bool selected, const char* tooltip);
        void canvas_window ();
        void document_tabs ();
        void close_prompt ();
        void canvas_input (float ox, float oy);
        void zoom_by (float factor, float anchor_x, float anchor_y);
        void begin_stroke (float mx, float my);
        void continue_stroke (float mx, float my);
        void end_stroke ();
        void move_preview (int32_t dx, int32_t dy);
        void begin_free_transform ();
        void begin_transform_mode (int mode);
        void set_warp_style (int style);
        void warp_input (float ox, float oy);
        void warp_overlay (float ox, float oy);
        engine::Value transform_params (bool preview);
        void free_transform_input (float ox, float oy);
        void free_transform_overlay (float ox, float oy);
        void free_transform_options ();
        void free_transform_preview ();
        void commit_free_transform ();
        void cancel_free_transform ();
        bool begin_puppet_warp ();
        bool begin_perspective_warp ();
        bool warp_mode_active () const;
        void commit_warp_mode ();
        void cancel_warp_mode ();
        void warp_mode_preview ();
        void puppet_input (float ox, float oy);
        void puppet_overlay (float ox, float oy);
        void perspective_input (float ox, float oy);
        void perspective_overlay (float ox, float oy);
        void warp_mode_options ();
        bool select_tool_input (float ox, float oy);
        void select_tool_overlay (float ox, float oy);
        void selection_cursor ();
        void select_options ();
        void finish_lasso (bool polygon);
        void modify_dialog ();
        bool fill_tool_input (float ox, float oy);
        void fill_tool_overlay (float ox, float oy);
        void fill_tool_options ();
        bool crop_tool_input (float ox, float oy);
        void crop_tool_overlay (float ox, float oy);
        void crop_tool_options ();
        void commit_crop ();
        void image_size_dialog ();
        bool retouch_tool_input (float ox, float oy);
        void retouch_tool_overlay (float ox, float oy);
        void retouch_options ();
        bool pattern_combo (const char* id, int* index, float width);
        bool heal_tool_input (float ox, float oy);
        void heal_tool_overlay (float ox, float oy);
        void heal_options ();
        engine::Value retouch_params ();
        void end_retouch ();
        void canvas_size_dialog ();
        void color_range_dialog ();
        void layers_window ();
        void layer_rows (const doc::Document& d, const std::vector<doc::Layer>& layers, int depth);
        void layer_row (const doc::Document& d, const doc::Layer& l, int depth);
        void layer_context_menu ();
        void layer_options (const doc::Document& d);
        void layer_buttons (const doc::Document& d);
        unsigned int thumbnail (const doc::Document& d, const doc::Layer& l, bool mask, int px, int* tw, int* th);
        void thumb_box (const doc::Document& d, const doc::Layer& l, bool mask, ImVec2 a, float size, bool target);
        void command_item (const char* id, const char* label);
        bool mask_targeted () const;
        void history_window ();
        void channels_window ();
        void histogram_window ();
        void navigator_window ();
        void info_window ();
        void channel_filter (compose::Buffer* b);
        void update_small_preview (const doc::Document& d, const compose::Buffer& b);
        void color_window ();
        void swatches_window ();
        void color_field (float* color, bool background);
        bool sv_hue_field (ImVec2 fa, float fw, float fh, float strip_w, float* h, float* s, float* v);
        void open_color_picker (bool background);
        void picker_set_rgb (const float rgb[3]);
        void color_picker_dialog ();
        bool eyedropper_input (float mx, float my);
        void eyedropper_options ();
        bool sample_at (float mx, float my, float out[4]);
        void fill_dialog ();
        bool open_filter_dialog (const char* id);
        engine::Value filter_params ();
        void filter_dialog ();
        void open_apply_image (bool calculations);
        engine::Value calc_source_spec (int i);
        engine::Value apply_image_params ();
        bool calc_source_rows (int i, const char* title);
        void apply_image_dialog ();
        void open_color_table ();
        void open_fill_editor (doc::LayerId id, bool fresh);
        bool gradient_stops_editor (doc::FillLayer* f);
        void fill_editor_dialog ();
        bool blend_if_bar (int bar, const char* label, float black[2], float white[2], int channel);
        void blending_options_page ();
        void color_table_dialog ();
        void open_select_and_mask ();
        void select_and_mask_button ();
        void close_select_and_mask (bool ok);
        engine::Value sam_params (bool preview);
        void sam_preview ();
        void sam_frame ();
        void sam_toolbar (ImVec2 pos, ImVec2 size);
        void sam_pick_tool (int t);
        void sam_options ();
        void sam_properties (ImVec2 pos, ImVec2 size);
        void sam_input (float ox, float oy);
        void sam_overlay (float ox, float oy);
        void sam_keys ();
        void quick_update ();
        bool edit_smart_filter (doc::LayerId layer, int index);
        bool load_filter_dialog (const char* id, const engine::Value& p);
        bool has_filter_dialog (const char* id) const;
        void sync_actions ();
        void play_action (int set, int action, int from);
        void actions_window ();
        void action_name_dialog ();
        void batch_dialog ();
        void smart_filter_rows (const doc::Layer& l, int depth);
        void smart_filter_blending_dialog ();
        void stroke_dialog ();
        void fade_dialog ();
        void save_selection_dialog ();
        void load_selection_dialog ();
        void update_alpha_thumbnails (const doc::Document& d, uint64_t revision);
        void show_color_channel (int r);
        void toggle_quick_mask ();
        void frame_dialogs ();
        bool guides_shown () const;
        void rulers (ImVec2 full, ImVec2 full_size, float ox, float oy);
        void view_overlays (float ox, float oy, ImVec2 origin, ImVec2 avail);
        int guide_at (float ox, float oy);
        bool guide_input (float ox, float oy, ImVec2 origin, ImVec2 avail);
        void snap_point (float* x, float* y, bool sx, bool sy, bool from_guide);
        void snap_box_move (const float box[4], float* dx, float* dy);
        void proof_filter (compose::Buffer* b);
        void zoom_to (float z);
        void fit_layers ();
        void set_screen_mode (int mode);
        void toggle_panels ();
        void toggle_view (bool* flag);
        bool view_command (const char* id);
        void view_menu ();
        void view_shortcuts ();
        void new_guide_dialog ();
        void guide_layout_dialog ();
        void grid_options_dialog ();
        void register_type_fonts ();
        doc::LayerId type_layer_at (float x, float y);
        void begin_type_edit (doc::LayerId id, size_t caret, bool is_new);
        void type_changed ();
        void commit_type_edit ();
        void cancel_type_edit ();
        void type_insert (const std::string& s);
        void type_keys ();
        bool type_tool_input (float ox, float oy);
        void type_overlay (float ox, float oy);
        doc::CharStyle type_shown_style (doc::ParaStyle* para, doc::AntiAlias* aa);
        void type_apply_style (const engine::Value& patch);
        void type_apply_para (const engine::Value& patch);
        float type_value (const char* id, float shown);
        void type_drag (const char* id, const char* key, float v, bool changed, bool para);
        void font_combos (const doc::CharStyle& st, float fam_w, float style_w, bool stacked);
        void type_options ();
        void character_window ();
        void paragraph_window ();
        void open_layer_style (const doc::FxKind* kind, int index);
        bool effect_settings (doc::Effect* e);
        void layer_style_dialog ();
        void effect_rows (const doc::Layer& l, int depth);
        bool path_target (engine::Value* target, doc::Path* path);
        void sync_pen ();
        engine::Value shape_style_params ();
        void commit_pen (const char* label);
        void end_pen_drawing ();
        bool path_tool_input (float ox, float oy);
        void pen_press (double mx, double my, double r, bool alt);
        void select_press (Tool t, double mx, double my, double r, bool shift);
        void pen_drag_to (double mx, double my, double dx, double dy, bool alt);
        void marquee_select (Tool t, bool shift);
        doc::LayerId shape_layer_at (double x, double y, double r);
        void shape_tool_input (double mx, double my, bool activated, bool active, bool released, bool shift, bool alt);
        void shape_rect (double r[4]);
        void shape_live (const double r[4], doc::LiveShape* live);
        bool path_keys ();
        void path_tool_overlay (float ox, float oy);
        void shape_style_options ();
        void path_options ();
        void path_thumb (const doc::Document& d, const doc::Path& p, ImVec2 a, float size);
        void paths_window ();
        void shape_properties (const doc::Layer& l);
        void vector_mask_properties (const doc::Layer& l);
        void new_document_dialog ();
        void status_bar ();
        std::string status_info (const doc::Document& d);
        void process_dialogs ();
        void update_canvas_texture ();
        void properties_window ();
        void mask_properties (const doc::Layer& l);
        void props_header (const char* icon, const char* title);
        void layer_transform_properties (const doc::Document& d, const doc::Layer& l);
        void document_properties (const doc::Document& d);
        void adjustment_properties (const engine::DocState& d, const doc::Layer& l);
        void adjust_dialog ();
        void open_adjust_dialog (doc::AdjustmentKind kind, const char* command);
        void preview_destructive ();
        void preview_properties (const doc::Layer& layer);
        void clear_preview ();
        void update_selection_texture ();
        void selection_outline (ImDrawList* dl, ImVec2 pos, ImVec2 clip0, ImVec2 clip1);
        void set_error (const std::string& message);
        void publish_clipboard ();
        void import_os_clipboard ();
        engine::Value paste_params ();

        engine::Session& session_;
        SDL_Window* window_;
        ThemeKind theme_kind_;
        bool theme_dirty_;
        Tokens theme_;
        Fonts fonts_;
        float ui_scale_;
        bool quit_;
        std::string status_;
        bool status_is_error_;
        std::string status_info_;

        std::mutex dialog_lock_;
        std::vector<DialogResult> dialog_results_;
        int file_dialogs_open_;
        double file_dialog_since_;
        bool time_next_render_;

        unsigned int texture_;
        uint64_t texture_doc_;
        uint64_t texture_revision_;
        int texture_width_;
        int texture_height_;
        bool selection_built_;
        uint64_t selection_preview_serial_;
        std::vector<int32_t> sel_hruns_;
        std::map<geom::TileCoord, SelEdgeTile> sel_edges_;
        raster::Surface sel_edge_surface_;
        int32_t sel_edge_w_;
        int32_t sel_edge_h_;
        std::vector<int32_t> sel_vruns_;
        uint64_t selection_doc_;
        uint64_t selection_revision_;
        bool selection_visible_;
        float zoom_;
        bool opacity_editing_;
        float opacity_edit_;
        bool fill_editing_;
        float fill_edit_;
        struct Thumb
        {
            doc::LayerId id;
            bool mask;
            raster::Surface src;
            int w;
            int h;
            unsigned int tex;
        };
        std::vector<Thumb> thumbs_;

        float pan_x_;
        float pan_y_;
        float canvas_center_x_;
        float canvas_center_y_;
        float canvas_avail_x_;
        float canvas_avail_y_;
        bool panning_;
        bool fit_view_;
        float fit_avail_x_;
        float fit_avail_y_;

        bool stroking_;
        std::string stroke_command_;
        engine::Value stroke_params_;
        bool stroke_mask_;
        std::vector<paint::StrokePoint> stroke_points_;
        std::unique_ptr<paint::StrokeRenderer> stroke_renderer_;
        std::shared_ptr<doc::Document> stroke_doc_;
        raster::Surface stroke_pre_;
        doc::LayerId stroke_layer_;
        bool stroke_lock_;
        uint64_t stroke_seed_;
        uint64_t stroke_revision_;
        uint64_t stroke_counter_;

        bool moving_;
        float move_box_[4];
        bool move_has_box_;
        float move_x0_;
        float move_y0_;
        int32_t move_dx_;
        int32_t move_dy_;

        paint::BrushSettings brush_edit_;
        bool brush_editing_;
        bool pencil_auto_erase_;
        int eraser_mode_;

        doc::LayerId mask_target_layer_;
        doc::LayerId rename_layer_;
        bool rename_focus_;
        size_t tab_active_;
        int close_request_;
        DialogKind pending_dialog_;
        bool color_bg_active_;
        float color_hue_;
        float color_sat_;
        std::string selected_swatch_;
        std::string rename_swatch_;
        char swatch_buf_[128];
        int swatch_section_;
        bool channel_shown_[4];
        bool channel_view_dirty_;
        std::vector<uint8_t> small_;
        int small_w_;
        int small_h_;
        uint64_t small_serial_;
        unsigned int channel_tex_[4];
        uint64_t channel_tex_serial_;
        int alpha_view_;
        bool alpha_overlay_;
        std::vector<unsigned int> alpha_tex_;
        uint64_t alpha_tex_doc_;
        uint64_t alpha_tex_revision_;
        int rename_channel_;
        bool picker_open_;
        bool picker_bg_;
        float picker_hsv_[3];
        float picker_old_[3];
        int eyedrop_size_;
        int eyedrop_layers_;
        bool eyedropping_;
        bool eyedrop_bg_;
        bool picker_focus_;
        bool quick_mask_;
        bool type_active_;
        bool view_rulers_;
        bool view_extras_;
        bool view_guides_;
        bool view_grid_;
        bool view_sel_edges_;
        bool view_layer_edges_;
        bool view_pixel_grid_;
        bool view_snap_;
        bool snap_guides_;
        bool snap_grid_;
        bool snap_layers_;
        bool snap_bounds_;
        bool view_lock_guides_;
        bool view_proof_;
        bool view_gamut_;
        int screen_mode_;
        bool panels_hidden_;
        bool redock_canvas_;
        float grid_spacing_;
        int grid_subdivisions_;
        int guide_drag_;
        bool guide_drag_vertical_;
        float guide_drag_pos_;
        float ruler_px_;
        doc::LayerId snap_skip_layer_;
        unsigned int main_dock_;
        bool show_new_guide_;
        bool show_guide_layout_;
        bool show_grid_options_;
        int guide_dialog_vertical_;
        float guide_dialog_pos_;
        int layout_cols_;
        int layout_rows_;
        float layout_col_gutter_;
        float layout_row_gutter_;
        float layout_margin_[4];
        bool layout_clear_;
        bool type_new_;
        doc::LayerId type_layer_;
        doc::DocId type_doc_;
        doc::TextLayer type_text_;
        doc::TextLayer type_original_;
        size_t type_caret_;
        size_t type_anchor_;
        bool type_selecting_;
        int type_handle_;
        text::Layout type_layout_;
        double type_blink_;
        bool type_dragging_;
        float type_drag_[4];
        doc::CharStyle type_style_;
        doc::ParaStyle type_para_;
        doc::AntiAlias type_aa_;
        std::vector<std::string> type_families_;
        char type_filter_[64];
        unsigned int type_drag_id_;
        float type_drag_value_;
        bool show_style_;
        doc::LayerId style_layer_;
        doc::LayerStyle style_work_;
        int style_sel_;
        bool style_preview_;
        bool style_dirty_;
        float style_light_[2];
        std::vector<doc::LayerId> fx_collapsed_;
        bool stroke_quick_;
        bool show_fill_;
        int fill_contents_;
        float fill_color_[3];
        int fill_mode_;
        float fill_dialog_opacity_;
        bool fill_preserve_;
        bool show_stroke_;
        int stroke_width_;
        float stroke_color_[3];
        int stroke_location_;
        int stroke_mode_;
        float stroke_opacity_;
        bool stroke_preserve_;
        bool show_save_selection_;
        int save_channel_;
        int save_operation_;
        char channel_name_[64];
        bool show_load_selection_;
        int load_channel_;
        bool load_invert_;
        int load_operation_;
        bool show_filter_;
        bool show_fade_;
        float fade_opacity_;
        int fade_mode_;
        std::string last_filter_;
        engine::Value last_filter_params_;
        bool info_over_;
        int32_t info_x_;
        int32_t info_y_;
        int32_t info_sampled_[2];
        uint64_t info_doc_;
        uint64_t info_revision_;
        float info_px_[4];
        int filter_dialog_;
        float filter_values_[48];
        int filter_mode_;
        bool actions_loaded_;
        size_t actions_hash_;
        int act_sel_[3];
        int act_ref_[3];
        int act_naming_;
        char act_name_[128];
        bool show_batch_;
        int batch_set_;
        int batch_action_;
        int batch_format_;
        std::string batch_source_;
        std::string batch_dest_;
        doc::LayerId filter_edit_layer_;
        int filter_edit_index_;
        doc::LayerId blend_edit_layer_;
        int blend_edit_index_;
        color::BlendMode blend_edit_mode_;
        float blend_edit_opacity_;
        bool filter_preview_;
        bool filter_preview_dirty_;
        char rename_buf_[256];

        Tool tool_;
        Tool slot_last_[SLOT_COUNT];
        IconCache icons_;
        int selection_mode_;
        Tool brush_tool_;
        Tool lasso_tool_;
        std::vector<float> lasso_points_;
        float quick_size_;
        bool quick_active_;
        bool quick_all_layers_;
        std::vector<float> quick_points_;
        size_t quick_sent_;
        const char* quick_mode_now_;
        bool lasso_active_;
        float sel_feather_;
        bool sel_anti_alias_;
        int wand_tolerance_;
        bool wand_contiguous_;
        bool wand_all_layers_;
        std::string modify_command_;
        float modify_radius_;
        bool modify_at_bounds_;
        bool show_color_range_;
        int range_select_;
        float range_fuzziness_;
        bool range_invert_;
        Tool fill_tool_;
        bool gradient_dragging_;
        float gradient_from_[2];
        float gradient_to_[2];
        int gradient_style_;
        bool gradient_reverse_;
        bool gradient_dither_;
        float fill_opacity_;
        int bucket_tolerance_;
        bool bucket_contiguous_;
        bool bucket_all_layers_;
        bool crop_active_;
        float crop_rect_[4];
        int crop_drag_;
        float crop_press_[2];
        float crop_start_[4];
        bool crop_delete_;
        bool show_image_size_;
        int size_width_;
        int size_height_;
        float size_resolution_;
        bool size_constrain_;
        int size_method_;
        bool show_canvas_size_;
        int canvas_width_;
        int canvas_height_;
        bool canvas_relative_;
        int canvas_anchor_;
        int canvas_extension_;
        Tool heal_tool_;
        Tool tone_tool_;
        bool retouching_;
        std::string retouch_cmd_;
        engine::Value retouch_params_;
        std::vector<float> retouch_points_;
        std::unique_ptr<engine::LiveRetouch> live_retouch_;
        std::unique_ptr<engine::LiveDab> live_dab_;
        bool has_clone_source_;
        float clone_source_[2];
        bool has_clone_offset_;
        int32_t clone_offset_[2];
        bool clone_aligned_;
        int clone_sample_;
        int tone_range_;
        float tone_exposure_;
        bool tone_protect_;
        bool sponge_saturate_;
        bool sponge_vibrance_;
        float focus_strength_;
        bool focus_all_layers_;
        bool smudge_finger_;
        bool spot_all_layers_;
        int spot_type_;
        bool remove_all_layers_;
        int sample_mode_;
        int limits_mode_;
        int replace_mode_;
        float bg_tolerance_;
        float cr_tolerance_;
        bool bg_protect_;
        float mixer_wet_;
        float mixer_load_;
        float mixer_mix_;
        bool mixer_loaded_;
        bool mixer_all_layers_;
        int pattern_index_;
        bool pattern_aligned_;
        int art_style_;
        float art_area_;
        float art_tolerance_;
        float redeye_pupil_;
        float redeye_darken_;
        bool patch_dest_;
        bool cam_extend_;
        int heal_drag_;
        float heal_press_[2];
        float heal_last_[2];
        std::vector<float> heal_points_;
        int fill_pattern_;
        FreeTransform xf_;
        PuppetWarp pw_;
        PerspectiveWarp vw_;
        ApplyImageDialog apply_image_;
        SelectMask sam_;
        bool show_color_table_;
        FillEditor fill_layer_ed_;
        int bi_drag_bar_;
        int bi_drag_handle_;
        bool bi_drag_split_;
        float bi_drag_from_;
        float bi_drag_start_[4];
        int bi_channel_;
        color::BlendMode style_blend_;
        float style_opacity_;
        float style_fill_;
        bool style_channels_[3];
        doc::BlendIf style_blend_if_;
        std::vector<uint8_t> ct_rgb_;
        int ct_transparent_;
        int ct_edit_;
        bool dragging_;
        float drag_x0_;
        float drag_y0_;
        float drag_x1_;
        float drag_y1_;
        bool reset_layout_;

        std::vector<uint8_t> clip_png_;
        bool clip_png_ready_;
        bool os_clip_new_;
        bool clip_external_;

        std::shared_ptr<const doc::Document> shown_doc_;
        compose::MaskCache mask_cache_;
        geom::Rect stroke_damage_;
        std::shared_ptr<const doc::Document> preview_doc_;
        uint64_t preview_serial_;
        uint64_t texture_preview_serial_;

        bool show_adjust_dialog_;
        bool adjust_dialog_preview_;
        AdjustEditor dialog_;
        std::string dialog_command_;
        std::string dialog_title_;

        AdjustEditor props_;
        doc::LayerId props_layer_;
        uint64_t props_revision_;
        bool props_editing_;
        bool mask_editing_;
        float mask_density_;
        float mask_feather_;
        bool xform_editing_;
        float xform_edit_[4];

        int path_sel_;
        doc::LayerId path_hidden_layer_;
        int pen_mode_;
        int shape_mode_;
        int path_op_;
        doc::Path pen_path_;
        engine::Value pen_target_;
        bool pen_has_target_;
        doc::DocId pen_doc_;
        uint64_t pen_revision_;
        int pen_sub_;
        int pen_drag_;
        Tool pen_drag_tool_;
        int pen_drag_sub_;
        int pen_drag_knot_;
        float pen_press_[2];
        float pen_last_[2];
        bool pen_moved_;
        std::string pen_label_;
        std::vector<uint64_t> path_knots_sel_;
        std::vector<int> path_subs_sel_;
        bool shape_dragging_;
        float shape_from_[2];
        float shape_to_[2];
        bool shape_shift_;
        bool shape_alt_;
        float shape_fill_[3];
        bool shape_fill_on_;
        float shape_stroke_[3];
        bool shape_stroke_on_;
        float shape_stroke_w_;
        float shape_radius_;
        int shape_sides_;
        float shape_star_;
        float shape_weight_;
        bool shape_editing_;
        bool shape_dirty_;
        bool shape_props_editing_;
        float shape_props_[6];
        bool vmask_editing_;
        float vmask_props_[2];
        int rename_path_;

        bool show_new_dialog_;
        int new_width_;
        int new_height_;
        int new_background_;
        char new_name_[128];
    };

} // namespace ui
