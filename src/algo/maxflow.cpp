#include "algo/maxflow.h"

#include <math.h>

namespace algo
{

    /* Max-flow / min-cut by Boykov and Kolmogorov ("An Experimental Comparison of Min-Cut/Max-Flow
       Algorithms for Energy Minimization in Vision", IEEE PAMI 26(9), 2004): a source tree and a
       sink tree grow towards each other; each contact gives an augmenting path; saturated tree edges
       make orphans, which are adopted again or freed before growing on. The trees are reused between
       augmentations, which suits the short paths of image grids. Arcs come in pairs (a, a ^ 1). */

    const uint32_t NONE     = UINT32_MAX;
    const uint32_t TERMINAL = UINT32_MAX - 1;
    const uint32_t ORPHAN   = UINT32_MAX - 2;
    const uint32_t INF_DIST = UINT32_MAX;

    /* Residuals at or below this count as saturated (float round-off would otherwise leave long chains
       of negligible augmentations). */
    const float EPS = 1e-5f;

    MaxflowGraph::MaxflowGraph (size_t nodes, size_t edges)
        : first_ (nodes, NONE), tr_cap_ (nodes, 0.0f), parent_ (nodes, NONE), is_sink_ (nodes, 0), ts_ (nodes, 0),
          dist_ (nodes, 0), active_ (nodes, 0), flow_ (0.0)
    {
        head_.reserve (edges * 2);
        next_.reserve (edges * 2);
        r_cap_.reserve (edges * 2);
    }

    /* Terminal capacities of node i; the common part of both is flow already. */
    void MaxflowGraph::add_tweights (size_t i, float cap_source, float cap_sink)
    {
        float delta = tr_cap_[i];
        if (delta > 0.0f)
        {
            cap_source += delta;
        }
        else
        {
            cap_sink -= delta;
        }
        flow_ += fminf (cap_source, cap_sink);
        tr_cap_[i] = cap_source - cap_sink;
    }

    void MaxflowGraph::add_edge (size_t i, size_t j, float cap, float rev_cap)
    {
        uint32_t a = (uint32_t) head_.size ();
        head_.push_back ((uint32_t) j);
        next_.push_back (first_[i]);
        r_cap_.push_back (cap);
        first_[i] = a;
        head_.push_back ((uint32_t) i);
        next_.push_back (first_[j]);
        r_cap_.push_back (rev_cap);
        first_[j] = a + 1;
    }

    /* After maxflow: whether node i is on the source side of the minimum cut (free nodes are not). */
    bool MaxflowGraph::in_source (size_t i) const
    {
        return parent_[i] != NONE && !is_sink_[i];
    }

    double MaxflowGraph::maxflow ()
    {
        size_t n = first_.size ();
        std::deque<uint32_t> queue, orphans;
        for (size_t i = 0; i < n; i++)
        {
            active_[i] = 0;
            ts_[i]     = 0;
            if (fabsf (tr_cap_[i]) > EPS)
            {
                is_sink_[i] = tr_cap_[i] < 0.0f;
                parent_[i]  = TERMINAL;
                dist_[i]    = 1;
                active_[i]  = 1;
                queue.push_back ((uint32_t) i);
            }
            else
            {
                parent_[i] = NONE;
            }
        }
        uint32_t time = 0;
        while (!queue.empty ())
        {
            size_t i = queue.front ();
            queue.pop_front ();
            active_[i] = 0;
            if (parent_[i] == NONE)
            {
                continue;
            }
            /* Growth. */
            uint32_t contact = NONE;
            bool sink        = is_sink_[i];
            for (uint32_t a = first_[i]; a != NONE; a = next_[a])
            {
                size_t j  = head_[a];
                float cap = sink ? r_cap_[a ^ 1] : r_cap_[a];
                if (cap <= EPS)
                {
                    continue;
                }
                if (parent_[j] == NONE)
                {
                    is_sink_[j] = sink;
                    parent_[j]  = a ^ 1;
                    ts_[j]      = ts_[i];
                    dist_[j]    = dist_[i] + 1;
                    if (!active_[j])
                    {
                        active_[j] = 1;
                        queue.push_back ((uint32_t) j);
                    }
                }
                else if ((bool) is_sink_[j] != sink)
                {
                    /* An arc from the source side to the sink side. */
                    contact = sink ? a ^ 1 : a;
                    break;
                }
                else if (ts_[j] <= ts_[i] && dist_[j] > dist_[i])
                {
                    /* Shorten the path to the terminal. */
                    parent_[j] = a ^ 1;
                    ts_[j]     = ts_[i];
                    dist_[j]   = dist_[i] + 1;
                }
            }
            if (contact == NONE)
            {
                continue;
            }
            /* Keep scanning this node after the augmentation. */
            if (!active_[i])
            {
                active_[i] = 1;
                queue.push_front ((uint32_t) i);
            }
            time++;
            augment (contact, &orphans);
            while (!orphans.empty ())
            {
                size_t o = orphans.front ();
                orphans.pop_front ();
                adopt (o, time, &queue, &orphans);
            }
        }
        return flow_;
    }

    /* Pushes the bottleneck along the path through arc m (source side to sink side). */
    void MaxflowGraph::augment (size_t m, std::deque<uint32_t>* orphans)
    {
        size_t u = head_[m ^ 1], v = head_[m];
        float b  = r_cap_[m];
        size_t x = u;
        while (parent_[x] != TERMINAL)
        {
            uint32_t a = parent_[x];
            b          = fminf (b, r_cap_[a ^ 1]);
            x          = head_[a];
        }
        b = fminf (b, tr_cap_[x]);
        x = v;
        while (parent_[x] != TERMINAL)
        {
            uint32_t a = parent_[x];
            b          = fminf (b, r_cap_[a]);
            x          = head_[a];
        }
        b = fminf (b, -tr_cap_[x]);

        r_cap_[m ^ 1] += b;
        r_cap_[m] -= b;
        x = u;
        while (parent_[x] != TERMINAL)
        {
            uint32_t a = parent_[x];
            r_cap_[a] += b;
            r_cap_[a ^ 1] -= b;
            size_t up = head_[a];
            if (r_cap_[a ^ 1] <= EPS)
            {
                parent_[x] = ORPHAN;
                orphans->push_back ((uint32_t) x);
            }
            x = up;
        }
        tr_cap_[x] -= b;
        if (tr_cap_[x] <= EPS)
        {
            parent_[x] = ORPHAN;
            orphans->push_back ((uint32_t) x);
        }
        x = v;
        while (parent_[x] != TERMINAL)
        {
            uint32_t a = parent_[x];
            r_cap_[a ^ 1] += b;
            r_cap_[a] -= b;
            size_t up = head_[a];
            if (r_cap_[a] <= EPS)
            {
                parent_[x] = ORPHAN;
                orphans->push_back ((uint32_t) x);
            }
            x = up;
        }
        tr_cap_[x] += b;
        if (tr_cap_[x] >= -EPS)
        {
            parent_[x] = ORPHAN;
            orphans->push_back ((uint32_t) x);
        }
        flow_ += b;
    }

    /* Finds orphan i a new parent in its tree whose path still reaches the terminal (the shortest),
       or frees it and orphans its children. */
    void MaxflowGraph::adopt (size_t i, uint32_t time, std::deque<uint32_t>* queue, std::deque<uint32_t>* orphans)
    {
        bool sink     = is_sink_[i];
        uint32_t best = NONE, d_min = INF_DIST;
        for (uint32_t a = first_[i]; a != NONE; a = next_[a])
        {
            float cap = sink ? r_cap_[a] : r_cap_[a ^ 1];
            size_t j  = head_[a];
            if (cap <= EPS || (bool) is_sink_[j] != sink || parent_[j] == NONE)
            {
                continue;
            }
            uint32_t d = 0;
            size_t k   = j;
            for (;;)
            {
                if (ts_[k] == time)
                {
                    d = dist_[k] > INF_DIST - d ? INF_DIST - 1 : d + dist_[k];
                    break;
                }
                uint32_t pa = parent_[k];
                d++;
                if (pa == TERMINAL)
                {
                    ts_[k]   = time;
                    dist_[k] = 1;
                    break;
                }
                if (pa == ORPHAN)
                {
                    d = INF_DIST;
                    break;
                }
                k = head_[pa];
            }
            if (d == INF_DIST)
            {
                continue;
            }
            if (d < d_min)
            {
                best  = a;
                d_min = d;
            }
            /* Cache the distances along the path. */
            uint32_t dd = d;
            for (k = j; ts_[k] != time; k = head_[parent_[k]])
            {
                ts_[k]   = time;
                dist_[k] = dd--;
            }
        }
        if (best != NONE)
        {
            parent_[i] = best;
            ts_[i]     = time;
            dist_[i]   = d_min + 1;
            return;
        }
        parent_[i] = NONE;
        for (uint32_t a = first_[i]; a != NONE; a = next_[a])
        {
            size_t j    = head_[a];
            uint32_t pj = parent_[j];
            if ((bool) is_sink_[j] != sink || pj == NONE)
            {
                continue;
            }
            float cap = sink ? r_cap_[a] : r_cap_[a ^ 1];
            if (cap > EPS && !active_[j])
            {
                active_[j] = 1;
                queue->push_back ((uint32_t) j);
            }
            if (pj != TERMINAL && pj != ORPHAN && head_[pj] == i)
            {
                parent_[j] = ORPHAN;
                orphans->push_back ((uint32_t) j);
            }
        }
    }

} // namespace algo
