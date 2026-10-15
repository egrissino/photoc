#include "engine/brush.h"
#include "engine/layers.h"
#include "engine/session.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

namespace engine
{

    /* Layer > Align and Distribute (and the Move tool's buttons). Two or more selected layers align
       to the box around them; one layer aligns to the selection when there is one, else to the
       canvas ("to" overrides). Distribute spaces three or more by an edge or centre, or with equal
       gaps (Horizontally / Vertically). Layers move by whole pixels with their linked layers, as
       the Move tool moves them. Edges are compared at double scale so centres stay exact. */

    enum class Edge : uint8_t
    {
        Top,
        VCenter,
        Bottom,
        Left,
        HCenter,
        Right,
    };

    struct Placed
    {
        doc::LayerId id;
        geom::Rect r;
    };

    const char* const EDGE_IDS[6] = {
        "topEdges", "verticalCenters", "bottomEdges", "leftEdges", "horizontalCenters", "rightEdges"};

    bool vertical (Edge e)
    {
        return e == Edge::Top || e == Edge::VCenter || e == Edge::Bottom;
    }

    int64_t edge2 (Edge e, geom::Rect r)
    {
        switch (e)
        {
        case Edge::Top:
            return 2 * (int64_t) r.y0;
        case Edge::VCenter:
            return (int64_t) r.y0 + r.y1;
        case Edge::Bottom:
            return 2 * (int64_t) r.y1;
        case Edge::Left:
            return 2 * (int64_t) r.x0;
        case Edge::HCenter:
            return (int64_t) r.x0 + r.x1;
        default:
            return 2 * (int64_t) r.x1;
        }
    }

    /* What a layer shows: its pixels, or its visible children's. */
    geom::Rect content_rect (const doc::Layer& l)
    {
        if (l.is_group ())
        {
            geom::Rect r;
            for (size_t i = 0; i < l.children.size (); i++)
            {
                if (l.children[i].visible)
                {
                    r = r.unite (content_rect (l.children[i]));
                }
            }
            return r;
        }
        const raster::Surface* s = l.surface ();
        return s != NULL ? s->content_bounds () : geom::Rect ();
    }

    /* The selected top-level layers with content. */
    std::vector<Placed> placed_selection (const doc::Document& d, const std::vector<doc::LayerId>& sel)
    {
        std::vector<doc::LayerId> top = top_level (d, sel);
        std::vector<Placed> out;
        for (size_t i = 0; i < top.size (); i++)
        {
            const doc::Layer* l = d.layer (top[i]);
            if (l == NULL)
            {
                continue;
            }
            geom::Rect r = content_rect (*l).intersect (geom::Rect::from_xywh (-1000000, -1000000, 2000000, 2000000));
            if (!r.is_empty ())
            {
                out.push_back (Placed{top[i], r});
            }
        }
        return out;
    }

    bool apply_moves (doc::Document* d, const std::vector<Placed>& items, const std::vector<int32_t>& dx,
        const std::vector<int32_t>& dy, int* moved, Error* err)
    {
        *moved = 0;
        for (size_t i = 0; i < items.size (); i++)
        {
            if (dx[i] == 0 && dy[i] == 0)
            {
                continue;
            }
            if (!move_layers (d, std::vector<doc::LayerId> (1, items[i].id), dx[i], dy[i], false, err))
            {
                return false;
            }
            (*moved)++;
        }
        return true;
    }

    bool cmd_align (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd = s.running ();
        int ei          = -1;
        for (int i = 0; i < 6; i++)
        {
            if (strcmp (cmd + strlen ("layer.align."), EDGE_IDS[i]) == 0)
            {
                ei = i;
            }
        }
        if (ei < 0)
        {
            return bad_params (err, cmd, "unknown edge");
        }
        Edge edge      = (Edge) ei;
        std::string to = "auto";
        param_str (p, "to", &to);
        if (to != "auto" && to != "layers" && to != "selection" && to != "canvas")
        {
            return bad_params (err, cmd, "`to` must be auto, layers, selection or canvas");
        }
        std::vector<doc::LayerId> sel = s.active ()->selected ();
        Edit e (s);
        doc::Document* d          = e.doc ();
        std::vector<Placed> items = placed_selection (*d, sel);
        if (items.empty ())
        {
            return fail (err, ErrorKind::Other, "the selected layers have nothing to align");
        }
        geom::Rect target;
        if (to == "canvas" || (to == "auto" && sel.size () == 1 && !d->has_selection))
        {
            target = d->bounds ();
        }
        else if (to == "selection" || (to == "auto" && sel.size () == 1))
        {
            if (!d->has_selection)
            {
                return fail (err, ErrorKind::Other, "there is no selection to align to");
            }
            target = d->selection.content_bounds ();
        }
        else
        {
            for (size_t i = 0; i < items.size (); i++)
            {
                target = target.unite (items[i].r);
            }
        }
        if (target.is_empty ())
        {
            return fail (err, ErrorKind::Other, "nothing to align to");
        }
        std::vector<int32_t> dx (items.size (), 0), dy (items.size (), 0);
        for (size_t i = 0; i < items.size (); i++)
        {
            int64_t diff = edge2 (edge, target) - edge2 (edge, items[i].r);
            int32_t m    = (int32_t) (diff >= 0 ? diff / 2 : -((-diff + 1) / 2));
            if (vertical (edge))
            {
                dy[i] = m;
            }
            else
            {
                dx[i] = m;
            }
        }
        int moved;
        if (!apply_moves (d, items, dx, dy, &moved, err))
        {
            return false;
        }
        if (!e.commit ("Align", err))
        {
            return false;
        }
        *result = Value{{"moved", moved}};
        return true;
    }

    bool cmd_distribute (Session& s, const Value& p, Value* result, Error* err)
    {
        const char* cmd               = s.running ();
        const char* kind              = cmd + strlen ("layer.distribute.");
        std::vector<doc::LayerId> sel = s.active ()->selected ();
        Edit e (s);
        doc::Document* d          = e.doc ();
        std::vector<Placed> items = placed_selection (*d, sel);
        size_t n                  = items.size ();
        if (n < 3)
        {
            return fail (err, ErrorKind::Other, "distributing needs three or more layers with content");
        }
        std::vector<int32_t> dx (n, 0), dy (n, 0);
        std::vector<size_t> order (n);
        for (size_t i = 0; i < n; i++)
        {
            order[i] = i;
        }
        bool gaps = strcmp (kind, "horizontally") == 0 || strcmp (kind, "vertically") == 0;
        if (gaps)
        {
            /* Equal gaps between neighbours; the outermost layers stay put. */
            bool v      = strcmp (kind, "vertically") == 0;
            Edge centre = v ? Edge::VCenter : Edge::HCenter, lo_e = v ? Edge::Top : Edge::Left,
                 hi_e = v ? Edge::Bottom : Edge::Right;
            for (size_t i = 1; i < n; i++)
            {
                for (size_t j = i; j > 0 && edge2 (centre, items[order[j]].r) < edge2 (centre, items[order[j - 1]].r);
                     j--)
                {
                    std::swap (order[j], order[j - 1]);
                }
            }
            int64_t first = edge2 (lo_e, items[order[0]].r) / 2, last = edge2 (hi_e, items[order[n - 1]].r) / 2,
                    total = 0;
            for (size_t i = 0; i < n; i++)
            {
                total += (edge2 (hi_e, items[i].r) - edge2 (lo_e, items[i].r)) / 2;
            }
            double gap = (double) (last - first - total) / (double) (n - 1), cursor = (double) first;
            for (size_t k = 0; k < n; k++)
            {
                size_t i   = order[k];
                int64_t lo = edge2 (lo_e, items[i].r) / 2, size = edge2 (hi_e, items[i].r) / 2 - lo;
                int32_t m = (int32_t) (llround (cursor) - lo);
                if (v)
                {
                    dy[i] = m;
                }
                else
                {
                    dx[i] = m;
                }
                cursor += (double) size + gap;
            }
        }
        else
        {
            int ei = -1;
            for (int i = 0; i < 6; i++)
            {
                if (strcmp (kind, EDGE_IDS[i]) == 0)
                {
                    ei = i;
                }
            }
            if (ei < 0)
            {
                return bad_params (err, cmd, "unknown kind");
            }
            Edge edge = (Edge) ei;
            for (size_t i = 1; i < n; i++)
            {
                for (size_t j = i; j > 0 && edge2 (edge, items[order[j]].r) < edge2 (edge, items[order[j - 1]].r); j--)
                {
                    std::swap (order[j], order[j - 1]);
                }
            }
            double a = (double) edge2 (edge, items[order[0]].r), b = (double) edge2 (edge, items[order[n - 1]].r);
            for (size_t k = 0; k < n; k++)
            {
                size_t i    = order[k];
                double want = a + (b - a) * (double) k / (double) (n - 1);
                int32_t m   = (int32_t) lround ((want - (double) edge2 (edge, items[i].r)) / 2.0);
                if (vertical (edge))
                {
                    dy[i] = m;
                }
                else
                {
                    dx[i] = m;
                }
            }
        }
        int moved;
        if (!apply_moves (d, items, dx, dy, &moved, err))
        {
            return false;
        }
        if (!e.commit ("Distribute", err))
        {
            return false;
        }
        *result = Value{{"moved", moved}};
        return true;
    }

    bool can_align (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        return true;
    }

    bool can_distribute (const Session& s, std::string* why)
    {
        if (!has_layer (s, why))
        {
            return false;
        }
        if (s.active ()->selected ().size () >= 3)
        {
            return true;
        }
        *why = "select three or more layers to distribute";
        return false;
    }

    const char* const ALIGN_PARAMS = "{\"to\":\"auto|layers|selection|canvas\"=\"auto\"} (auto: the selected layers, "
                                     "or for one layer the selection or canvas)";

    const CommandSpec ALIGN_COMMANDS[] = {
        {"layer.align.topEdges", "Top Edges", "Layer/Align", NULL, ALIGN_PARAMS, can_align, cmd_align, true},
        {"layer.align.verticalCenters", "Vertical Centers", "Layer/Align", NULL, ALIGN_PARAMS, can_align, cmd_align,
            true},
        {"layer.align.bottomEdges", "Bottom Edges", "Layer/Align", NULL, ALIGN_PARAMS, can_align, cmd_align, true},
        {"layer.align.leftEdges", "Left Edges", "Layer/Align", NULL, ALIGN_PARAMS, can_align, cmd_align, true},
        {"layer.align.horizontalCenters", "Horizontal Centers", "Layer/Align", NULL, ALIGN_PARAMS, can_align, cmd_align,
            true},
        {"layer.align.rightEdges", "Right Edges", "Layer/Align", NULL, ALIGN_PARAMS, can_align, cmd_align, true},
        {"layer.distribute.topEdges", "Top Edges", "Layer/Distribute", NULL, "{}", can_distribute, cmd_distribute,
            true},
        {"layer.distribute.verticalCenters", "Vertical Centers", "Layer/Distribute", NULL, "{}", can_distribute,
            cmd_distribute, true},
        {"layer.distribute.bottomEdges", "Bottom Edges", "Layer/Distribute", NULL, "{}", can_distribute, cmd_distribute,
            true},
        {"layer.distribute.leftEdges", "Left Edges", "Layer/Distribute", NULL, "{}", can_distribute, cmd_distribute,
            true},
        {"layer.distribute.horizontalCenters", "Horizontal Centers", "Layer/Distribute", NULL, "{}", can_distribute,
            cmd_distribute, true},
        {"layer.distribute.rightEdges", "Right Edges", "Layer/Distribute", NULL, "{}", can_distribute, cmd_distribute,
            true},
        {"layer.distribute.horizontally", "Horizontally", "Layer/Distribute", NULL, "{} (equal gaps)", can_distribute,
            cmd_distribute, true},
        {"layer.distribute.vertically", "Vertically", "Layer/Distribute", NULL, "{} (equal gaps)", can_distribute,
            cmd_distribute, true},
    };

    void register_align_commands (std::vector<CommandSpec>* out)
    {
        out->insert (out->end (), ALIGN_COMMANDS, ALIGN_COMMANDS + sizeof ALIGN_COMMANDS / sizeof ALIGN_COMMANDS[0]);
    }

} // namespace engine
