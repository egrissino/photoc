#include "engine/layers.h"
#include "engine/session.h"
#include <math.h>
#include <string.h>

namespace engine
{

    /* View > guides: New Guide, New Guide Layout, Clear Guides, and moving or deleting one guide (the
       rulers and the Move tool). Guides belong to the document (undoable, saved in PSD); showing
       them, the grid and the rulers is the window's business. */

    bool guide_orientation (const Value& p, bool* vertical, const char* cmd, Error* err)
    {
        std::string o = "vertical";
        param_str (p, "orientation", &o);
        if (o != "vertical" && o != "horizontal")
        {
            return bad_params (err, cmd, "`orientation` must be vertical or horizontal");
        }
        *vertical = o == "vertical";
        return true;
    }

    bool cmd_new_guide (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "view.newGuide";
        bool vertical;
        double pos;
        if (!guide_orientation (p, &vertical, cmd, err))
        {
            return false;
        }
        if (!param_f64 (p, "position", &pos) || !isfinite (pos) || fabs (pos) > 1e7)
        {
            return bad_params (err, cmd, "`position` must be a number (px)");
        }
        Edit e (s);
        doc::Guide g = {vertical, pos};
        e.doc ()->guides.push_back (g);
        if (!e.commit ("New Guide", err))
        {
            return false;
        }
        *result = Value{{"index", s.active ()->doc->guides.size () - 1}};
        return true;
    }

    bool guide_index (const Session& s, const Value& p, size_t* out, const char* cmd, Error* err)
    {
        int64_t i;
        if (!param_i64 (p, "index", &i) || i < 0 || (size_t) i >= s.active ()->doc->guides.size ())
        {
            return bad_params (err, cmd, "no guide at `index`");
        }
        *out = (size_t) i;
        return true;
    }

    bool cmd_move_guide (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "view.moveGuide";
        size_t i;
        double pos;
        if (!guide_index (s, p, &i, cmd, err))
        {
            return false;
        }
        if (!param_f64 (p, "position", &pos) || !isfinite (pos) || fabs (pos) > 1e7)
        {
            return bad_params (err, cmd, "`position` must be a number (px)");
        }
        Edit e (s);
        e.doc ()->guides[i].position = pos;
        return e.commit ("Move Guide", err);
    }

    bool cmd_delete_guide (Session& s, const Value& p, Value* result, Error* err)
    {
        size_t i;
        if (!guide_index (s, p, &i, "view.deleteGuide", err))
        {
            return false;
        }
        Edit e (s);
        e.doc ()->guides.erase (e.doc ()->guides.begin () + (ptrdiff_t) i);
        return e.commit ("Delete Guide", err);
    }

    bool has_guides (const Session& s, std::string* why)
    {
        if (!has_doc (s, why))
        {
            return false;
        }
        if (!s.active ()->doc->guides.empty ())
        {
            return true;
        }
        *why = "the document has no guides";
        return false;
    }

    bool cmd_clear_guides (Session& s, const Value& p, Value* result, Error* err)
    {
        Edit e (s);
        e.doc ()->guides.clear ();
        return e.commit ("Clear Guides", err);
    }

    /* New Guide Layout: columns and rows (with a gutter between them, or a fixed width / height)
       inside margins; guides at every column and row edge. */
    bool cmd_new_guide_layout (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = "view.newGuideLayout";
        int64_t cols = 0, rows = 0;
        param_i64 (p, "columns", &cols);
        param_i64 (p, "rows", &rows);
        if (cols < 0 || cols > 1000 || rows < 0 || rows > 1000)
        {
            return bad_params (err, cmd, "`columns` and `rows` must be 0..1000");
        }
        double col_gutter = 0, row_gutter = 0, col_width = 0, row_height = 0, top = 0, left = 0, bottom = 0, right = 0;
        param_f64 (p, "columnGutter", &col_gutter);
        param_f64 (p, "rowGutter", &row_gutter);
        param_f64 (p, "columnWidth", &col_width);
        param_f64 (p, "rowHeight", &row_height);
        param_f64 (p, "marginTop", &top);
        param_f64 (p, "marginLeft", &left);
        param_f64 (p, "marginBottom", &bottom);
        param_f64 (p, "marginRight", &right);
        bool clear = false, center = false;
        param_bool (p, "clearExisting", &clear);
        param_bool (p, "centerColumns", &center);
        Edit e (s);
        doc::Document* d = e.doc ();
        if (clear)
        {
            d->guides.clear ();
        }
        double w = d->size.width, h = d->size.height;
        bool margins = top != 0 || left != 0 || bottom != 0 || right != 0;
        for (int axis = 0; axis < 2; axis++)
        {
            int64_t n = axis == 0 ? cols : rows;
            if (n <= 0 && !margins)
            {
                continue;
            }
            double a = axis == 0 ? left : top, b = (axis == 0 ? w : h) - (axis == 0 ? right : bottom);
            double gutter = axis == 0 ? col_gutter : row_gutter, fixed = axis == 0 ? col_width : row_height;
            bool vertical = axis == 0;
            std::vector<double> at;
            if (margins)
            {
                at.push_back (a);
                at.push_back (b);
            }
            if (n > 0)
            {
                double cell  = fixed > 0 ? fixed : (b - a - gutter * (double) (n - 1)) / (double) n;
                double span  = cell * (double) n + gutter * (double) (n - 1);
                double start = fixed > 0 && center ? a + (b - a - span) / 2.0 : a;
                for (int64_t i = 0; i < n; i++)
                {
                    double x0 = start + (double) i * (cell + gutter);
                    at.push_back (x0);
                    at.push_back (x0 + cell);
                }
            }
            for (size_t i = 0; i < at.size (); i++)
            {
                bool dup = false;
                for (size_t k = 0; k < d->guides.size (); k++)
                {
                    dup = dup || (d->guides[k].vertical == vertical && fabs (d->guides[k].position - at[i]) < 1e-6);
                }
                if (!dup)
                {
                    d->guides.push_back (doc::Guide{vertical, at[i]});
                }
            }
        }
        if (!e.commit ("New Guide Layout", err))
        {
            return false;
        }
        *result = Value{{"guides", s.active ()->doc->guides.size ()}};
        return true;
    }

    bool cmd_list_guides (Session& s, const Value& p, Value* result, Error* err)
    {
        Value list                       = Value::array ();
        const std::vector<doc::Guide>& g = s.active ()->doc->guides;
        for (size_t i = 0; i < g.size (); i++)
        {
            list.push_back (
                Value{{"orientation", g[i].vertical ? "vertical" : "horizontal"}, {"position", g[i].position}});
        }
        *result = Value{{"guides", list}};
        return true;
    }

    const CommandSpec VIEW_COMMANDS[] = {
        {"view.newGuide", "New Guide…", "", NULL,
            "{\"orientation\":\"vertical|horizontal\"=\"vertical\",\"position\":px}", has_doc, cmd_new_guide, true},
        {"view.moveGuide", "Move Guide", "", NULL, "{\"index\":n,\"position\":px}", has_guides, cmd_move_guide, true},
        {"view.deleteGuide", "Delete Guide", "", NULL, "{\"index\":n}", has_guides, cmd_delete_guide, true},
        {"view.clearGuides", "Clear Guides", "", NULL, "{}", has_guides, cmd_clear_guides, true},
        {"view.newGuideLayout", "New Guide Layout…", "", NULL,
            "{\"columns\":0..1000,\"rows\":0..1000,\"columnGutter\":px,\"rowGutter\":px,\"columnWidth\":px?,"
            "\"rowHeight\":px?,\"marginTop|Left|Bottom|Right\":px,"
            "\"centerColumns\":bool=false,\"clearExisting\":bool=false}",
            has_doc, cmd_new_guide_layout, true},
        {"view.guides", "Guides", "", NULL, "{} → {guides:[{orientation, position}]}", has_doc, cmd_list_guides, false},
    };

    void register_view_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), VIEW_COMMANDS, VIEW_COMMANDS + sizeof VIEW_COMMANDS / sizeof VIEW_COMMANDS[0]);
    }

} // namespace engine
