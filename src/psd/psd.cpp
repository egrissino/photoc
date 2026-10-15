#include "psd/psd.h"

#include "compose/compose.h"
#include "psd/adjust_map.h"
#include "psd/bytes.h"
#include "psd/descriptor.h"
#include "psd/effects.h"
#include "psd/fill.h"
#include "psd/smart.h"
#include "psd/type.h"
#include "psd/vector.h"
#include "vec/vec.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace psd
{

    /* Photoshop documents (PSD, and PSB for reading): RGB, Grayscale, CMYK and Lab at 8, 16 and 32
       bits. Pixel layers with masks, groups, blend modes, opacity and fill, clipping, visibility,
       locks, colour labels, layer styles and editable type (TySh, see type.cpp); resolution, the
       ICC profile and alpha channels; paths, vector masks and solid-colour shape layers (vector.cpp);
       smart objects with their embedded files (smart.cpp; io opens the files). Tagged blocks this build doesn't
       interpret (adjustment, smart object and fill data, vector masks...) are kept on the layer and written back as
       they were, so a round trip through PhotoC keeps them for Photoshop; such layers show their stored pixels here. */

    using geom::Rect;

    const uint16_t RES_RESOLUTION          = 1005;
    const uint16_t RES_GUIDES              = 1032;
    const uint16_t RES_ALPHA_NAMES         = 1006;
    const uint16_t RES_ICC                 = 1039;
    const uint16_t RES_UNICODE_ALPHA_NAMES = 1045;
    const uint16_t RES_WORK_PATH           = 1025;
    const uint16_t RES_PATH_FIRST          = 2000;
    const uint16_t RES_PATH_LAST           = 2997;

    /* Blocks written from the document's own fields (never copied from psd_blocks). */
    const char* const OWN_KEYS[] = {
        "luni", "lsct", "lyid", "lfx2", "lfxs", "lrFX", "iOpa", "lspf", "lclr", "Lr16", "Lr32", "Layr"};

    /* Keys whose data marks an adjustment layer. */
    const char* const ADJUSTMENT_KEYS[] = {"levl", "curv", "hue2", "hue ", "brit", "blnc", "nvrt", "post", "thrs",
        "grdm", "selc", "blwh", "vibA", "expA", "mixr", "phfl", "clrL", "CgEd"};

    void set_err (char* err, size_t err_len, const char* msg)
    {
        if (err != NULL && err_len > 0)
        {
            snprintf (err, err_len, "%s", msg);
        }
    }

    bool is_psd (const uint8_t* data, size_t len)
    {
        return len >= 4 && memcmp (data, "8BPS", 4) == 0;
    }

    bool key_in (const std::string& key, const char* const* keys, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (memcmp (key.data (), keys[i], 4) == 0)
            {
                return true;
            }
        }
        return false;
    }

    /* PSB tagged blocks with 8-byte lengths. */
    bool long_key (const std::string& key)
    {
        const char* const keys[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2",
            "FEid", "FXid", "PxSD", "lnk3", "lnkE", "pths", "extd", "extn", "FELS", "cinf", "artd"};
        return key_in (key, keys, sizeof keys / sizeof keys[0]);
    }

    struct Block
    {
        std::string key;
        std::vector<uint8_t> data;
    };

    /* Tagged blocks to the end of the reader: signature, key, length, data (with padding that
       writers disagree on, found by looking for the next signature). */
    void read_blocks (Reader* r, bool psb, std::vector<Block>* out)
    {
        while (remaining (*r) >= 12)
        {
            std::string sig = rd_key4 (r);
            if (sig != "8BIM" && sig != "8B64")
            {
                break;
            }
            Block b;
            b.key      = rd_key4 (r);
            uint64_t n = psb && long_key (b.key) ? rd_u64 (r) : rd_u32 (r);
            if (n > remaining (*r))
            {
                break;
            }
            const uint8_t* p = rd_bytes (r, (size_t) n);
            b.data.assign (p, p + n);
            out->push_back (b);
            for (int k = 0; k < 3 && remaining (*r) >= 4; k++)
            {
                const uint8_t* next = r->data + r->pos;
                if (memcmp (next, "8BIM", 4) == 0 || memcmp (next, "8B64", 4) == 0)
                {
                    break;
                }
                rd_skip (r, 1);
            }
        }
    }

    const Block* find_block (const std::vector<Block>& blocks, const char* key)
    {
        for (size_t i = 0; i < blocks.size (); i++)
        {
            if (memcmp (blocks[i].key.data (), key, 4) == 0)
            {
                return &blocks[i];
            }
        }
        return NULL;
    }

    struct Channel
    {
        int16_t id;
        uint64_t len;
        std::vector<uint8_t> data;
    };

    struct Record
    {
        Rect rect;
        std::vector<Channel> channels;
        std::string blend;
        uint8_t opacity;
        uint8_t clipping;
        uint8_t flags;
        bool has_mask;
        Rect mask_rect;
        uint8_t mask_default;
        uint8_t mask_flags;
        float mask_density;
        float mask_feather;
        std::string name;
        std::vector<uint8_t> ranges;
        std::vector<Block> blocks;
    };

    Rect read_rect (Reader* r)
    {
        int32_t top = rd_i32 (r), left = rd_i32 (r), bottom = rd_i32 (r), right = rd_i32 (r);
        Rect out = {left, top, right, bottom};
        if (right < left || bottom < top)
        {
            out = Rect ();
        }
        return out;
    }

    /* The mask fields of a layer record's extra data. */
    void read_mask (const uint8_t* d, size_t n, Record* rec)
    {
        rec->has_mask = n >= 18;
        if (!rec->has_mask)
        {
            return;
        }
        Reader r          = reader (d, n);
        rec->mask_rect    = read_rect (&r);
        rec->mask_default = rd_u8 (&r);
        rec->mask_flags   = rd_u8 (&r);
        rec->mask_density = 1.0f;
        rec->mask_feather = 0.0f;
        if ((rec->mask_flags & 0x10) == 0)
        {
            return;
        }
        /* Parameters: after the real-mask fields when Photoshop wrote both (36 bytes in). */
        size_t at  = n >= 37 ? 36 : 18;
        Reader p   = reader (d + at, n - at);
        uint8_t pf = rd_u8 (&p);
        if (pf & 1)
        {
            rec->mask_density = rd_u8 (&p) / 255.0f;
        }
        if (pf & 2)
        {
            rec->mask_feather = (float) rd_f64 (&p);
        }
    }

    bool read_record (Reader* r, bool psb, Record* rec)
    {
        rec->rect    = read_rect (r);
        uint16_t nch = rd_u16 (r);
        if (nch > 56)
        {
            return false;
        }
        for (uint16_t i = 0; i < nch; i++)
        {
            Channel c;
            c.id  = rd_i16 (r);
            c.len = psb ? rd_u64 (r) : rd_u32 (r);
            rec->channels.push_back (c);
        }
        if (rd_key4 (r) != "8BIM")
        {
            return false;
        }
        rec->blend    = rd_key4 (r);
        rec->opacity  = rd_u8 (r);
        rec->clipping = rd_u8 (r);
        rec->flags    = rd_u8 (r);
        rd_u8 (r);
        uint32_t extra = rd_u32 (r);
        if (extra > remaining (*r))
        {
            return false;
        }
        Reader ex     = reader (rd_bytes (r, extra), extra);
        uint32_t mlen = rd_u32 (&ex);
        if (mlen > remaining (ex))
        {
            return false;
        }
        const uint8_t* m = rd_bytes (&ex, mlen);
        rec->has_mask    = false;
        read_mask (m, mlen, rec);
        uint32_t blen     = rd_u32 (&ex);
        const uint8_t* bp = rd_bytes (&ex, blen);
        if (bp != NULL)
        {
            rec->ranges.assign (bp, bp + blen);
        }
        rec->name = rd_pascal (&ex, 4);
        read_blocks (&ex, psb, &rec->blocks);
        return r->ok && ex.ok;
    }

    size_t sample_bytes (int depth)
    {
        return depth == 8 ? 1 : depth == 16 ? 2 : 4;
    }

    /* One channel's samples (w x h, big-endian) from its compressed data. */
    bool decode_channel (
        const uint8_t* src, size_t n, size_t w, size_t h, int depth, bool psb, std::vector<uint8_t>* out)
    {
        size_t row = w * sample_bytes (depth);
        out->assign (row * h, 0);
        if (w == 0 || h == 0)
        {
            return true;
        }
        if (n < 2)
        {
            return false;
        }
        uint16_t comp = (uint16_t) (src[0] << 8 | src[1]);
        src += 2;
        n -= 2;
        if (comp == 0)
        {
            if (n < out->size ())
            {
                return false;
            }
            memcpy (out->data (), src, out->size ());
            return true;
        }
        if (comp == 1)
        {
            size_t cbytes = psb ? 4 : 2;
            if (n < h * cbytes)
            {
                return false;
            }
            Reader cr = reader (src, h * cbytes);
            size_t at = h * cbytes;
            for (size_t y = 0; y < h; y++)
            {
                size_t len = psb ? rd_u32 (&cr) : rd_u16 (&cr);
                if (at + len > n || !packbits_decode (src + at, len, out->data () + y * row, row))
                {
                    return false;
                }
                at += len;
            }
            return true;
        }
        if (comp == 2 || comp == 3)
        {
            if (!zip_decode (src, n, out->data (), out->size ()))
            {
                return false;
            }
            if (comp == 3)
            {
                unpredict (out->data (), w, h, depth);
            }
            return true;
        }
        return false;
    }

    float sample_at (const uint8_t* p, int depth)
    {
        if (depth == 8)
        {
            return p[0] / 255.0f;
        }
        if (depth == 16)
        {
            return (float) (p[0] << 8 | p[1]) / 65535.0f;
        }
        uint32_t u = (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
        float f;
        memcpy (&f, &u, sizeof f);
        return f;
    }

    void put_sample (uint8_t* p, int depth, float v)
    {
        if (depth == 8)
        {
            p[0] = (uint8_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 255.0f);
        }
        else if (depth == 16)
        {
            uint16_t s = (uint16_t) lroundf (fminf (fmaxf (v, 0.0f), 1.0f) * 65535.0f);
            p[0]       = (uint8_t) (s >> 8);
            p[1]       = (uint8_t) s;
        }
        else
        {
            uint32_t u;
            memcpy (&u, &v, sizeof u);
            for (int i = 0; i < 4; i++)
            {
                p[i] = (uint8_t) (u >> (24 - 8 * i));
            }
        }
    }

    struct SmartSize
    {
        std::string uuid;
        uint32_t width;
        uint32_t height;
    };

    struct ReadCtx
    {
        std::vector<SmartSize> smart_sizes;
        doc::IdSource* ids;
        color::PixelFormat fmt;
        int depth;
        bool cmyk_mode;
        bool psb;
        size_t colors;
        Rect canvas;
        std::vector<std::string>* warnings;
        bool warned_kinds;
        bool warned_adjust;
        text::FontDb* fonts;
        float dpi;
        color::ColorMode mode;
        std::vector<std::string> missing_fonts;
    };

    const Channel* channel_of (const Record& rec, int16_t id)
    {
        for (size_t i = 0; i < rec.channels.size (); i++)
        {
            if (rec.channels[i].id == id)
            {
                return &rec.channels[i];
            }
        }
        return NULL;
    }

    /* A record's pixels and mask on a layer. */
    void fill_layer (const ReadCtx& cx, const Record& rec, doc::Layer* l)
    {
        Rect r   = rec.rect;
        size_t w = r.width (), h = r.height (), bps = sample_bytes (cx.depth);
        l->pixels = raster::Surface (cx.fmt);
        if (w > 0 && h > 0)
        {
            size_t n = cx.fmt.channels (), count = w * h;
            std::vector<float> px (count * n, 0.0f);
            for (size_t c = 0; c < n; c++)
            {
                bool alpha        = c + 1 == n;
                const Channel* ch = channel_of (rec, alpha ? (int16_t) -1 : (int16_t) c);
                for (size_t i = 0; i < count; i++)
                {
                    float v = ch != NULL && ch->data.size () >= count * bps ? sample_at (&ch->data[i * bps], cx.depth)
                            : alpha                                         ? 1.0f
                                                                            : 0.0f;
                    if (cx.cmyk_mode && !alpha && ch != NULL)
                    {
                        v = 1.0f - v;
                    }
                    px[i * n + c] = v;
                }
            }
            l->pixels.write_region (r, px.data (), px.size ());
            l->pixels.prune ();
        }
        if (rec.has_mask)
        {
            const Channel* mc = channel_of (rec, -2);
            float def         = rec.mask_default / 255.0f;
            l->mask.surface   = raster::Surface::with_default (color::PixelFormat::gray8 (), &def);
            Rect mr           = rec.mask_rect;
            size_t mw = mr.width (), mh = mr.height ();
            if (mc != NULL && mw > 0 && mh > 0 && mc->data.size () >= mw * mh * bps)
            {
                std::vector<float> mv (mw * mh);
                for (size_t i = 0; i < mv.size (); i++)
                {
                    mv[i] = sample_at (&mc->data[i * bps], cx.depth);
                }
                l->mask.surface.write_region (mr, mv.data (), mv.size ());
                l->mask.surface.prune ();
            }
            l->has_mask     = true;
            l->mask.enabled = (rec.mask_flags & 2) == 0;
            l->mask.linked  = (rec.mask_flags & 1) == 0;
            l->mask.density = rec.mask_density;
            l->mask.feather = rec.mask_feather;
        }
    }

    /* Properties shared by layers and groups: name, blend, opacity, fill, flags, locks, label,
       style, and the blocks kept for writing back. */
    void apply_props (ReadCtx* cx, const Record& rec, doc::Layer* l)
    {
        l->name           = rec.name;
        const Block* luni = find_block (rec.blocks, "luni");
        if (luni != NULL)
        {
            Reader r      = reader (luni->data.data (), luni->data.size ());
            std::string u = rd_unicode (&r);
            if (r.ok && !u.empty ())
            {
                l->name = u;
            }
        }
        color::BlendMode m;
        if (blend_from_key (rec.blend.c_str (), &m))
        {
            l->blend = m;
        }
        l->opacity = rec.opacity / 255.0f;
        /* Blend If: 8 bytes a channel (this layer's black, black split, white split, white, then the
           underlying layers'), Gray first, then the colour channels (kept for RGB). */
        for (size_t c = 0; c < 4 && (c + 1) * 8 <= rec.ranges.size (); c++)
        {
            if (c > 0 && cx->mode != color::ColorMode::Rgb)
            {
                break;
            }
            doc::BlendRange& r = c == 0 ? l->blend_if.gray : l->blend_if.channels[c - 1];
            const uint8_t* q   = &rec.ranges[c * 8];
            r.this_black[0]    = q[0];
            r.this_black[1]    = q[1];
            r.this_white[0]    = q[2];
            r.this_white[1]    = q[3];
            r.under_black[0]   = q[4];
            r.under_black[1]   = q[5];
            r.under_white[0]   = q[6];
            r.under_white[1]   = q[7];
        }
        l->clipped            = rec.clipping != 0;
        l->visible            = (rec.flags & 2) == 0;
        l->locks.transparency = (rec.flags & 1) != 0;
        const Block* b        = find_block (rec.blocks, "iOpa");
        if (b != NULL && !b->data.empty ())
        {
            l->fill_opacity = b->data[0] / 255.0f;
        }
        b = find_block (rec.blocks, "lspf");
        if (b != NULL && b->data.size () >= 4)
        {
            uint32_t f =
                (uint32_t) b->data[0] << 24 | (uint32_t) b->data[1] << 16 | (uint32_t) b->data[2] << 8 | b->data[3];
            l->locks.transparency = l->locks.transparency || (f & 1) != 0;
            l->locks.pixels       = (f & 2) != 0;
            l->locks.position     = (f & 4) != 0;
            l->locks.all          = (f & 0x80000000u) != 0;
        }
        b = find_block (rec.blocks, "lclr");
        if (b != NULL && b->data.size () >= 2)
        {
            uint16_t c = (uint16_t) (b->data[0] << 8 | b->data[1]);
            if (c < doc::LABEL_COLOR_COUNT)
            {
                l->label = doc::LABEL_COLORS[c];
            }
        }
        b = find_block (rec.blocks, "lfx2");
        if (b == NULL)
        {
            b = find_block (rec.blocks, "lfxs");
        }
        if (b != NULL && !parse_lfx2 (b->data.data (), b->data.size (), &l->style))
        {
            cx->warnings->push_back ("layer \"" + l->name + "\": its layer style could not be read");
        }
        b = find_block (rec.blocks, "lyid");
        if (b != NULL && b->data.size () >= 4)
        {
            l->has_psd_id = true;
            l->psd_id =
                (uint32_t) b->data[0] << 24 | (uint32_t) b->data[1] << 16 | (uint32_t) b->data[2] << 8 | b->data[3];
        }
        const char* const kept_note[] = {"PtFl"};
        for (size_t i = 0; i < rec.blocks.size (); i++)
        {
            const Block& k = rec.blocks[i];
            if (key_in (k.key, OWN_KEYS, sizeof OWN_KEYS / sizeof OWN_KEYS[0]))
            {
                continue;
            }
            doc::PsdBlock pb;
            memcpy (pb.key, k.key.data (), 4);
            pb.data = std::make_shared<const std::vector<uint8_t>> (k.data);
            l->psd_blocks.push_back (pb);
            if (!cx->warned_kinds && key_in (k.key, kept_note, sizeof kept_note / sizeof kept_note[0]))
            {
                cx->warned_kinds = true;
                cx->warnings->push_back ("pattern fill layers show their stored pixels (they are kept for Photoshop)");
            }
        }
    }

    bool is_adjustment (const Record& rec)
    {
        for (size_t i = 0; i < rec.blocks.size (); i++)
        {
            if (key_in (rec.blocks[i].key, ADJUSTMENT_KEYS, sizeof ADJUSTMENT_KEYS / sizeof ADJUSTMENT_KEYS[0]))
            {
                return true;
            }
        }
        return false;
    }

    /* A solid colour fill layer (SoCo) is drawn as its colour over the canvas. */
    void solid_fill (const ReadCtx& cx, const Record& rec, doc::Layer* l, const color::CmykSpace* cmyk)
    {
        const Block* b = find_block (rec.blocks, "SoCo");
        if (b == NULL)
        {
            return;
        }
        Reader r = reader (b->data.data (), b->data.size ());
        rd_u32 (&r);
        DValue d;
        if (!parse_descriptor (&r, &d))
        {
            return;
        }
        const DValue* c = d_get (d, "Clr ");
        if (c == NULL)
        {
            return;
        }
        float rgba[4] = {(float) d_num (*c, "Rd  ", 0) / 255.0f, (float) d_num (*c, "Grn ", 0) / 255.0f,
            (float) d_num (*c, "Bl  ", 0) / 255.0f, 1.0f};
        float px[raster::MAX_CHANNELS];
        raster::from_rgba (cx.fmt, rgba, cmyk, px);
        l->pixels = raster::Surface (cx.fmt);
        l->pixels.fill_rect (cx.canvas, px);
    }

    bool vector_key (const char* key, bool shape)
    {
        if (strcmp (key, "vmsk") == 0 || strcmp (key, "vsms") == 0)
        {
            return true;
        }
        return shape
            && (strcmp (key, "SoCo") == 0 || strcmp (key, "vstk") == 0 || strcmp (key, "vscg") == 0
                || strcmp (key, "vogk") == 0);
    }

    void drop_vector_blocks (doc::Layer* l, bool shape)
    {
        std::vector<doc::PsdBlock> kept;
        for (size_t k = 0; k < l->psd_blocks.size (); k++)
        {
            char key[5] = {
                l->psd_blocks[k].key[0], l->psd_blocks[k].key[1], l->psd_blocks[k].key[2], l->psd_blocks[k].key[3], 0};
            if (!vector_key (key, shape))
            {
                kept.push_back (l->psd_blocks[k]);
            }
        }
        l->psd_blocks = kept;
    }

    /* A vector mask: with a solid colour fill the layer is a shape layer (drawn from its path),
       otherwise the layer's vector mask. */
    void apply_vectors (ReadCtx* cx, const Record& rec, doc::Layer* l, const color::CmykSpace* cmyk)
    {
        const Block* vm = find_block (rec.blocks, "vmsk");
        if (vm == NULL)
        {
            vm = find_block (rec.blocks, "vsms");
        }
        if (vm == NULL)
        {
            return;
        }
        uint32_t w = cx->canvas.width (), h = cx->canvas.height ();
        doc::VectorMask m = l->vector_mask;
        if (!parse_vector_mask (vm->data.data (), vm->data.size (), w, h, &m))
        {
            cx->warnings->push_back ("layer \"" + l->name + "\": its vector mask could not be read");
            return;
        }
        const Block* soco = find_block (rec.blocks, "SoCo");
        float fill[3];
        if (soco != NULL && l->kind == doc::LayerKind::Raster
            && parse_solid_color (soco->data.data (), soco->data.size (), fill))
        {
            l->kind       = doc::LayerKind::Shape;
            l->shape      = doc::ShapeLayer::make ();
            l->shape.path = m.path;
            memcpy (l->shape.fill, fill, sizeof fill);
            const Block* st = find_block (rec.blocks, "vstk");
            if (st != NULL)
            {
                parse_stroke_style (st->data.data (), st->data.size (), &l->shape.stroke, &l->shape.has_fill);
            }
            l->pixels = vec::render_shape (l->shape, cx->fmt, cmyk, cx->canvas);
            drop_vector_blocks (l, true);
            return;
        }
        l->has_vector_mask     = true;
        l->vector_mask         = m;
        l->vector_mask.surface = vec::vector_mask_surface (m.path, cx->canvas);
        drop_vector_blocks (l, false);
    }

    /* Layer info: the records (bottom first), then every record's channel data. Section dividers
       open and close groups. */
    bool read_layer_info (ReadCtx* cx, Reader* r, const color::CmykSpace* cmyk, std::vector<doc::Layer>* out,
        std::vector<Record>* records)
    {
        int16_t count = rd_i16 (r);
        size_t n      = (size_t) (count < 0 ? -count : count);
        for (size_t i = 0; i < n; i++)
        {
            Record rec;
            if (!read_record (r, cx->psb, &rec))
            {
                return false;
            }
            records->push_back (rec);
        }
        for (size_t i = 0; i < records->size (); i++)
        {
            Record& rec = (*records)[i];
            for (size_t c = 0; c < rec.channels.size (); c++)
            {
                Channel& ch = rec.channels[c];
                if (ch.len > remaining (*r))
                {
                    return false;
                }
                const uint8_t* p = rd_bytes (r, (size_t) ch.len);
                Rect cr          = ch.id == -2 || ch.id == -3 ? rec.mask_rect : rec.rect;
                if (ch.id < -1 && !rec.has_mask)
                {
                    continue;
                }
                if (!decode_channel (p, (size_t) ch.len, cr.width (), cr.height (), cx->depth, cx->psb, &ch.data))
                {
                    cx->warnings->push_back ("layer \"" + rec.name + "\": a channel could not be decoded");
                    ch.data.clear ();
                }
            }
        }
        /* Build the tree: a divider (lsct 3) starts a group's children, its folder record (1, 2)
           closes it. */
        std::vector<std::vector<doc::Layer>> stack (1);
        for (size_t i = 0; i < records->size (); i++)
        {
            const Record& rec = (*records)[i];
            const Block* sct  = find_block (rec.blocks, "lsct");
            uint32_t type     = 0;
            if (sct != NULL && sct->data.size () >= 4)
            {
                type = (uint32_t) sct->data[0] << 24 | (uint32_t) sct->data[1] << 16 | (uint32_t) sct->data[2] << 8
                     | sct->data[3];
            }
            if (type == 3)
            {
                if (stack.size () <= doc::MAX_GROUP_DEPTH)
                {
                    stack.push_back (std::vector<doc::Layer> ());
                }
                continue;
            }
            if ((type == 1 || type == 2) && stack.size () > 1)
            {
                doc::Layer g = doc::Layer::group (*cx->ids, rec.name, stack.back ());
                stack.pop_back ();
                apply_props (cx, rec, &g);
                g.expanded = type == 1;
                if (sct->data.size () >= 12 && memcmp (&sct->data[4], "8BIM", 4) == 0)
                {
                    color::BlendMode m;
                    if (blend_from_key ((const char*) &sct->data[8], &m))
                    {
                        g.blend = m;
                    }
                }
                if (rec.has_mask)
                {
                    fill_layer (*cx, rec, &g);
                }
                g.pixels = raster::Surface ();
                apply_vectors (cx, rec, &g, cmyk);
                stack.back ().push_back (g);
                continue;
            }
            doc::Layer l = doc::Layer::raster (*cx->ids, rec.name, cx->fmt);
            fill_layer (*cx, rec, &l);
            apply_props (cx, rec, &l);
            /* Type: editable, showing Photoshop's rendering until it is edited. */
            const Block* tysh = find_block (rec.blocks, "TySh");
            if (tysh != NULL && cx->fonts != NULL
                && tysh_to_text (
                    tysh->data.data (), tysh->data.size (), cx->dpi, cx->fonts, &l.text, &cx->missing_fonts))
            {
                l.kind = doc::LayerKind::Text;
            }
            if (is_adjustment (rec))
            {
                /* An adjustment layer: its settings from the block, which then isn't kept raw. */
                const Block* cged = find_block (rec.blocks, "CgEd");
                for (size_t k = 0; k < rec.blocks.size (); k++)
                {
                    if (!is_adjustment_key (rec.blocks[k].key.c_str ()))
                    {
                        continue;
                    }
                    l.kind       = doc::LayerKind::Adjustment;
                    l.adjustment = parse_adjustment (
                        rec.blocks[k].key, rec.blocks[k].data, cged != NULL ? &cged->data : NULL, cx->mode);
                    l.pixels = raster::Surface ();
                    break;
                }
                std::vector<doc::PsdBlock> kept;
                for (size_t k = 0; k < l.psd_blocks.size (); k++)
                {
                    char key[5] = {l.psd_blocks[k].key[0], l.psd_blocks[k].key[1], l.psd_blocks[k].key[2],
                        l.psd_blocks[k].key[3], 0};
                    if (!is_adjustment_key (key) && strcmp (key, "CgEd") != 0)
                    {
                        kept.push_back (l.psd_blocks[k]);
                    }
                }
                l.psd_blocks = kept;
                if (l.adjustment.kind == doc::AdjustmentKind::Unsupported && !cx->warned_adjust)
                {
                    cx->warned_adjust = true;
                    cx->warnings->push_back ("some adjustment layers (pencil curves, color lookups...) are kept for "
                                             "Photoshop but not applied here");
                }
            }
            solid_fill (*cx, rec, &l, cmyk);
            apply_vectors (cx, rec, &l, cmyk);
            /* Solid colour and gradient fill layers become fill layers again (their stored pixels
               aren't needed). */
            if (l.kind == doc::LayerKind::Raster)
            {
                const Block *soco = find_block (rec.blocks, "SoCo"), *gdfl = find_block (rec.blocks, "GdFl");
                doc::FillLayer f = doc::FillLayer::make ();
                bool is_fill     = false;
                if (soco != NULL && parse_solid_color (soco->data.data (), soco->data.size (), f.color))
                {
                    f.kind  = doc::FillKind::Solid;
                    is_fill = true;
                }
                else if (gdfl != NULL && parse_gradient_fill (gdfl->data.data (), gdfl->data.size (), &f))
                {
                    is_fill = true;
                }
                if (is_fill)
                {
                    l.kind   = doc::LayerKind::Fill;
                    l.fill   = f;
                    l.pixels = raster::Surface ();
                    std::vector<doc::PsdBlock> kept;
                    for (size_t k = 0; k < l.psd_blocks.size (); k++)
                    {
                        if (memcmp (l.psd_blocks[k].key, "SoCo", 4) != 0
                            && memcmp (l.psd_blocks[k].key, "GdFl", 4) != 0)
                        {
                            kept.push_back (l.psd_blocks[k]);
                        }
                    }
                    l.psd_blocks = kept;
                }
            }
            /* A smart object: its placement; the pixels Photoshop stored show until it changes. */
            const Block *sold = find_block (rec.blocks, "SoLd"), *plld = find_block (rec.blocks, "PlLd");
            if ((sold != NULL || plld != NULL) && l.kind == doc::LayerKind::Raster)
            {
                uint32_t sw, sh;
                if (parse_smart_layer (sold ? sold->data.data () : NULL, sold ? sold->data.size () : 0,
                        plld ? plld->data.data () : NULL, plld ? plld->data.size () : 0, &l.smart, &sw, &sh))
                {
                    l.kind = doc::LayerKind::Smart;
                    cx->smart_sizes.push_back (SmartSize{l.smart.uuid, sw, sh});
                    for (size_t k = 0; k < l.smart.filters.size (); k++)
                    {
                        if (l.smart.filters[k].id.empty ())
                        {
                            cx->warnings->push_back (
                                "layer \"" + l.name + "\": smart filter \"" + l.smart.filters[k].name
                                + "\" isn't available here; it's kept but skipped once the layer is redrawn");
                        }
                    }
                    std::vector<doc::PsdBlock> kept;
                    for (size_t k = 0; k < l.psd_blocks.size (); k++)
                    {
                        char key[5] = {l.psd_blocks[k].key[0], l.psd_blocks[k].key[1], l.psd_blocks[k].key[2],
                            l.psd_blocks[k].key[3], 0};
                        if (strcmp (key, "SoLd") != 0 && strcmp (key, "PlLd") != 0 && strcmp (key, "SoLE") != 0)
                        {
                            kept.push_back (l.psd_blocks[k]);
                        }
                    }
                    l.psd_blocks = kept;
                }
            }
            stack.back ().push_back (l);
        }
        while (stack.size () > 1)
        {
            std::vector<doc::Layer> rest = stack.back ();
            stack.pop_back ();
            stack.back ().insert (stack.back ().end (), rest.begin (), rest.end ());
        }
        *out = stack[0];
        return true;
    }

    bool read_document (doc::IdSource& ids, const std::string& name, const uint8_t* data, size_t len,
        const color::CmykSpace* cmyk, text::FontDb* fonts, doc::Document* out, std::vector<std::string>* warnings,
        char* err, size_t err_len)
    {
        Reader r = reader (data, len);
        if (rd_key4 (&r) != "8BPS")
        {
            set_err (err, err_len, "not a Photoshop file");
            return false;
        }
        uint16_t version = rd_u16 (&r);
        rd_skip (&r, 6);
        uint16_t channels = rd_u16 (&r);
        uint32_t height = rd_u32 (&r), width = rd_u32 (&r);
        uint16_t depth = rd_u16 (&r), mode = rd_u16 (&r);
        if (!r.ok || (version != 1 && version != 2) || width == 0 || height == 0 || width > 300000 || height > 300000)
        {
            set_err (err, err_len, "the Photoshop file header is damaged");
            return false;
        }
        color::ColorMode cm;
        size_t colors;
        if (mode == 1)
        {
            cm = color::ColorMode::Grayscale, colors = 1;
        }
        else if (mode == 0)
        {
            cm = color::ColorMode::Bitmap, colors = 1;
        }
        else if (mode == 2)
        {
            cm = color::ColorMode::Indexed, colors = 1;
        }
        else if (mode == 8)
        {
            cm = color::ColorMode::Duotone, colors = 1;
        }
        else if (mode == 3)
        {
            cm = color::ColorMode::Rgb, colors = 3;
        }
        else if (mode == 4)
        {
            cm = color::ColorMode::Cmyk, colors = 4;
        }
        else if (mode == 9)
        {
            cm = color::ColorMode::Lab, colors = 3;
        }
        else
        {
            const char* names[] = {
                "Bitmap", "Grayscale", "Indexed Color", "RGB", "CMYK", "", "", "Multichannel", "Duotone", "Lab"};
            char msg[128];
            snprintf (msg, sizeof msg, "%s Photoshop documents can't be opened yet",
                mode < 10 ? names[mode] : "this kind of");
            set_err (err, err_len, msg);
            return false;
        }
        if ((depth != 8 && depth != 16 && depth != 32 && !(depth == 1 && cm == color::ColorMode::Bitmap))
            || (depth != 8 && cm == color::ColorMode::Indexed))
        {
            set_err (err, err_len, "only 8, 16 and 32 bits per channel Photoshop files can be opened");
            return false;
        }
        color::SampleType st = depth <= 8  ? color::SampleType::U8
                             : depth == 16 ? color::SampleType::U16
                                           : color::SampleType::F32;
        doc::Document d      = doc::Document::make (ids, name, geom::Size{width, height}, cm, st);
        bool psb             = version == 2;

        /* Color mode data: an Indexed image's palette (256 reds, greens, then blues), a Duotone's
           ink block (kept as it is, its inks shown as their gray plate). */
        uint32_t cmd_len        = rd_u32 (&r);
        const uint8_t* cmd_data = rd_bytes (&r, cmd_len);
        if (cm == color::ColorMode::Indexed && cmd_data != NULL && cmd_len >= 768)
        {
            d.has_color_table = true;
            d.color_table.rgb.resize (256 * 3);
            for (size_t i = 0; i < 256; i++)
            {
                for (size_t c = 0; c < 3; c++)
                {
                    d.color_table.rgb[i * 3 + c] = cmd_data[c * 256 + i];
                }
            }
        }
        if (cm == color::ColorMode::Duotone)
        {
            d.has_duotone = true;
            d.duotone.inks.push_back (doc::DuotoneInk::make ("Black", 0.0f, 0.0f, 0.0f));
            if (cmd_data != NULL)
            {
                d.duotone.psd_raw.assign (cmd_data, cmd_data + cmd_len);
            }
            warnings->push_back (
                "Duotone inks are not interpreted yet: the image shows as its gray plate (the inks are "
                "kept for saving)");
        }

        /* Image resources: resolution, ICC profile, alpha channel names. */
        uint32_t res_len = rd_u32 (&r);
        Reader res       = reader (rd_bytes (&r, res_len), res_len);
        std::vector<std::string> alpha_names;
        while (res.ok && remaining (res) >= 12)
        {
            if (rd_key4 (&res) != "8BIM")
            {
                break;
            }
            uint16_t id          = rd_u16 (&res);
            std::string res_name = rd_pascal (&res, 2);
            uint32_t n           = rd_u32 (&res);
            const uint8_t* p     = rd_bytes (&res, n);
            if (n & 1)
            {
                rd_skip (&res, 1);
            }
            if (p == NULL)
            {
                break;
            }
            if (id == RES_RESOLUTION && n >= 16)
            {
                Reader rr        = reader (p, n);
                double hres      = rd_u32 (&rr) / 65536.0;
                uint16_t unit    = rd_u16 (&rr);
                d.resolution_dpi = (float) (unit == 2 ? hres * 2.54 : hres);
            }
            else if (id == RES_GUIDES && n >= 16)
            {
                /* Grid and guides: version, two grid cycles, count, then each guide at 1/32 px. */
                Reader rr = reader (p, n);
                rd_u32 (&rr);
                rd_u32 (&rr);
                rd_u32 (&rr);
                uint32_t count = rd_u32 (&rr);
                for (uint32_t g = 0; g < count && rr.ok && remaining (rr) >= 5; g++)
                {
                    int32_t at  = rd_i32 (&rr);
                    uint8_t dir = rd_u8 (&rr);
                    d.guides.push_back (doc::Guide{dir == 0, at / 32.0});
                }
            }
            else if (id == RES_WORK_PATH)
            {
                d.has_work_path =
                    parse_path_records (p, n, width, height, &d.work_path) && !d.work_path.subpaths.empty ();
            }
            else if (id >= RES_PATH_FIRST && id <= RES_PATH_LAST)
            {
                doc::NamedPath np;
                np.name = res_name.empty () ? "Path" : res_name;
                if (parse_path_records (p, n, width, height, &np.path))
                {
                    d.paths.push_back (np);
                }
            }
            else if (id == 1047 && n >= 2)
            {
                /* The Indexed image's transparent entry. */
                Reader rr                 = reader (p, n);
                d.color_table.transparent = rd_u16 (&rr);
            }
            else if (id == RES_ICC && n > 0)
            {
                d.icc_profile = std::make_shared<const std::vector<uint8_t>> (p, p + n);
            }
            else if (id == RES_UNICODE_ALPHA_NAMES)
            {
                Reader rr = reader (p, n);
                alpha_names.clear ();
                while (rr.ok && remaining (rr) >= 4)
                {
                    alpha_names.push_back (rd_unicode (&rr));
                }
            }
            else if (id == RES_ALPHA_NAMES && alpha_names.empty ())
            {
                Reader rr = reader (p, n);
                while (rr.ok && remaining (rr) >= 1)
                {
                    alpha_names.push_back (rd_pascal (&rr, 1));
                }
            }
        }

        ReadCtx cx;
        cx.ids           = &ids;
        cx.fmt           = d.pixel_format ();
        cx.depth         = depth;
        cx.cmyk_mode     = cm == color::ColorMode::Cmyk;
        cx.psb           = psb;
        cx.colors        = colors;
        cx.canvas        = d.bounds ();
        cx.warnings      = warnings;
        cx.warned_kinds  = false;
        cx.warned_adjust = false;
        cx.fonts         = fonts;
        cx.dpi           = d.resolution_dpi;
        cx.mode          = cm;

        /* Layer and mask information. */
        uint64_t lm_len = psb ? rd_u64 (&r) : rd_u32 (&r);
        if (lm_len > remaining (r))
        {
            set_err (err, err_len, "the Photoshop file is truncated");
            return false;
        }
        Reader lm         = reader (rd_bytes (&r, (size_t) lm_len), (size_t) lm_len);
        bool merged_alpha = false;
        std::vector<Record> records;
        if (lm_len > 0)
        {
            uint64_t li_len = psb ? rd_u64 (&lm) : rd_u32 (&lm);
            if (li_len > remaining (lm))
            {
                li_len = remaining (lm);
            }
            if (li_len > 0)
            {
                Reader li    = reader (rd_bytes (&lm, (size_t) li_len), (size_t) li_len);
                merged_alpha = li_len >= 2 && (int16_t) (li.data[0] << 8 | li.data[1]) < 0;
                if (!read_layer_info (&cx, &li, cmyk, &d.layers, &records))
                {
                    set_err (err, err_len, "the Photoshop file's layers are damaged");
                    return false;
                }
            }
            uint32_t gm = rd_u32 (&lm);
            rd_skip (&lm, gm);
            std::vector<Block> globals;
            read_blocks (&lm, psb, &globals);
            for (size_t i = 0; i < globals.size (); i++)
            {
                if (globals[i].key == "lnk2" || globals[i].key == "lnkD" || globals[i].key == "lnk3")
                {
                    parse_linked_files (globals[i].data.data (), globals[i].data.size (), &d.smart);
                }
            }
            for (size_t i = 0; i < globals.size () && records.empty (); i++)
            {
                if (globals[i].key != "Lr16" && globals[i].key != "Lr32" && globals[i].key != "Layr")
                {
                    continue;
                }
                Reader li = reader (globals[i].data.data (), globals[i].data.size ());
                merged_alpha =
                    globals[i].data.size () >= 2 && (int16_t) (globals[i].data[0] << 8 | globals[i].data[1]) < 0;
                if (!read_layer_info (&cx, &li, cmyk, &d.layers, &records))
                {
                    set_err (err, err_len, "the Photoshop file's layers are damaged");
                    return false;
                }
            }
        }

        for (size_t i = 0; i < cx.smart_sizes.size (); i++)
        {
            doc::SmartContents* c = d.smart_contents_mut (cx.smart_sizes[i].uuid);
            if (c != NULL && c->width == 0)
            {
                c->width  = cx.smart_sizes[i].width;
                c->height = cx.smart_sizes[i].height;
            }
        }

        /* The merged image: the document when it has no layers, and its alpha channels. */
        size_t w = width, h = height, bps = sample_bytes (depth), row = depth == 1 ? (w + 7) / 8 : w * bps,
               plane = row * h;
        std::vector<std::vector<uint8_t>> merged (channels);
        uint16_t comp  = rd_u16 (&r);
        bool merged_ok = r.ok;
        if (merged_ok && comp == 1)
        {
            size_t cbytes = psb ? 4 : 2;
            std::vector<size_t> lens (h * channels);
            for (size_t i = 0; i < lens.size (); i++)
            {
                lens[i] = psb ? rd_u32 (&r) : rd_u16 (&r);
            }
            (void) cbytes;
            for (size_t c = 0; c < channels && merged_ok; c++)
            {
                merged[c].assign (plane, 0);
                for (size_t y = 0; y < h && merged_ok; y++)
                {
                    size_t n         = lens[c * h + y];
                    const uint8_t* p = rd_bytes (&r, n);
                    merged_ok        = p != NULL && packbits_decode (p, n, merged[c].data () + y * row, row);
                }
            }
        }
        else if (merged_ok && comp == 0)
        {
            for (size_t c = 0; c < channels && merged_ok; c++)
            {
                const uint8_t* p = rd_bytes (&r, plane);
                merged_ok        = p != NULL;
                if (merged_ok)
                {
                    merged[c].assign (p, p + plane);
                }
            }
        }
        else if (merged_ok && (comp == 2 || comp == 3))
        {
            /* One ZIP stream over all channels. */
            std::vector<uint8_t> all (plane * channels);
            merged_ok = zip_decode (r.data + r.pos, remaining (r), all.data (), all.size ());
            for (size_t c = 0; c < channels && merged_ok; c++)
            {
                merged[c].assign (all.begin () + (ptrdiff_t) (c * plane), all.begin () + (ptrdiff_t) ((c + 1) * plane));
                if (comp == 3 && depth != 1)
                {
                    unpredict (merged[c].data (), w, h, depth);
                }
            }
        }
        else
        {
            merged_ok = false;
        }
        if (d.layers.empty ())
        {
            if (!merged_ok)
            {
                set_err (err, err_len, "the Photoshop file has no layers and its image could not be read");
                return false;
            }
            bool clear_entry = cm == color::ColorMode::Indexed && d.color_table.transparent >= 0;
            doc::Layer bg    = doc::Layer::raster (ids, clear_entry ? "Index" : "Background", cx.fmt);
            size_t n         = cx.fmt.channels ();
            std::vector<float> px (w * h * n);
            for (size_t i = 0; i < w * h; i++)
            {
                px[i * n + n - 1] = 1.0f;
                if (cm == color::ColorMode::Indexed)
                {
                    /* Each index through the palette. */
                    size_t k = merged[0][i];
                    float rgb[3];
                    d.color_table.color (k, rgb);
                    for (size_t c = 0; c < 3; c++)
                    {
                        px[i * n + c] = rgb[c];
                    }
                    if (clear_entry && (int) k == d.color_table.transparent)
                    {
                        px[i * n + n - 1] = 0.0f;
                    }
                    continue;
                }
                if (depth == 1)
                {
                    /* One bit a pixel, rows padded to a byte; 1 is black. */
                    size_t x = i % w, y = i / w;
                    px[i * n] = (merged[0][y * row + x / 8] >> (7 - x % 8) & 1) != 0 ? 0.0f : 1.0f;
                    continue;
                }
                for (size_t c = 0; c < colors; c++)
                {
                    float v       = sample_at (&merged[c][i * bps], depth);
                    px[i * n + c] = cx.cmyk_mode ? 1.0f - v : v;
                }
            }
            bg.pixels.write_region (d.bounds (), px.data (), px.size ());
            bg.pixels.prune ();
            bg.locks.transparency = !clear_entry;
            bg.locks.position     = !clear_entry;
            d.layers.push_back (bg);
        }
        else if (!records.empty () && (records[0].flags & 1) != 0 && channel_of (records[0], -1) == NULL
                 && d.layers[0].kind == doc::LayerKind::Raster)
        {
            /* Photoshop's Background: the bottom record, transparency-protected, without a
               transparency channel. */
            d.layers[0].locks.transparency = true;
            d.layers[0].locks.position     = true;
        }
        if (merged_ok && depth != 1)
        {
            size_t first = colors + (merged_alpha ? 1 : 0);
            size_t skip  = merged_alpha && alpha_names.size () == channels - colors ? 1 : 0;
            for (size_t c = first; c < channels; c++)
            {
                doc::AlphaChannel a;
                size_t k = c - first + skip;
                char fallback[32];
                snprintf (fallback, sizeof fallback, "Alpha %zu", c - first + 1);
                a.name    = k < alpha_names.size () && !alpha_names[k].empty () ? alpha_names[k] : fallback;
                a.surface = raster::Surface (color::PixelFormat::gray8 ());
                std::vector<float> v (w * h);
                for (size_t i = 0; i < v.size (); i++)
                {
                    v[i] = sample_at (&merged[c][i * bps], depth);
                }
                a.surface.write_region (d.bounds (), v.data (), v.size ());
                a.surface.prune ();
                d.channels.push_back (a);
            }
        }
        else
        {
            warnings->push_back ("the file's merged image could not be read (alpha channels are missing)");
        }
        if (!cx.missing_fonts.empty ())
        {
            std::string list;
            for (size_t i = 0; i < cx.missing_fonts.size () && i < 6; i++)
            {
                list += (i > 0 ? ", " : "") + cx.missing_fonts[i];
            }
            if (cx.missing_fonts.size () > 6)
            {
                list += ", …";
            }
            warnings->push_back ("fonts not installed (a substitute is used when the type is edited): " + list);
        }
        *out = d;
        return true;
    }

    /* ---- Writing ---- */

    struct WriteCtx
    {
        const doc::Document* d;
        const color::CmykSpace* cmyk;
        int depth;
        bool cmyk_mode;
        size_t colors;
        uint32_t next_id;
        text::FontDb* fonts;
        std::vector<uint8_t> records;
        std::vector<uint8_t> channel_data;
        int count;
    };

    /* One channel's samples (w x h of float values) compressed: RLE at 8 bits, ZIP with
       prediction at 16 and 32. */
    void encode_channel (const std::vector<float>& v, size_t w, size_t h, int depth, std::vector<uint8_t>* out)
    {
        size_t bps = sample_bytes (depth), row = w * bps;
        std::vector<uint8_t> raw (row * h);
        for (size_t i = 0; i < w * h; i++)
        {
            put_sample (&raw[i * bps], depth, v[i]);
        }
        if (w == 0 || h == 0)
        {
            wr_u16 (out, 0);
            return;
        }
        if (depth == 8)
        {
            wr_u16 (out, 1);
            std::vector<std::vector<uint8_t>> rows (h);
            for (size_t y = 0; y < h; y++)
            {
                rows[y] = packbits_encode (&raw[y * row], row);
                wr_u16 (out, (uint16_t) rows[y].size ());
            }
            for (size_t y = 0; y < h; y++)
            {
                wr_bytes (out, rows[y].data (), rows[y].size ());
            }
            return;
        }
        predict (raw.data (), w, h, depth);
        std::vector<uint8_t> z = zip_encode (raw.data (), raw.size ());
        wr_u16 (out, 3);
        wr_bytes (out, z.data (), z.size ());
    }

    void write_block (std::vector<uint8_t>* w, const char* key, const std::vector<uint8_t>& data)
    {
        wr_key4 (w, "8BIM");
        wr_key4 (w, key);
        size_t padded = (data.size () + 3) & ~(size_t) 3;
        wr_u32 (w, (uint32_t) padded);
        wr_bytes (w, data.data (), data.size ());
        for (size_t i = data.size (); i < padded; i++)
        {
            wr_u8 (w, 0);
        }
    }

    /* A layer record and its channel data. `section`: 0 a layer, 1/2 an open/closed group record,
       3 a group's bottom divider. */
    void write_record (WriteCtx* cx, const doc::Layer& l, int section)
    {
        std::vector<uint8_t>& w = cx->records;
        Rect r;
        bool pixels = section == 0 && l.surface () != NULL;
        if (pixels)
        {
            r = l.surface ()->content_bounds ();
        }
        size_t n = cx->d->pixel_format ().channels ();
        struct Chan
        {
            int16_t id;
            std::vector<uint8_t> data;
        };
        std::vector<Chan> chans;
        size_t pw = r.width (), ph = r.height ();
        std::vector<float> vals;
        if (pixels && pw > 0 && ph > 0)
        {
            l.surface ()->read_region (r, &vals);
        }
        color::PixelFormat lf = pixels ? l.surface ()->format () : cx->d->pixel_format ();
        size_t ln             = lf.channels ();
        for (size_t c = 0; c < n; c++)
        {
            bool alpha = c + 1 == n;
            Chan ch;
            ch.id = alpha ? -1 : (int16_t) c;
            std::vector<float> v (pw * ph, alpha ? 0.0f : 0.0f);
            for (size_t i = 0; i < v.size () && !vals.empty (); i++)
            {
                float x = alpha ? (lf.alpha ? vals[i * ln + ln - 1] : 1.0f) : vals[i * ln + c];
                v[i]    = cx->cmyk_mode && !alpha ? 1.0f - x : x;
            }
            encode_channel (v, pw, ph, cx->depth, &ch.data);
            chans.push_back (ch);
        }
        Rect mr;
        uint8_t mdef = 255;
        if (l.has_mask && section != 3)
        {
            mr = l.mask.surface.content_bounds ();
            float dp[raster::MAX_CHANNELS];
            l.mask.surface.default_pixel (dp);
            mdef = dp[0] >= 0.5f ? 255 : 0;
            Chan ch;
            ch.id = -2;
            std::vector<float> mv;
            if (!mr.is_empty ())
            {
                l.mask.surface.read_region (mr, &mv);
            }
            encode_channel (mv, mr.width (), mr.height (), cx->depth, &ch.data);
            chans.push_back (ch);
        }

        wr_u32 (&w, (uint32_t) r.y0);
        wr_u32 (&w, (uint32_t) r.x0);
        wr_u32 (&w, (uint32_t) r.y1);
        wr_u32 (&w, (uint32_t) r.x1);
        wr_u16 (&w, (uint16_t) chans.size ());
        for (size_t i = 0; i < chans.size (); i++)
        {
            wr_u16 (&w, (uint16_t) chans[i].id);
            wr_u32 (&w, (uint32_t) chans[i].data.size ());
            wr_bytes (&cx->channel_data, chans[i].data.data (), chans[i].data.size ());
        }
        wr_key4 (&w, "8BIM");
        bool group = section == 1 || section == 2;
        wr_key4 (&w, group && l.blend == color::BlendMode::PassThrough ? "norm" : blend_key (l.blend));
        wr_u8 (&w, (uint8_t) lroundf (fminf (fmaxf (l.opacity, 0.0f), 1.0f) * 255.0f));
        wr_u8 (&w, l.clipped ? 1 : 0);
        uint8_t flags = 0x08;
        if (l.locks.transparency)
        {
            flags |= 1;
        }
        if (!l.visible)
        {
            flags |= 2;
        }
        wr_u8 (&w, flags);
        wr_u8 (&w, 0);
        size_t extra_at = w.size ();
        wr_u32 (&w, 0);
        /* Mask data. */
        if (l.has_mask && section != 3)
        {
            bool params = l.mask.density < 1.0f || l.mask.feather > 0.0f;
            std::vector<uint8_t> m;
            wr_u32 (&m, (uint32_t) mr.y0);
            wr_u32 (&m, (uint32_t) mr.x0);
            wr_u32 (&m, (uint32_t) mr.y1);
            wr_u32 (&m, (uint32_t) mr.x1);
            wr_u8 (&m, mdef);
            uint8_t mf = 0;
            if (!l.mask.linked)
            {
                mf |= 1;
            }
            if (!l.mask.enabled)
            {
                mf |= 2;
            }
            if (params)
            {
                mf |= 0x10;
            }
            wr_u8 (&m, mf);
            if (params)
            {
                uint8_t pf = (l.mask.density < 1.0f ? 1 : 0) | (l.mask.feather > 0.0f ? 2 : 0);
                wr_u8 (&m, pf);
                if (pf & 1)
                {
                    wr_u8 (&m, (uint8_t) lroundf (l.mask.density * 255.0f));
                }
                if (pf & 2)
                {
                    wr_f64 (&m, l.mask.feather);
                }
            }
            while (m.size () < 20 || m.size () % 4 != 0)
            {
                wr_u8 (&m, 0);
            }
            wr_u32 (&w, (uint32_t) m.size ());
            wr_bytes (&w, m.data (), m.size ());
        }
        else
        {
            wr_u32 (&w, 0);
        }
        /* Blending ranges (Blend If): gray, then each channel. */
        wr_u32 (&w, (uint32_t) (8 * (cx->colors + 1)));
        for (size_t i = 0; i < cx->colors + 1; i++)
        {
            bool rgb_channel =
                i > 0 && i <= 3 && (cx->d->mode == color::ColorMode::Rgb || cx->d->mode == color::ColorMode::Indexed);
            const doc::BlendRange* r = i == 0 ? &l.blend_if.gray : rgb_channel ? &l.blend_if.channels[i - 1] : NULL;
            if (r == NULL)
            {
                wr_u32 (&w, 0x0000FFFF);
                wr_u32 (&w, 0x0000FFFF);
                continue;
            }
            const float* v[4] = {r->this_black, r->this_white, r->under_black, r->under_white};
            for (int k = 0; k < 4; k++)
            {
                wr_u8 (&w, (uint8_t) lroundf (fminf (fmaxf (v[k][0], 0.0f), 255.0f)));
                wr_u8 (&w, (uint8_t) lroundf (fminf (fmaxf (v[k][1], 0.0f), 255.0f)));
            }
        }
        std::string name = section == 3 ? "</Layer group>" : l.name;
        wr_pascal (&w, name, 4);
        std::vector<uint8_t> b;
        wr_unicode (&b, name, false);
        write_block (&w, "luni", b);
        b.clear ();
        wr_u32 (&b, l.has_psd_id && section != 3 ? l.psd_id : cx->next_id++);
        write_block (&w, "lyid", b);
        if (section != 0)
        {
            b.clear ();
            wr_u32 (&b, (uint32_t) section);
            if (group)
            {
                wr_key4 (&b, "8BIM");
                wr_key4 (&b, blend_key (l.blend));
            }
            write_block (&w, "lsct", b);
        }
        if (section != 3)
        {
            b.clear ();
            wr_u8 (&b, (uint8_t) lroundf (fminf (fmaxf (l.fill_opacity, 0.0f), 1.0f) * 255.0f));
            wr_u8 (&b, 0);
            wr_u8 (&b, 0);
            wr_u8 (&b, 0);
            write_block (&w, "iOpa", b);
            b.clear ();
            uint32_t lk = (l.locks.transparency ? 1u : 0u) | (l.locks.pixels ? 2u : 0u) | (l.locks.position ? 4u : 0u)
                        | (l.locks.all ? 0x80000000u : 0u);
            wr_u32 (&b, lk);
            write_block (&w, "lspf", b);
            b.clear ();
            size_t label = 0;
            for (size_t i = 0; i < doc::LABEL_COLOR_COUNT; i++)
            {
                if (doc::LABEL_COLORS[i] == l.label)
                {
                    label = i;
                }
            }
            wr_u16 (&b, label < 8 ? (uint16_t) label : 0);
            for (int i = 0; i < 6; i++)
            {
                wr_u8 (&b, 0);
            }
            write_block (&w, "lclr", b);
            if (!l.style.items.empty ())
            {
                write_block (&w, "lfx2", write_lfx2 (l.style));
            }
            /* Type: its TySh, from the model (patching the original one). */
            const std::vector<uint8_t>* old_tysh = NULL;
            for (size_t i = 0; i < l.psd_blocks.size (); i++)
            {
                if (memcmp (l.psd_blocks[i].key, "TySh", 4) == 0 && l.psd_blocks[i].data)
                {
                    old_tysh = l.psd_blocks[i].data.get ();
                }
            }
            if (l.kind == doc::LayerKind::Text)
            {
                write_block (&w, "TySh", text_to_tysh (l.text, old_tysh, cx->d->resolution_dpi, cx->fonts));
            }
            uint32_t cw = cx->d->size.width, ch = cx->d->size.height;
            if (l.kind == doc::LayerKind::Smart)
            {
                const doc::SmartContents* c = cx->d->smart_contents (l.smart.uuid);
                uint32_t sw = c != NULL ? c->width : 0, sh = c != NULL ? c->height : 0;
                write_block (&w, "SoLd", write_sold (l.smart, sw, sh, cx->d->resolution_dpi));
            }
            if (l.kind == doc::LayerKind::Shape)
            {
                write_block (&w, "SoCo", write_solid_color (l.shape.fill));
                doc::VectorMask vm = l.vector_mask;
                vm.path            = l.shape.path;
                vm.enabled         = true;
                vm.linked          = true;
                write_block (&w, "vmsk", write_vector_mask (vm, cw, ch));
                write_block (&w, "vstk", write_stroke_style (l.shape.stroke, l.shape.has_fill));
                std::vector<uint8_t> sc;
                wr_key4 (&sc, "SoCo");
                std::vector<uint8_t> scd = write_solid_color (l.shape.stroke.color);
                wr_bytes (&sc, scd.data (), scd.size ());
                write_block (&w, "vscg", sc);
            }
            else if (l.has_vector_mask)
            {
                write_block (&w, "vmsk", write_vector_mask (l.vector_mask, cw, ch));
            }
            if (l.kind == doc::LayerKind::Fill && l.fill.kind == doc::FillKind::Solid)
            {
                write_block (&w, "SoCo", write_solid_color (l.fill.color));
            }
            if (l.kind == doc::LayerKind::Fill && l.fill.kind == doc::FillKind::Gradient)
            {
                write_block (&w, "GdFl", write_gradient_fill (l.fill));
            }
            if (l.kind == doc::LayerKind::Adjustment)
            {
                std::vector<AdjBlock> adj = write_adjustment (l.adjustment);
                for (size_t i = 0; i < adj.size (); i++)
                {
                    write_block (&w, adj[i].key.c_str (), adj[i].data);
                }
            }
            for (size_t i = 0; i < l.psd_blocks.size (); i++)
            {
                char key[5] = {
                    l.psd_blocks[i].key[0], l.psd_blocks[i].key[1], l.psd_blocks[i].key[2], l.psd_blocks[i].key[3], 0};
                if (l.kind == doc::LayerKind::Text && strcmp (key, "TySh") == 0)
                {
                    continue;
                }
                if (l.kind == doc::LayerKind::Adjustment && (is_adjustment_key (key) || strcmp (key, "CgEd") == 0))
                {
                    continue;
                }
                if ((l.kind == doc::LayerKind::Shape || l.has_vector_mask)
                    && vector_key (key, l.kind == doc::LayerKind::Shape))
                {
                    continue;
                }
                if (l.kind == doc::LayerKind::Smart
                    && (strcmp (key, "SoLd") == 0 || strcmp (key, "PlLd") == 0 || strcmp (key, "SoLE") == 0))
                {
                    continue;
                }
                if (l.psd_blocks[i].data)
                {
                    write_block (&w, key, *l.psd_blocks[i].data);
                }
            }
        }
        put_u32_at (&w, extra_at, (uint32_t) (w.size () - extra_at - 4));
        cx->count++;
    }

    void write_layers (WriteCtx* cx, const std::vector<doc::Layer>& layers)
    {
        for (size_t i = 0; i < layers.size (); i++)
        {
            const doc::Layer& l = layers[i];
            if (l.is_group ())
            {
                write_record (cx, l, 3);
                write_layers (cx, l.children);
                write_record (cx, l, l.expanded ? 1 : 2);
            }
            else
            {
                write_record (cx, l, 0);
            }
        }
    }

    /* Pattern fill layers as pixel layers (writing Photoshop's pattern data isn't supported yet):
       their pattern rendered over the canvas, the mask kept. */
    void bake_pattern_fills (
        const doc::Document& d, std::vector<doc::Layer>* layers, const color::CmykSpace* cmyk, bool* any)
    {
        for (size_t i = 0; i < layers->size (); i++)
        {
            doc::Layer& l = (*layers)[i];
            bake_pattern_fills (d, &l.children, cmyk, any);
            if (l.kind != doc::LayerKind::Fill || l.fill.kind != doc::FillKind::Pattern)
            {
                continue;
            }
            compose::RenderSettings settings = compose::default_settings ();
            settings.cmyk                    = cmyk;
            Rect canvas                      = d.bounds ();
            compose::Buffer b                = compose::fill_layer_pixels (d, l, canvas, settings);
            color::PixelFormat fmt           = d.pixel_format ();
            size_t n = fmt.channels (), count = b.pixel_count ();
            std::vector<float> data (count * n);
            for (size_t k = 0; k < count; k++)
            {
                raster::from_rgba (fmt, &b.px[k * 4], cmyk, &data[k * n]);
            }
            l.kind   = doc::LayerKind::Raster;
            l.pixels = raster::Surface (fmt);
            l.pixels.write_region (canvas, data.data (), data.size ());
            l.pixels.prune ();
            *any = true;
        }
    }

    bool write_document (const doc::Document& d, const color::CmykSpace* cmyk, text::FontDb* fonts,
        std::vector<uint8_t>* out, std::vector<std::string>* warnings, char* err, size_t err_len)
    {
        uint16_t mode;
        size_t colors;
        /* Indexed Color saves as RGB and Bitmap as Grayscale (their pixels are stored expanded);
           Duotone saves as Duotone with the ink block it was opened with, else as Grayscale. */
        bool duo_raw = d.mode == color::ColorMode::Duotone && d.has_duotone && !d.duotone.psd_raw.empty ();
        switch (d.mode)
        {
        case color::ColorMode::Indexed:
            mode = 3, colors = 3;
            warnings->push_back ("Indexed Color saved as RGB");
            break;
        case color::ColorMode::Bitmap:
            mode = 1, colors = 1;
            warnings->push_back ("Bitmap saved as Grayscale");
            break;
        case color::ColorMode::Duotone:
            mode = duo_raw ? 8 : 1, colors = 1;
            if (!duo_raw)
            {
                warnings->push_back ("Duotone saved as Grayscale (writing Photoshop's ink block isn't supported yet)");
            }
            break;
        case color::ColorMode::Grayscale:
            mode = 1, colors = 1;
            break;
        case color::ColorMode::Rgb:
            mode = 3, colors = 3;
            break;
        case color::ColorMode::Cmyk:
            mode = 4, colors = 4;
            break;
        case color::ColorMode::Lab:
            mode = 9, colors = 3;
            break;
        default:
            set_err (err, err_len, "this colour mode can't be saved as PSD yet");
            return false;
        }
        if (d.size.width > 30000 || d.size.height > 30000)
        {
            set_err (
                err, err_len, "PSD files are limited to 30000 px a side (large documents need PSB, not ported yet)");
            return false;
        }
        int depth               = d.depth == color::SampleType::U8 ? 8 : d.depth == color::SampleType::U16 ? 16 : 32;
        bool has_bg             = d.background () != NULL;
        size_t total            = colors + (has_bg ? 0 : 1) + d.channels.size ();
        std::vector<uint8_t>& w = *out;
        w.clear ();
        wr_key4 (&w, "8BPS");
        wr_u16 (&w, 1);
        for (int i = 0; i < 6; i++)
        {
            wr_u8 (&w, 0);
        }
        wr_u16 (&w, (uint16_t) total);
        wr_u32 (&w, d.size.height);
        wr_u32 (&w, d.size.width);
        wr_u16 (&w, (uint16_t) depth);
        wr_u16 (&w, mode);
        if (duo_raw)
        {
            wr_u32 (&w, (uint32_t) d.duotone.psd_raw.size ());
            w.insert (w.end (), d.duotone.psd_raw.begin (), d.duotone.psd_raw.end ());
        }
        else
        {
            wr_u32 (&w, 0);
        }

        /* Resources: resolution, ICC profile, alpha channel names. */
        std::vector<uint8_t> res;
        {
            std::vector<uint8_t> v;
            uint32_t fixed = (uint32_t) lround (d.resolution_dpi * 65536.0);
            wr_u32 (&v, fixed);
            wr_u16 (&v, 1);
            wr_u16 (&v, 1);
            wr_u32 (&v, fixed);
            wr_u16 (&v, 1);
            wr_u16 (&v, 1);
            wr_key4 (&res, "8BIM");
            wr_u16 (&res, RES_RESOLUTION);
            wr_u16 (&res, 0);
            wr_u32 (&res, (uint32_t) v.size ());
            wr_bytes (&res, v.data (), v.size ());
        }
        if (!d.guides.empty ())
        {
            std::vector<uint8_t> v;
            wr_u32 (&v, 1);
            wr_u32 (&v, 576);
            wr_u32 (&v, 576);
            wr_u32 (&v, (uint32_t) d.guides.size ());
            for (size_t i = 0; i < d.guides.size (); i++)
            {
                wr_u32 (&v, (uint32_t) (int32_t) lround (d.guides[i].position * 32.0));
                wr_u8 (&v, d.guides[i].vertical ? 0 : 1);
            }
            wr_key4 (&res, "8BIM");
            wr_u16 (&res, RES_GUIDES);
            wr_u16 (&res, 0);
            wr_u32 (&res, (uint32_t) v.size ());
            wr_bytes (&res, v.data (), v.size ());
            if (v.size () & 1)
            {
                wr_u8 (&res, 0);
            }
        }
        for (size_t i = 0; i <= d.paths.size (); i++)
        {
            bool work = i == d.paths.size ();
            if (work && !d.has_work_path)
            {
                break;
            }
            if (!work && i > (size_t) (RES_PATH_LAST - RES_PATH_FIRST))
            {
                continue;
            }
            std::vector<uint8_t> v;
            write_path_records (work ? d.work_path : d.paths[i].path, d.size.width, d.size.height, &v);
            wr_key4 (&res, "8BIM");
            wr_u16 (&res, work ? RES_WORK_PATH : (uint16_t) (RES_PATH_FIRST + i));
            wr_pascal (&res, work ? std::string () : d.paths[i].name, 2);
            wr_u32 (&res, (uint32_t) v.size ());
            wr_bytes (&res, v.data (), v.size ());
            if (v.size () & 1)
            {
                wr_u8 (&res, 0);
            }
        }
        if (d.icc_profile && !d.icc_profile->empty ())
        {
            wr_key4 (&res, "8BIM");
            wr_u16 (&res, RES_ICC);
            wr_u16 (&res, 0);
            wr_u32 (&res, (uint32_t) d.icc_profile->size ());
            wr_bytes (&res, d.icc_profile->data (), d.icc_profile->size ());
            if (d.icc_profile->size () & 1)
            {
                wr_u8 (&res, 0);
            }
        }
        if (!d.channels.empty ())
        {
            std::vector<uint8_t> uni, pas;
            for (size_t i = 0; i < d.channels.size (); i++)
            {
                wr_unicode (&uni, d.channels[i].name, true);
                wr_pascal (&pas, d.channels[i].name, 1);
            }
            wr_key4 (&res, "8BIM");
            wr_u16 (&res, RES_UNICODE_ALPHA_NAMES);
            wr_u16 (&res, 0);
            wr_u32 (&res, (uint32_t) uni.size ());
            wr_bytes (&res, uni.data (), uni.size ());
            if (uni.size () & 1)
            {
                wr_u8 (&res, 0);
            }
            wr_key4 (&res, "8BIM");
            wr_u16 (&res, RES_ALPHA_NAMES);
            wr_u16 (&res, 0);
            wr_u32 (&res, (uint32_t) pas.size ());
            wr_bytes (&res, pas.data (), pas.size ());
            if (pas.size () & 1)
            {
                wr_u8 (&res, 0);
            }
        }
        wr_u32 (&w, (uint32_t) res.size ());
        wr_bytes (&w, res.data (), res.size ());

        /* Layers. 16 and 32-bit files keep them in a global Lr16 / Lr32 block, as Photoshop does. */
        WriteCtx cx;
        cx.d                             = &d;
        cx.cmyk                          = cmyk;
        cx.depth                         = depth;
        cx.cmyk_mode                     = d.mode == color::ColorMode::Cmyk;
        cx.colors                        = colors;
        cx.next_id                       = 1;
        cx.fonts                         = fonts;
        cx.count                         = 0;
        std::vector<doc::WalkEntry> walk = d.walk ();
        for (size_t i = 0; i < walk.size (); i++)
        {
            if (walk[i].layer->has_psd_id && walk[i].layer->psd_id >= cx.next_id)
            {
                cx.next_id = walk[i].layer->psd_id + 1;
            }
        }
        std::vector<doc::WalkEntry> all = d.walk ();
        for (size_t i = 0; i < all.size (); i++)
        {
            bool pattern_fx = false;
            for (size_t k = 0; k < all[i].layer->style.items.size (); k++)
            {
                pattern_fx = pattern_fx || all[i].layer->style.items[k].kind == doc::FxKind::PatternOverlay;
            }
            if (pattern_fx)
            {
                warnings->push_back ("Pattern Overlay effects aren't saved in PSD yet");
                break;
            }
        }
        std::vector<doc::Layer> layers = d.layers;
        bool baked                     = false;
        bake_pattern_fills (d, &layers, cmyk, &baked);
        if (baked)
        {
            warnings->push_back ("pattern fill layers saved as pixel layers");
        }
        write_layers (&cx, layers);
        std::vector<uint8_t> info;
        wr_u16 (&info, (uint16_t) (int16_t) (has_bg ? cx.count : -cx.count));
        wr_bytes (&info, cx.records.data (), cx.records.size ());
        wr_bytes (&info, cx.channel_data.data (), cx.channel_data.size ());
        while (info.size () % 4 != 0)
        {
            wr_u8 (&info, 0);
        }
        std::vector<uint8_t> lm;
        if (depth == 8)
        {
            wr_u32 (&lm, (uint32_t) info.size ());
            wr_bytes (&lm, info.data (), info.size ());
            wr_u32 (&lm, 0);
        }
        else
        {
            wr_u32 (&lm, 0);
            wr_u32 (&lm, 0);
            write_block (&lm, depth == 16 ? "Lr16" : "Lr32", info);
        }
        /* The smart objects' files, once each: as they were embedded, or the edited contents saved
           as a Photoshop document. */
        std::vector<uint8_t> links;
        for (size_t i = 0; i < d.smart.size (); i++)
        {
            const doc::SmartContents& c = d.smart[i];
            bool used                   = false;
            for (size_t k = 0; k < walk.size () && !used; k++)
            {
                used = walk[k].layer->kind == doc::LayerKind::Smart && walk[k].layer->smart.uuid == c.uuid;
            }
            if (!used)
            {
                continue;
            }
            if (c.bytes)
            {
                write_linked_file (c.uuid, c.filename, c.filetype, *c.bytes, &links);
            }
            else if (c.doc)
            {
                std::vector<uint8_t> inner;
                if (!write_document (*c.doc, cmyk, fonts, &inner, warnings, err, err_len))
                {
                    return false;
                }
                std::string name = c.filename;
                size_t dot       = name.rfind ('.');
                name             = (dot != std::string::npos ? name.substr (0, dot) : name) + ".psd";
                write_linked_file (c.uuid, name, "8BPS", inner, &links);
            }
        }
        if (!links.empty ())
        {
            write_block (&lm, "lnk2", links);
        }
        if (lm.size () > 0xFFFFFFF0u)
        {
            set_err (err, err_len, "the layers are too large for a PSD file");
            return false;
        }
        wr_u32 (&w, (uint32_t) lm.size ());
        wr_bytes (&w, lm.data (), lm.size ());

        /* The merged image: the composite (matted with white where transparent, as Photoshop
           stores it), its transparency, then the alpha channels. RLE at every depth. */
        compose::RenderSettings settings = compose::default_settings ();
        settings.cmyk                    = cmyk;
        color::PixelFormat fmt           = d.pixel_format ();
        raster::Surface comp             = compose::composite_to_surface (d, d.bounds (), fmt, NULL, settings);
        std::vector<float> vals;
        comp.read_region (d.bounds (), &vals);
        size_t pw = d.size.width, ph = d.size.height, n = fmt.channels (), bps = sample_bytes (depth), row = pw * bps;
        float white[raster::MAX_CHANNELS], one[4] = {1, 1, 1, 1};
        raster::from_rgba (fmt, one, cmyk, white);
        std::vector<std::vector<uint8_t>> planes (total, std::vector<uint8_t> (row * ph));
        for (size_t i = 0; i < pw * ph; i++)
        {
            float a = vals[i * n + n - 1];
            for (size_t c = 0; c < colors; c++)
            {
                float v = vals[i * n + c] * a + white[c] * (1.0f - a);
                put_sample (&planes[c][i * bps], depth, cx.cmyk_mode ? 1.0f - v : v);
            }
            if (!has_bg)
            {
                put_sample (&planes[colors][i * bps], depth, a);
            }
        }
        for (size_t k = 0; k < d.channels.size (); k++)
        {
            std::vector<float> av;
            d.channels[k].surface.read_region (d.bounds (), &av);
            size_t c = colors + (has_bg ? 0 : 1) + k;
            for (size_t i = 0; i < pw * ph && i < av.size (); i++)
            {
                put_sample (&planes[c][i * bps], depth, av[i]);
            }
        }
        wr_u16 (&w, 1);
        std::vector<std::vector<uint8_t>> rows (total * ph);
        for (size_t c = 0; c < total; c++)
        {
            for (size_t y = 0; y < ph; y++)
            {
                rows[c * ph + y] = packbits_encode (&planes[c][y * row], row);
            }
        }
        for (size_t i = 0; i < rows.size (); i++)
        {
            wr_u16 (&w, (uint16_t) rows[i].size ());
        }
        for (size_t i = 0; i < rows.size (); i++)
        {
            wr_bytes (&w, rows[i].data (), rows[i].size ());
        }
        (void) warnings;
        return true;
    }

} // namespace psd
