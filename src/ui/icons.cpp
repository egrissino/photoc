#include "ui/icons.h"

#include "ui/icon_data.h"
#include <SDL3/SDL_opengl.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace ui
{

    /* The interface icons: Lucide-style SVGs (24 x 24, stroked, a few fills) rasterized once per
       pixel size into a small alpha texture, then drawn tinted. Only what the icon set uses is
       read: path, circle, ellipse, rect, line, polygon and polyline, fill and stroke attributes, and
       a group's translate/scale and stroke-width. Strokes take their coverage from the distance to
       the path (exact round caps and joins), fills from 4 x 4 samples. */

    struct Pt
    {
        float x;
        float y;
    };

    struct Subpath
    {
        std::vector<Pt> pts;
        bool closed;
    };

    struct Shape
    {
        std::vector<Subpath> subpaths;
        bool fill;
        bool stroke;
        bool evenodd;
        float width;
    };

    struct Group
    {
        float tx;
        float ty;
        float scale;
        float width;
        bool has_width;
    };

    /* The value of attribute `name` in the tag text [tag, end), or false. */
    bool attr (const char* tag, const char* end, const char* name, std::string* out)
    {
        size_t n = strlen (name);
        for (const char* p = tag; p + n + 2 < end; p++)
        {
            if (strncmp (p, name, n) != 0 || p[n] != '=' || p[n + 1] != '"')
            {
                continue;
            }
            if (p > tag && !isspace ((unsigned char) p[-1]))
            {
                continue;
            }
            const char* v = p + n + 2;
            const char* q = (const char*) memchr (v, '"', (size_t) (end - v));
            if (q == NULL)
            {
                return false;
            }
            out->assign (v, (size_t) (q - v));
            return true;
        }
        return false;
    }

    float attr_num (const char* tag, const char* end, const char* name, float fallback)
    {
        std::string v;
        return attr (tag, end, name, &v) ? strtof (v.c_str (), NULL) : fallback;
    }

    /* Number parsing for path data: skips separators; false at the end or a command letter. */
    bool next_num (const char** s, float* out)
    {
        const char* p = *s;
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')
        {
            p++;
        }
        if (*p == 0 || (!isdigit ((unsigned char) *p) && *p != '-' && *p != '+' && *p != '.'))
        {
            *s = p;
            return false;
        }
        char* e;
        *out = strtof (p, &e);
        if (e == p)
        {
            return false;
        }
        *s = e;
        return true;
    }

    /* An arc flag: a single 0 or 1, possibly without separators. */
    bool next_flag (const char** s, bool* out)
    {
        const char* p = *s;
        while (*p == ' ' || *p == ',')
        {
            p++;
        }
        if (*p != '0' && *p != '1')
        {
            return false;
        }
        *out = *p == '1';
        *s   = p + 1;
        return true;
    }

    void cubic (Subpath* sp, Pt p0, Pt c1, Pt c2, Pt p3)
    {
        const int N = 16;
        for (int i = 1; i <= N; i++)
        {
            float t = (float) i / N, u = 1.0f - t;
            Pt q = {u * u * u * p0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p3.x,
                u * u * u * p0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p3.y};
            sp->pts.push_back (q);
        }
    }

    void quad (Subpath* sp, Pt p0, Pt c, Pt p2)
    {
        const int N = 12;
        for (int i = 1; i <= N; i++)
        {
            float t = (float) i / N, u = 1.0f - t;
            Pt q = {u * u * p0.x + 2 * u * t * c.x + t * t * p2.x, u * u * p0.y + 2 * u * t * c.y + t * t * p2.y};
            sp->pts.push_back (q);
        }
    }

    /* An SVG endpoint arc (converted to its centre form, SVG 1.1 appendix F.6). */
    void arc (Subpath* sp, Pt p0, float rx, float ry, float phi_deg, bool large, bool sweep, Pt p1)
    {
        if (rx == 0.0f || ry == 0.0f)
        {
            sp->pts.push_back (p1);
            return;
        }
        rx        = fabsf (rx);
        ry        = fabsf (ry);
        float phi = phi_deg * (float) M_PI / 180.0f, cp = cosf (phi), spn = sinf (phi);
        float dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
        float x1 = cp * dx + spn * dy, y1 = -spn * dx + cp * dy;
        float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lam > 1.0f)
        {
            rx *= sqrtf (lam);
            ry *= sqrtf (lam);
        }
        float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        float co  = den > 0 ? sqrtf (fmaxf (num / den, 0.0f)) : 0.0f;
        if (large == sweep)
        {
            co = -co;
        }
        float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
        float cx = cp * cxp - spn * cyp + (p0.x + p1.x) / 2, cy = spn * cxp + cp * cyp + (p0.y + p1.y) / 2;
        float a0 = atan2f ((y1 - cyp) / ry, (x1 - cxp) / rx);
        float a1 = atan2f ((-y1 - cyp) / ry, (-x1 - cxp) / rx);
        float da = a1 - a0;
        if (sweep && da < 0)
        {
            da += 2 * (float) M_PI;
        }
        if (!sweep && da > 0)
        {
            da -= 2 * (float) M_PI;
        }
        int n = (int) ceilf (fabsf (da) / ((float) M_PI / 16)) + 1;
        for (int i = 1; i <= n; i++)
        {
            float a = a0 + da * (float) i / (float) n;
            Pt q    = {cx + rx * cosf (a) * cp - ry * sinf (a) * spn, cy + rx * cosf (a) * spn + ry * sinf (a) * cp};
            sp->pts.push_back (q);
        }
    }

    /* Path data to flattened subpaths. */
    void parse_path (const char* d, std::vector<Subpath>* out)
    {
        Pt cur = {0, 0}, start = {0, 0}, last_ctrl = {0, 0};
        char last_cmd = 0;
        char cmd      = 0;
        const char* s = d;
        Subpath* sp   = NULL;
        while (*s)
        {
            while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t')
            {
                s++;
            }
            if (*s == 0)
            {
                break;
            }
            if (isalpha ((unsigned char) *s))
            {
                cmd = *s++;
            }
            else if (cmd == 0)
            {
                break;
            }
            bool rel = islower ((unsigned char) cmd) != 0;
            char c   = (char) toupper ((unsigned char) cmd);
            float v[7];
            Pt base = rel ? cur : Pt{0, 0};
            if (c == 'Z')
            {
                if (sp != NULL)
                {
                    sp->closed = true;
                }
                cur      = start;
                sp       = NULL;
                last_cmd = c;
                cmd      = 0;
                continue;
            }
            if (sp == NULL || c == 'M')
            {
                out->push_back (Subpath ());
                sp         = &out->back ();
                sp->closed = false;
                if (c != 'M')
                {
                    sp->pts.push_back (cur);
                }
            }
            bool ok = true;
            switch (c)
            {
            case 'M':
                ok = next_num (&s, &v[0]) && next_num (&s, &v[1]);
                if (!ok)
                {
                    break;
                }
                cur   = Pt{base.x + v[0], base.y + v[1]};
                start = cur;
                sp->pts.push_back (cur);
                cmd = rel ? 'l' : 'L';
                break;
            case 'L':
                ok = next_num (&s, &v[0]) && next_num (&s, &v[1]);
                if (!ok)
                {
                    break;
                }
                cur = Pt{base.x + v[0], base.y + v[1]};
                sp->pts.push_back (cur);
                break;
            case 'H':
                ok = next_num (&s, &v[0]);
                if (!ok)
                {
                    break;
                }
                cur.x = base.x + v[0];
                sp->pts.push_back (cur);
                break;
            case 'V':
                ok = next_num (&s, &v[0]);
                if (!ok)
                {
                    break;
                }
                cur.y = (rel ? cur.y : 0) + v[0];
                sp->pts.push_back (cur);
                break;
            case 'C':
                {
                    ok = next_num (&s, &v[0]) && next_num (&s, &v[1]) && next_num (&s, &v[2]) && next_num (&s, &v[3])
                      && next_num (&s, &v[4]) && next_num (&s, &v[5]);
                    if (!ok)
                    {
                        break;
                    }
                    Pt c1 = {base.x + v[0], base.y + v[1]}, c2 = {base.x + v[2], base.y + v[3]},
                       p3 = {base.x + v[4], base.y + v[5]};
                    cubic (sp, cur, c1, c2, p3);
                    last_ctrl = c2;
                    cur       = p3;
                    break;
                }
            case 'S':
                {
                    ok = next_num (&s, &v[0]) && next_num (&s, &v[1]) && next_num (&s, &v[2]) && next_num (&s, &v[3]);
                    if (!ok)
                    {
                        break;
                    }
                    Pt c1 = (last_cmd == 'C' || last_cmd == 'S') ? Pt{2 * cur.x - last_ctrl.x, 2 * cur.y - last_ctrl.y}
                                                                 : cur;
                    Pt c2 = {base.x + v[0], base.y + v[1]}, p3 = {base.x + v[2], base.y + v[3]};
                    cubic (sp, cur, c1, c2, p3);
                    last_ctrl = c2;
                    cur       = p3;
                    break;
                }
            case 'Q':
                {
                    ok = next_num (&s, &v[0]) && next_num (&s, &v[1]) && next_num (&s, &v[2]) && next_num (&s, &v[3]);
                    if (!ok)
                    {
                        break;
                    }
                    Pt c1 = {base.x + v[0], base.y + v[1]}, p2 = {base.x + v[2], base.y + v[3]};
                    quad (sp, cur, c1, p2);
                    last_ctrl = c1;
                    cur       = p2;
                    break;
                }
            case 'T':
                {
                    ok = next_num (&s, &v[0]) && next_num (&s, &v[1]);
                    if (!ok)
                    {
                        break;
                    }
                    Pt c1 = (last_cmd == 'Q' || last_cmd == 'T') ? Pt{2 * cur.x - last_ctrl.x, 2 * cur.y - last_ctrl.y}
                                                                 : cur;
                    Pt p2 = {base.x + v[0], base.y + v[1]};
                    quad (sp, cur, c1, p2);
                    last_ctrl = c1;
                    cur       = p2;
                    break;
                }
            case 'A':
                {
                    bool large, sweep;
                    ok = next_num (&s, &v[0]) && next_num (&s, &v[1]) && next_num (&s, &v[2]) && next_flag (&s, &large)
                      && next_flag (&s, &sweep) && next_num (&s, &v[3]) && next_num (&s, &v[4]);
                    if (!ok)
                    {
                        break;
                    }
                    Pt p1 = {base.x + v[3], base.y + v[4]};
                    arc (sp, cur, v[0], v[1], v[2], large, sweep, p1);
                    cur = p1;
                    break;
                }
            default:
                ok = false;
                break;
            }
            if (!ok)
            {
                break;
            }
            last_cmd = c;
        }
    }

    void ellipse (std::vector<Subpath>* out, float cx, float cy, float rx, float ry)
    {
        Subpath sp;
        sp.closed = true;
        int n     = 48;
        for (int i = 0; i < n; i++)
        {
            float a = 2 * (float) M_PI * (float) i / (float) n;
            sp.pts.push_back (Pt{cx + rx * cosf (a), cy + ry * sinf (a)});
        }
        out->push_back (sp);
    }

    void rounded_rect (std::vector<Subpath>* out, float x, float y, float w, float h, float rx, float ry)
    {
        Subpath sp;
        sp.closed = true;
        rx        = fminf (rx, w / 2);
        ry        = fminf (ry, h / 2);
        if (rx <= 0 || ry <= 0)
        {
            sp.pts.push_back (Pt{x, y});
            sp.pts.push_back (Pt{x + w, y});
            sp.pts.push_back (Pt{x + w, y + h});
            sp.pts.push_back (Pt{x, y + h});
        }
        else
        {
            float cxs[4] = {x + w - rx, x + w - rx, x + rx, x + rx}, cys[4] = {y + ry, y + h - ry, y + h - ry, y + ry};
            for (int k = 0; k < 4; k++)
            {
                for (int i = 0; i <= 8; i++)
                {
                    float a = (float) M_PI * (-0.5f + 0.5f * (float) k) + (float) M_PI * 0.5f * (float) i / 8.0f;
                    sp.pts.push_back (Pt{cxs[k] + rx * cosf (a), cys[k] + ry * sinf (a)});
                }
            }
        }
        out->push_back (sp);
    }

    void points_list (const std::string& v, bool closed, std::vector<Subpath>* out)
    {
        Subpath sp;
        sp.closed     = closed;
        const char* s = v.c_str ();
        float x, y;
        while (next_num (&s, &x) && next_num (&s, &y))
        {
            sp.pts.push_back (Pt{x, y});
        }
        out->push_back (sp);
    }

    /* "translate(x y) scale(s)" (the forms the icon set uses). */
    void parse_transform (const std::string& t, Group* g)
    {
        const char* p = strstr (t.c_str (), "translate(");
        if (p != NULL)
        {
            p += 10;
            next_num (&p, &g->tx);
            next_num (&p, &g->ty);
        }
        p = strstr (t.c_str (), "scale(");
        if (p != NULL)
        {
            p += 6;
            next_num (&p, &g->scale);
        }
    }

    /* The SVG's shapes in 24 x 24 units. */
    void parse_svg (const char* svg, std::vector<Shape>* shapes)
    {
        const char* end_all = svg + strlen (svg);
        bool root_fill = false, root_stroke = true;
        float root_width = 2.0f;
        std::vector<Group> groups;
        for (const char* p = svg; (p = strchr (p, '<')) != NULL;)
        {
            const char* end = strchr (p, '>');
            if (end == NULL)
            {
                break;
            }
            const char* name = p + 1;
            size_t nlen      = 0;
            while (name + nlen < end && (isalpha ((unsigned char) name[nlen]) || name[nlen] == '/'))
            {
                nlen++;
            }
            std::string tag (name, nlen);
            std::string v;
            if (tag == "svg")
            {
                if (attr (p, end, "fill", &v))
                {
                    root_fill = v != "none";
                }
                if (attr (p, end, "stroke", &v))
                {
                    root_stroke = v != "none";
                }
                root_width = attr_num (p, end, "stroke-width", 2.0f);
            }
            else if (tag == "g")
            {
                Group g = {0, 0, 1, 0, false};
                if (!groups.empty ())
                {
                    g = groups.back ();
                }
                std::string t;
                Group local = {0, 0, 1, 0, false};
                if (attr (p, end, "transform", &t))
                {
                    parse_transform (t, &local);
                }
                g.tx += local.tx * g.scale;
                g.ty += local.ty * g.scale;
                g.scale *= local.scale;
                if (attr (p, end, "stroke-width", &v))
                {
                    g.width     = strtof (v.c_str (), NULL);
                    g.has_width = true;
                }
                groups.push_back (g);
            }
            else if (tag == "/g")
            {
                if (!groups.empty ())
                {
                    groups.pop_back ();
                }
            }
            else if (tag == "path" || tag == "circle" || tag == "ellipse" || tag == "rect" || tag == "line"
                     || tag == "polygon" || tag == "polyline")
            {
                Shape sh;
                sh.fill     = attr (p, end, "fill", &v) ? v != "none" : root_fill;
                sh.stroke   = attr (p, end, "stroke", &v) ? v != "none" : root_stroke;
                sh.evenodd  = attr (p, end, "fill-rule", &v) && v == "evenodd";
                float width = root_width;
                if (!groups.empty () && groups.back ().has_width)
                {
                    width = groups.back ().width;
                }
                sh.width = attr_num (p, end, "stroke-width", width);
                if (tag == "path")
                {
                    if (attr (p, end, "d", &v))
                    {
                        parse_path (v.c_str (), &sh.subpaths);
                    }
                }
                else if (tag == "circle")
                {
                    float r = attr_num (p, end, "r", 0);
                    ellipse (&sh.subpaths, attr_num (p, end, "cx", 0), attr_num (p, end, "cy", 0), r, r);
                }
                else if (tag == "ellipse")
                {
                    ellipse (&sh.subpaths, attr_num (p, end, "cx", 0), attr_num (p, end, "cy", 0),
                        attr_num (p, end, "rx", 0), attr_num (p, end, "ry", 0));
                }
                else if (tag == "rect")
                {
                    float rx = attr_num (p, end, "rx", -1), ry = attr_num (p, end, "ry", -1);
                    if (rx < 0)
                    {
                        rx = ry < 0 ? 0 : ry;
                    }
                    if (ry < 0)
                    {
                        ry = rx;
                    }
                    rounded_rect (&sh.subpaths, attr_num (p, end, "x", 0), attr_num (p, end, "y", 0),
                        attr_num (p, end, "width", 0), attr_num (p, end, "height", 0), rx, ry);
                }
                else if (tag == "line")
                {
                    Subpath sp;
                    sp.closed = false;
                    sp.pts.push_back (Pt{attr_num (p, end, "x1", 0), attr_num (p, end, "y1", 0)});
                    sp.pts.push_back (Pt{attr_num (p, end, "x2", 0), attr_num (p, end, "y2", 0)});
                    sh.subpaths.push_back (sp);
                }
                else if (attr (p, end, "points", &v))
                {
                    points_list (v, tag == "polygon", &sh.subpaths);
                }
                if (!groups.empty ())
                {
                    const Group& g = groups.back ();
                    for (size_t i = 0; i < sh.subpaths.size (); i++)
                    {
                        for (size_t k = 0; k < sh.subpaths[i].pts.size (); k++)
                        {
                            Pt& q = sh.subpaths[i].pts[k];
                            q.x   = g.tx + q.x * g.scale;
                            q.y   = g.ty + q.y * g.scale;
                        }
                    }
                    sh.width *= g.scale;
                }
                shapes->push_back (sh);
            }
            p = end + 1;
            if (p >= end_all)
            {
                break;
            }
        }
    }

    float segment_distance (float px, float py, Pt a, Pt b)
    {
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = dx * dx + dy * dy;
        float t   = len > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len : 0.0f;
        t         = t < 0 ? 0 : t > 1 ? 1 : t;
        float ex = a.x + t * dx - px, ey = a.y + t * dy - py;
        return sqrtf (ex * ex + ey * ey);
    }

    /* Winding number (non-zero) or crossing parity (even-odd) of (x, y) against the subpaths, all
       treated as closed for filling. */
    bool inside (const Shape& sh, float x, float y)
    {
        int winding = 0, crossings = 0;
        for (size_t i = 0; i < sh.subpaths.size (); i++)
        {
            const std::vector<Pt>& pts = sh.subpaths[i].pts;
            size_t n                   = pts.size ();
            for (size_t k = 0; k < n; k++)
            {
                Pt a = pts[k], b = pts[(k + 1) % n];
                if ((a.y <= y) != (b.y <= y))
                {
                    float xc = a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x);
                    if (xc > x)
                    {
                        crossings++;
                        winding += b.y > a.y ? 1 : -1;
                    }
                }
            }
        }
        return sh.evenodd ? (crossings & 1) != 0 : winding != 0;
    }

    /* The icon's coverage at px x px pixels (0..255 per pixel). */
    bool rasterize_icon (const char* svg, int px, std::vector<uint8_t>* alpha)
    {
        std::vector<Shape> shapes;
        parse_svg (svg, &shapes);
        if (shapes.empty () || px <= 0)
        {
            return false;
        }
        float k = (float) px / 24.0f;
        std::vector<float> cov ((size_t) px * px, 0.0f);
        for (size_t si = 0; si < shapes.size (); si++)
        {
            const Shape& sh = shapes[si];
            float half      = sh.width * k * 0.5f;
            for (int y = 0; y < px; y++)
            {
                for (int x = 0; x < px; x++)
                {
                    float c = 0.0f;
                    if (sh.fill)
                    {
                        int hits = 0;
                        for (int sy = 0; sy < 4; sy++)
                        {
                            for (int sx = 0; sx < 4; sx++)
                            {
                                float ux = ((float) x + ((float) sx + 0.5f) / 4.0f) / k,
                                      uy = ((float) y + ((float) sy + 0.5f) / 4.0f) / k;
                                if (inside (sh, ux, uy))
                                {
                                    hits++;
                                }
                            }
                        }
                        c = (float) hits / 16.0f;
                    }
                    if (sh.stroke && half > 0.0f)
                    {
                        float cx = ((float) x + 0.5f) / k, cy = ((float) y + 0.5f) / k;
                        float d = 1e9f;
                        for (size_t i = 0; i < sh.subpaths.size (); i++)
                        {
                            const std::vector<Pt>& pts = sh.subpaths[i].pts;
                            size_t n                   = pts.size ();
                            if (n == 1)
                            {
                                d = fminf (d, segment_distance (cx, cy, pts[0], pts[0]));
                            }
                            for (size_t j = 0; j + 1 < n; j++)
                            {
                                d = fminf (d, segment_distance (cx, cy, pts[j], pts[j + 1]));
                            }
                            if (sh.subpaths[i].closed && n > 2)
                            {
                                d = fminf (d, segment_distance (cx, cy, pts[n - 1], pts[0]));
                            }
                        }
                        float s = half + 0.5f - d * k;
                        s       = s < 0 ? 0 : s > 1 ? 1 : s;
                        c       = fmaxf (c, s);
                    }
                    float& dst = cov[(size_t) y * px + x];
                    dst        = 1.0f - (1.0f - dst) * (1.0f - c);
                }
            }
        }
        alpha->resize (cov.size ());
        for (size_t i = 0; i < cov.size (); i++)
        {
            (*alpha)[i] = (uint8_t) lroundf (fminf (cov[i], 1.0f) * 255.0f);
        }
        return true;
    }

    IconCache::IconCache () {}

    IconCache::~IconCache ()
    {
        clear ();
    }

    void IconCache::clear ()
    {
        for (size_t i = 0; i < entries_.size (); i++)
        {
            if (entries_[i].tex != 0)
            {
                glDeleteTextures (1, &entries_[i].tex);
            }
        }
        entries_.clear ();
    }

    /* The texture of icon `name` at px pixels (white, the coverage in alpha); 0 for an unknown
       icon. Made on first use. */
    unsigned int IconCache::texture (const char* name, int px)
    {
        for (size_t i = 0; i < entries_.size (); i++)
        {
            if (entries_[i].px == px && entries_[i].name == name)
            {
                return entries_[i].tex;
            }
        }
        Entry e = {name, px, 0};
        for (size_t i = 0; i < ICON_SOURCE_COUNT; i++)
        {
            if (strcmp (ICON_SOURCES[i].name, name) != 0)
            {
                continue;
            }
            std::vector<uint8_t> a;
            if (!rasterize_icon (ICON_SOURCES[i].svg, px, &a))
            {
                break;
            }
            std::vector<uint8_t> rgba (a.size () * 4);
            for (size_t k = 0; k < a.size (); k++)
            {
                rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = 255;
                rgba[k * 4 + 3]                                 = a[k];
            }
            glGenTextures (1, &e.tex);
            glBindTexture (GL_TEXTURE_2D, e.tex);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, px, px, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
            break;
        }
        entries_.push_back (e);
        return e.tex;
    }

    /* Icon `name` drawn size x size at pos (rasterized at the exact pixel size), tinted `color`. */
    void draw_icon (ImDrawList* dl, IconCache* cache, const char* name, ImVec2 pos, float size, ImU32 color)
    {
        int px           = (int) lroundf (size);
        unsigned int tex = cache->texture (name, px);
        if (tex == 0)
        {
            return;
        }
        ImVec2 a (floorf (pos.x), floorf (pos.y));
        dl->AddImage ((ImTextureID) (intptr_t) tex, a, ImVec2 (a.x + (float) px, a.y + (float) px), ImVec2 (0, 0),
            ImVec2 (1, 1), color);
    }

} // namespace ui
