#pragma once

#include "color/convert.h"
#include "doc/doc.h"
#include "engine/error.h"
#include "engine/params.h"
#include "ops/history.h"
#include "paint/paint.h"
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

namespace text
{
    class FontDb;
}

namespace engine
{

    class Session;

    typedef bool (*RunFn) (Session& s, const Value& params, Value* result, Error* err);
    typedef bool (*EnabledFn) (const Session& s, std::string* why);

    struct CommandSpec
    {
        const char* id;
        const char* label;
        const char* menu;
        const char* shortcut;
        const char* params;
        EnabledFn enabled;
        RunFn run;
        bool journal;
    };

    struct DocState
    {
        ops::DocRef doc;
        ops::History history;
        doc::LayerId active_layer;
        std::vector<doc::LayerId> selected_layers;
        doc::LayerId layer_anchor;
        doc::LayerId solo_layer;
        std::vector<std::pair<doc::LayerId, bool>> solo_snapshot;
        std::string path;
        uint64_t revision;
        uint64_t saved_revision;
        bool has_damage;
        geom::Rect last_damage;
        doc::DocId smart_parent;
        std::string smart_uuid;

        ops::LayerTarget layer_target () const;
        bool dirty () const;
        std::vector<doc::LayerId> selected () const;
        bool is_selected (doc::LayerId id) const;
        void fix_selection ();
    };

    struct ToolState
    {
        float foreground[4];
        float background[4];
        paint::BrushSettings brush;
    };

    struct LiveStroke
    {
        bool valid;
        std::string command;
        Value params;
        doc::DocId doc;
        uint64_t revision;
        raster::Surface surface;
        geom::Rect bounds;
    };

    /* A Quick Selection stroke while it's dragged: the points so far (x, y pairs; `done` of them
       already cut), what it selected, and the selection it makes with the document's. */
    struct QuickStroke
    {
        bool active;
        doc::DocId doc;
        uint64_t revision;
        float size;
        bool all_layers;
        std::string mode;
        std::vector<float> points;
        size_t done;
        raster::Surface region;
        raster::Surface preview;
    };

    struct Swatch
    {
        std::string name;
        float rgb[3];
    };

    struct SwatchGroup
    {
        std::string name;
        std::vector<Swatch> items;
    };

    std::vector<SwatchGroup> builtin_swatches ();

    struct Clipboard
    {
        bool full;
        raster::Surface surface;
        geom::Rect bounds;
    };

    struct JournalEntry
    {
        std::string id;
        Value params;
    };

    /* Actions: recorded steps (a command and its settings), grouped into actions and sets. */
    struct ActionStep
    {
        std::string command;
        Value params;
        bool enabled;
        bool dialog;
    };

    struct Action
    {
        std::string name;
        std::vector<ActionStep> steps;
    };

    struct ActionSet
    {
        std::string name;
        std::vector<Action> actions;
    };

    /* A pattern (Pattern Stamp, Fill > Pattern): a tile of straight RGBA. */
    struct Pattern
    {
        std::string name;
        uint32_t width;
        uint32_t height;
        std::vector<float> rgba;
    };

    struct ActionRecording
    {
        bool on;
        size_t set;
        size_t action;
    };

    class Session
    {
    public:
        Session ();
        ~Session ();

        bool timing;
        doc::IdSource ids;
        ToolState tools;
        const color::CmykSpace* cmyk;
        std::vector<JournalEntry> journal;
        LiveStroke live_stroke;
        QuickStroke quick;
        Clipboard clipboard;
        std::vector<SwatchGroup> swatches;
        doc::LayerStyle style_clipboard;
        bool has_style_clipboard;
        std::vector<ActionSet> actions;
        ActionRecording recording;
        std::vector<Pattern> patterns;

        text::FontDb* fonts ();

        const std::vector<CommandSpec>& commands () const;
        const CommandSpec* find_command (const char* id) const;
        bool command_enabled (const CommandSpec& spec, std::string* why) const;
        bool execute (const char* id, const Value& params, Value* result, Error* err);
        const char* running () const;

        size_t doc_count () const;
        DocState* doc_at (size_t index);
        const DocState* doc_at (size_t index) const;
        DocState* active ();
        const DocState* active () const;
        bool has_active () const;
        size_t active_index () const;
        void set_active (size_t index);
        size_t add_document (const doc::Document& d, const std::string& path);
        bool close (size_t index);
        void mark_saved (const std::string& path, const std::string& name);
        bool undo ();
        bool redo ();

    private:
        bool doc_id_in_use (doc::DocId id) const;

        std::vector<CommandSpec> commands_;
        const char* running_;
        std::vector<DocState> docs_;
        std::unique_ptr<text::FontDb> fonts_;
        size_t active_;
    };

    bool action_recordable (const char* id);
    Value action_params (const Session& s, const Value& params);

    class Edit
    {
    public:
        explicit Edit (Session& s);

        doc::Document* doc ();
        doc::LayerId* active_layer ();
        void select (const std::vector<doc::LayerId>& ids, doc::LayerId active);
        bool commit (const char* label, Error* err);

        Edit (const Edit&)            = delete;
        Edit& operator= (const Edit&) = delete;

    private:
        Session& session_;
        DocState* state_;
        doc::Document working_;
        doc::LayerId active_;
        std::vector<doc::LayerId> selected_;
    };

    bool always (const Session& s, std::string* why);
    bool has_doc (const Session& s, std::string* why);
    bool quick_stroke_begin (Session& s, float size, const char* mode, bool all_layers, Error* err);
    bool quick_stroke_extend (Session& s, const std::vector<float>& points, Error* err);
    void quick_stroke_cancel (Session& s);
    bool select_and_mask_coverage (
        Session& s, const doc::Document& d, doc::LayerId active, const Value& p, raster::Surface* out, Error* err);
    double timing_now ();
    void timing_log (const Session& s, const char* what, double seconds);
    bool has_layer (const Session& s, std::string* why);
    bool has_selection (const Session& s, std::string* why);
    bool can_undo (const Session& s, std::string* why);
    bool can_redo (const Session& s, std::string* why);

    void register_core_commands (std::vector<CommandSpec>* out);
    void register_layer_commands (std::vector<CommandSpec>* out);
    void register_edit_commands (std::vector<CommandSpec>* out);
    void register_adjustment_commands (std::vector<CommandSpec>* out);
    void register_brush_commands (std::vector<CommandSpec>* out);
    void register_layer_tree_commands (std::vector<CommandSpec>* out);
    void register_mask_commands (std::vector<CommandSpec>* out);
    void register_clipboard_commands (std::vector<CommandSpec>* out);
    void register_transform_commands (std::vector<CommandSpec>* out);
    void register_select_commands (std::vector<CommandSpec>* out);
    void register_fill_tool_commands (std::vector<CommandSpec>* out);
    void register_image_commands (std::vector<CommandSpec>* out);
    void register_retouch_commands (std::vector<CommandSpec>* out);
    void register_swatch_commands (std::vector<CommandSpec>* out);
    void register_channel_commands (std::vector<CommandSpec>* out);
    void register_filter_commands (std::vector<CommandSpec>* out);
    void register_misc_commands (std::vector<CommandSpec>* out);
    void register_sample_commands (std::vector<CommandSpec>* out);
    void register_style_commands (std::vector<CommandSpec>* out);
    void register_text_commands (std::vector<CommandSpec>* out);
    void register_view_commands (std::vector<CommandSpec>* out);
    void register_vector_commands (std::vector<CommandSpec>* out);
    void register_align_commands (std::vector<CommandSpec>* out);
    void register_smart_commands (std::vector<CommandSpec>* out);
    void register_action_commands (std::vector<CommandSpec>* out);
    void register_brush_tool_commands (std::vector<CommandSpec>* out);
    void register_warp_commands (std::vector<CommandSpec>* out);
    void register_tone_commands (std::vector<CommandSpec>* out);
    void register_mode_commands (std::vector<CommandSpec>* out);
    void register_fill_layer_commands (std::vector<CommandSpec>* out);
    void register_comps_commands (std::vector<CommandSpec>* out);
    void ensure_patterns (Session& s);
    bool color_table_preset (const std::string& name, std::vector<uint8_t>* out);
    bool apply_mode_command (
        Session& s, doc::Document* d, doc::LayerId* active, const char* cmd, const Value& p, Error* err);
    void register_apply_image_commands (std::vector<CommandSpec>* out);


} // namespace engine
