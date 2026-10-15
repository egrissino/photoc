#pragma once

#include "doc/doc.h"
#include <deque>
#include <memory>
#include <stddef.h>
#include <string>
#include <vector>

namespace ops
{

    typedef std::shared_ptr<const doc::Document> DocRef;

    struct LayerTarget
    {
        doc::LayerId active;
        std::vector<doc::LayerId> selected;
    };

    struct HistoryState
    {
        std::string label;
        DocRef doc;
        LayerTarget layers;
    };

    class History
    {
    public:
        explicit History (size_t max_states = 50);

        size_t max_states;
        size_t max_bytes;

        void record (const std::string& label, DocRef before, const LayerTarget& layers);
        bool can_undo () const;
        bool can_redo () const;
        const char* undo_label () const;
        const char* redo_label () const;
        std::vector<std::string> redo_labels () const;
        void set_current_layers (const LayerTarget& layers);
        bool undo (DocRef current, DocRef* out_doc, LayerTarget* out_layers);
        bool redo (DocRef current, DocRef* out_doc, LayerTarget* out_layers);
        std::vector<std::string> entries () const;
        DocRef state (size_t i) const;
        size_t past_len () const;
        void set_current_label (const std::string& label);
        bool purge_last ();
        void clear_redo ();
        void clear ();
        size_t pixel_bytes (const doc::Document& current) const;
        size_t trim (const doc::Document& current);

    private:
        std::deque<HistoryState> undo_;
        std::vector<HistoryState> redo_;
        std::string current_label_;
        LayerTarget current_layers_;
    };

} // namespace ops
