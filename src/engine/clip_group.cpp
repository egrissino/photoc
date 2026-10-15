#include "engine/layers.h"
#include <algorithm>

namespace engine
{

    /* Clip groups follow layers as they are added, moved and deleted, as in Photoshop. A clipped
       layer clips to the nearest unclipped layer below it (its base); a base and the run of clipped
       layers directly above it form a clip group. The rules:

       1. A layer added or moved joins the clip group it lands in: it is clipped when the layer
          directly above its new slot is clipped (unless that layer travels with it from its own
          group).
       2. A clipped layer moved stays clipped while it still sits on a clip group: the layer below its
          new slot is clipped, or was in its group. Otherwise it is released.
       3. When a base is moved away or deleted, the layers clipped to it are released. Rule 1 reads
          the stack before this release, so a base moved up into its own group joins it.
       4. A clipped layer left with nothing below it to clip to is released. */

    bool contains_id (const std::vector<doc::LayerId>& ids, doc::LayerId id)
    {
        return std::find (ids.begin (), ids.end (), id) != ids.end ();
    }

    /* The sibling list holding `id`, and its index there. */
    const std::vector<doc::Layer>* siblings_of (const doc::Document& d, doc::LayerId id, size_t* index)
    {
        doc::LayerPath path;
        if (!d.path_of (id, &path))
        {
            return NULL;
        }
        *index = path.back ();
        if (path.size () == 1)
        {
            return &d.layers;
        }
        path.pop_back ();
        const doc::Layer* parent = d.layer_at (path);
        return parent != NULL ? &parent->children : NULL;
    }

    /* The layer `delta` slots above (+) or below (-) `id` among its siblings. */
    const doc::Layer* neighbour (const doc::Document& d, doc::LayerId id, long delta)
    {
        size_t idx;
        const std::vector<doc::Layer>* list = siblings_of (d, id, &idx);
        if (list == NULL)
        {
            return NULL;
        }
        long at = (long) idx + delta;
        if (at < 0 || at >= (long) list->size ())
        {
            return NULL;
        }
        return &(*list)[(size_t) at];
    }

    /* The clip group `id` belongs to, base first; empty when it has none. */
    std::vector<doc::LayerId> group_of (const doc::Document& d, doc::LayerId id)
    {
        std::vector<doc::LayerId> group;
        size_t idx;
        const std::vector<doc::Layer>* list = siblings_of (d, id, &idx);
        if (list == NULL)
        {
            return group;
        }
        size_t base = idx + 1;
        while (base-- > 0)
        {
            if (!(*list)[base].clipped)
            {
                break;
            }
        }
        if (base > idx)
        {
            return group;
        }
        for (size_t i = base; i < list->size (); i++)
        {
            if (i != base && !(*list)[i].clipped)
            {
                break;
            }
            group.push_back ((*list)[i].id);
        }
        if (group.size () < 2)
        {
            group.clear ();
        }
        return group;
    }

    /* The layers clipped to `id` (empty unless `id` is a base). */
    std::vector<doc::LayerId> clipped_to (const doc::Document& d, doc::LayerId id)
    {
        std::vector<doc::LayerId> g = group_of (d, id);
        if (g.empty () || g[0] != id)
        {
            return std::vector<doc::LayerId> ();
        }
        return std::vector<doc::LayerId> (g.begin () + 1, g.end ());
    }

    /* The clip groups of layers about to move or be deleted, read before they leave their slots. */
    ClipBefore clip_before (const doc::Document& d, const std::vector<doc::LayerId>& moving)
    {
        ClipBefore b;
        b.moving = moving;
        for (size_t i = 0; i < moving.size (); i++)
        {
            const doc::Layer* l = d.layer (moving[i]);
            if (l != NULL && l->clipped)
            {
                b.clipped.push_back (moving[i]);
            }
            std::vector<doc::LayerId> g = group_of (d, moving[i]);
            if (!g.empty ())
            {
                b.groups.push_back (std::make_pair (moving[i], g));
            }
            std::vector<doc::LayerId> above = clipped_to (d, moving[i]);
            for (size_t k = 0; k < above.size (); k++)
            {
                if (!contains_id (moving, above[k]) && !contains_id (b.left, above[k]))
                {
                    b.left.push_back (above[k]);
                }
            }
        }
        return b;
    }

    const std::vector<doc::LayerId>* before_group (const ClipBefore& b, doc::LayerId id)
    {
        for (size_t i = 0; i < b.groups.size (); i++)
        {
            if (b.groups[i].first == id)
            {
                return &b.groups[i].second;
            }
        }
        return NULL;
    }

    void set_clipped (doc::Document* d, doc::LayerId id, bool clipped)
    {
        doc::Layer* l = d->layer_mut (id);
        if (l != NULL)
        {
            l->clipped = clipped;
        }
    }

    /* Rule 4: clipped layers at the bottom of a sibling list have nothing to clip to. */
    void release_orphans (std::vector<doc::Layer>* layers)
    {
        for (size_t i = 0; i < layers->size () && (*layers)[i].clipped; i++)
        {
            (*layers)[i].clipped = false;
        }
        for (size_t i = 0; i < layers->size (); i++)
        {
            if ((*layers)[i].is_group ())
            {
                release_orphans (&(*layers)[i].children);
            }
        }
    }

    /* After `placed` were added or moved (`before` read before they moved; empty for new layers):
       each placed layer joins or leaves clip groups by rules 1-2, topmost first so a run of placed
       layers joins together, or is clipped anyway when `force` (New Layer's Use Previous Layer to
       Create Clipping Mask). Then rules 3 and 4. */
    void clip_settle (doc::Document* d, const std::vector<doc::LayerId>& placed, const ClipBefore& before, bool force)
    {
        std::vector<std::pair<doc::LayerPath, doc::LayerId>> order;
        for (size_t i = 0; i < placed.size (); i++)
        {
            doc::LayerPath path;
            if (d->path_of (placed[i], &path))
            {
                order.push_back (std::make_pair (path, placed[i]));
            }
        }
        std::sort (order.begin (), order.end ());
        std::vector<doc::LayerId> none;
        for (size_t i = order.size (); i-- > 0;)
        {
            doc::LayerId id                        = order[i].second;
            const std::vector<doc::LayerId>* g     = before_group (before, id);
            const std::vector<doc::LayerId>& group = g != NULL ? *g : none;
            const doc::Layer* above                = neighbour (*d, id, 1);
            const doc::Layer* below                = neighbour (*d, id, -1);
            bool travelling = above != NULL && contains_id (before.moving, above->id) && contains_id (group, above->id);
            bool joins      = above != NULL && above->clipped && !travelling;
            bool stays =
                contains_id (before.clipped, id) && below != NULL && (below->clipped || contains_id (group, below->id));
            set_clipped (d, id, force || joins || stays);
        }
        for (size_t i = 0; i < before.left.size (); i++)
        {
            if (!contains_id (placed, before.left[i]))
            {
                set_clipped (d, before.left[i], false);
            }
        }
        release_orphans (&d->layers);
    }

} // namespace engine
