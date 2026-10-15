#include "ops/history.h"

#include <unordered_set>

namespace ops
{

    /* Whole-document snapshots per step. Tiles are shared copy-on-write, so a snapshot costs
       O(layers + tiles) pointer copies, not pixel copies. */

    History::History (size_t max) : max_states (max < 1 ? 1 : max), max_bytes (0), current_label_ ("Open")
    {
        current_layers_.active = doc::NO_LAYER;
    }

    void History::record (const std::string& label, DocRef before, const LayerTarget& layers)
    {
        HistoryState s;
        s.label         = current_label_;
        s.doc           = before;
        s.layers        = current_layers_;
        current_label_  = label;
        current_layers_ = layers;
        undo_.push_back (s);
        clear_redo ();
        while (undo_.size () > max_states)
        {
            undo_.pop_front ();
        }
    }

    bool History::can_undo () const
    {
        return !undo_.empty ();
    }

    bool History::can_redo () const
    {
        return !redo_.empty ();
    }

    const char* History::undo_label () const
    {
        return can_undo () ? current_label_.c_str () : NULL;
    }

    const char* History::redo_label () const
    {
        return redo_.empty () ? NULL : redo_.back ().label.c_str ();
    }

    std::vector<std::string> History::redo_labels () const
    {
        std::vector<std::string> out;
        for (size_t i = redo_.size (); i-- > 0;)
        {
            out.push_back (redo_[i].label);
        }
        return out;
    }

    void History::set_current_layers (const LayerTarget& layers)
    {
        current_layers_ = layers;
    }

    bool History::undo (DocRef current, DocRef* out_doc, LayerTarget* out_layers)
    {
        if (undo_.empty ())
        {
            return false;
        }
        HistoryState prev = undo_.back ();
        undo_.pop_back ();
        HistoryState now;
        now.label       = current_label_;
        now.doc         = current;
        now.layers      = current_layers_;
        current_label_  = prev.label;
        current_layers_ = prev.layers;
        redo_.push_back (now);
        *out_doc    = prev.doc;
        *out_layers = prev.layers;
        return true;
    }

    bool History::redo (DocRef current, DocRef* out_doc, LayerTarget* out_layers)
    {
        if (redo_.empty ())
        {
            return false;
        }
        HistoryState next = redo_.back ();
        redo_.pop_back ();
        HistoryState now;
        now.label       = current_label_;
        now.doc         = current;
        now.layers      = current_layers_;
        current_label_  = next.label;
        current_layers_ = next.layers;
        undo_.push_back (now);
        *out_doc    = next.doc;
        *out_layers = next.layers;
        return true;
    }

    /* History panel rows: past labels oldest to newest, then the current label. */
    std::vector<std::string> History::entries () const
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < undo_.size (); i++)
        {
            out.push_back (undo_[i].label);
        }
        out.push_back (current_label_);
        return out;
    }

    /* Document of past entry i (0 = oldest). The current document is not held here: NULL. */
    DocRef History::state (size_t i) const
    {
        if (i >= undo_.size ())
        {
            return DocRef ();
        }
        return undo_[i].doc;
    }

    size_t History::past_len () const
    {
        return undo_.size ();
    }

    void History::set_current_label (const std::string& label)
    {
        current_label_ = label;
    }

    /* Edit > Purge > Undo: the last step can no longer be undone. */
    bool History::purge_last ()
    {
        clear_redo ();
        if (undo_.empty ())
        {
            return false;
        }
        undo_.pop_back ();
        return true;
    }

    void History::clear_redo ()
    {
        redo_.clear ();
    }

    void History::clear ()
    {
        undo_.clear ();
        redo_.clear ();
    }

    size_t surface_bytes (const raster::Surface& s, std::unordered_set<const raster::Tile*>* seen)
    {
        size_t n = 0;
        for (raster::TileMap::const_iterator it = s.tiles ().begin (); it != s.tiles ().end (); ++it)
        {
            if (seen->insert (it->second.get ()).second)
            {
                n += it->second->data.size ();
            }
        }
        return n;
    }

    /* Pixel bytes of `d` (layers, masks, selection) whose tiles are not in `seen` yet. */
    size_t doc_bytes (const doc::Document& d, std::unordered_set<const raster::Tile*>* seen)
    {
        size_t n                        = 0;
        std::vector<doc::WalkEntry> all = d.walk ();
        for (size_t i = 0; i < all.size (); i++)
        {
            const raster::Surface* s = all[i].layer->surface ();
            if (s != NULL)
            {
                n += surface_bytes (*s, seen);
            }
            if (all[i].layer->has_mask)
            {
                n += surface_bytes (all[i].layer->mask.surface, seen);
            }
        }
        return n;
    }

    /* The current document's pixels plus the tiles only history holds. */
    size_t History::pixel_bytes (const doc::Document& current) const
    {
        std::unordered_set<const raster::Tile*> seen;
        size_t n = doc_bytes (current, &seen);
        for (size_t i = 0; i < undo_.size (); i++)
        {
            n += doc_bytes (*undo_[i].doc, &seen);
        }
        for (size_t i = 0; i < redo_.size (); i++)
        {
            n += doc_bytes (*redo_[i].doc, &seen);
        }
        return n;
    }

    /* Keeps pixel memory within max_bytes by dropping the oldest undo states (newest first are kept;
       the most recent one always stays). Returns how many states were dropped. */
    size_t History::trim (const doc::Document& current)
    {
        if (max_bytes == 0 || undo_.size () <= 1)
        {
            return 0;
        }
        std::unordered_set<const raster::Tile*> seen;
        size_t total = doc_bytes (current, &seen);
        for (size_t i = redo_.size (); i-- > 0;)
        {
            total += doc_bytes (*redo_[i].doc, &seen);
        }
        size_t keep = 0;
        for (size_t i = undo_.size (); i-- > 0;)
        {
            total += doc_bytes (*undo_[i].doc, &seen);
            if (total > max_bytes && keep >= 1)
            {
                break;
            }
            keep++;
        }
        size_t drop = undo_.size () - keep;
        undo_.erase (undo_.begin (), undo_.begin () + (ptrdiff_t) drop);
        return drop;
    }

} // namespace ops
