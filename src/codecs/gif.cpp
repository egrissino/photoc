#include "codecs/codecs.h"
#include <algorithm>
#include <gif_lib.h>
#include <string.h>

namespace codecs
{

    /* GIF through giflib. Reading takes the first frame on the logical screen as 8-bit RGBA
       (transparent where the frame doesn't reach or its transparent index is). Writing reduces the
       colours to a 255-entry palette by median cut (index 255 is transparent when the image has
       pixels under half opacity), without dithering. */

    struct GifIn
    {
        const uint8_t* data;
        size_t len;
        size_t pos;
    };

    int gif_read (GifFileType* g, GifByteType* buf, int n)
    {
        GifIn* in = (GifIn*) g->UserData;
        size_t k  = std::min ((size_t) n, in->len - in->pos);
        memcpy (buf, in->data + in->pos, k);
        in->pos += k;
        return (int) k;
    }

    int gif_write (GifFileType* g, const GifByteType* buf, int n)
    {
        std::vector<uint8_t>* out = (std::vector<uint8_t>*) g->UserData;
        out->insert (out->end (), buf, buf + n);
        return n;
    }

    bool gif_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        GifIn in       = {bytes, len, 0};
        int e          = 0;
        GifFileType* g = DGifOpen (&in, gif_read, &e);
        if (g == NULL)
        {
            set_error (err, err_len, "GIF: %s", GifErrorString (e));
            return false;
        }
        if (DGifSlurp (g) != GIF_OK || g->ImageCount < 1)
        {
            set_error (err, err_len, "GIF: %s", GifErrorString (g->Error));
            DGifCloseFile (g, &e);
            return false;
        }
        uint32_t w = (uint32_t) g->SWidth, h = (uint32_t) g->SHeight;
        const SavedImage& im = g->SavedImages[0];
        if (w == 0 || h == 0)
        {
            w = (uint32_t) im.ImageDesc.Width;
            h = (uint32_t) im.ImageDesc.Height;
        }
        if (!limits.check (w, h, ChannelLayout::Rgba, Sample::U8, err, err_len))
        {
            DGifCloseFile (g, &e);
            return false;
        }
        const ColorMapObject* map = im.ImageDesc.ColorMap != NULL ? im.ImageDesc.ColorMap : g->SColorMap;
        GraphicsControlBlock gcb;
        int transparent = -1;
        if (DGifSavedExtensionToGCB (g, 0, &gcb) == GIF_OK)
        {
            transparent = gcb.TransparentColor;
        }
        std::vector<uint8_t> data ((size_t) w * h * 4, 0);
        int fx = im.ImageDesc.Left, fy = im.ImageDesc.Top, fw = im.ImageDesc.Width, fh = im.ImageDesc.Height;
        for (int y = 0; y < fh && map != NULL; y++)
        {
            for (int x = 0; x < fw; x++)
            {
                int X = fx + x, Y = fy + y;
                if (X < 0 || Y < 0 || X >= (int) w || Y >= (int) h)
                {
                    continue;
                }
                int idx = im.RasterBits[(size_t) y * (size_t) fw + (size_t) x];
                if (idx == transparent || idx >= map->ColorCount)
                {
                    continue;
                }
                uint8_t* o = &data[((size_t) Y * w + (size_t) X) * 4];
                o[0]       = map->Colors[idx].Red;
                o[1]       = map->Colors[idx].Green;
                o[2]       = map->Colors[idx].Blue;
                o[3]       = 255;
            }
        }
        DGifCloseFile (g, &e);
        return image_from_raw (w, h, ChannelLayout::Rgba, Sample::U8, data, out, err, err_len);
    }

    struct ColorBox
    {
        std::vector<uint32_t> colors;
    };

    int channel_of_rgb (uint32_t c, int k)
    {
        return (int) ((c >> (16 - 8 * k)) & 0xff);
    }

    /* The colour channel with the widest spread in the box. */
    int widest (const ColorBox& b, int* spread)
    {
        int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
        for (size_t i = 0; i < b.colors.size (); i++)
        {
            for (int k = 0; k < 3; k++)
            {
                int v = channel_of_rgb (b.colors[i], k);
                lo[k] = std::min (lo[k], v);
                hi[k] = std::max (hi[k], v);
            }
        }
        int best = 0;
        for (int k = 1; k < 3; k++)
        {
            if (hi[k] - lo[k] > hi[best] - lo[best])
            {
                best = k;
            }
        }
        *spread = hi[best] - lo[best];
        return best;
    }

    bool by_red (uint32_t a, uint32_t b)
    {
        return channel_of_rgb (a, 0) < channel_of_rgb (b, 0);
    }

    bool by_green (uint32_t a, uint32_t b)
    {
        return channel_of_rgb (a, 1) < channel_of_rgb (b, 1);
    }

    bool by_blue (uint32_t a, uint32_t b)
    {
        return channel_of_rgb (a, 2) < channel_of_rgb (b, 2);
    }

    /* Median cut over the opaque pixels' colours: the box with the widest channel split at its
       median until there are `count` boxes; each box's mean is a palette entry. */
    std::vector<uint32_t> median_cut (const std::vector<uint32_t>& pixels, size_t count)
    {
        std::vector<ColorBox> boxes (1);
        boxes[0].colors = pixels;
        while (boxes.size () < count)
        {
            int best = -1, best_spread = 0, best_k = 0;
            for (size_t i = 0; i < boxes.size (); i++)
            {
                if (boxes[i].colors.size () < 2)
                {
                    continue;
                }
                int spread, k = widest (boxes[i], &spread);
                if (spread > best_spread)
                {
                    best_spread = spread;
                    best        = (int) i;
                    best_k      = k;
                }
            }
            if (best < 0)
            {
                break;
            }
            std::vector<uint32_t>& c = boxes[(size_t) best].colors;
            std::sort (c.begin (), c.end (), best_k == 0 ? by_red : best_k == 1 ? by_green : by_blue);
            ColorBox upper;
            upper.colors.assign (c.begin () + (ptrdiff_t) (c.size () / 2), c.end ());
            c.resize (c.size () / 2);
            boxes.push_back (upper);
        }
        std::vector<uint32_t> pal;
        for (size_t i = 0; i < boxes.size (); i++)
        {
            if (boxes[i].colors.empty ())
            {
                continue;
            }
            uint64_t s[3] = {0, 0, 0};
            for (size_t j = 0; j < boxes[i].colors.size (); j++)
            {
                for (int k = 0; k < 3; k++)
                {
                    s[k] += (uint64_t) channel_of_rgb (boxes[i].colors[j], k);
                }
            }
            size_t n = boxes[i].colors.size ();
            pal.push_back ((uint32_t) ((s[0] / n) << 16 | (s[1] / n) << 8 | (s[2] / n)));
        }
        if (pal.empty ())
        {
            pal.push_back (0);
        }
        return pal;
    }

    int nearest_entry (const std::vector<uint32_t>& pal, int r, int g, int b)
    {
        int best = 0, bd = 1 << 30;
        for (size_t i = 0; i < pal.size (); i++)
        {
            int dr = channel_of_rgb (pal[i], 0) - r, dg = channel_of_rgb (pal[i], 1) - g,
                db = channel_of_rgb (pal[i], 2) - b;
            int d  = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd)
            {
                bd   = d;
                best = (int) i;
            }
        }
        return best;
    }

    /* `img` is 8-bit RGBA (encode_plan). */
    bool gif_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (img.width > 65535 || img.height > 65535)
        {
            set_error (err, err_len, "GIF images are at most 65535 px a side");
            return false;
        }
        size_t count     = (size_t) img.width * img.height;
        bool transparent = false;
        std::vector<uint32_t> sample;
        size_t step = std::max ((size_t) 1, count / 500000);
        for (size_t i = 0; i < count; i++)
        {
            const uint8_t* p = &img.data[i * 4];
            if (p[3] < 128)
            {
                transparent = true;
                continue;
            }
            if (i % step == 0)
            {
                sample.push_back ((uint32_t) p[0] << 16 | (uint32_t) p[1] << 8 | p[2]);
            }
        }
        std::vector<uint32_t> pal = median_cut (sample, transparent ? 255 : 256);
        /* Nearest entries cached on a 5-bit grid. */
        std::vector<int16_t> cache (32768, -1);
        std::vector<uint8_t> idx (count);
        for (size_t i = 0; i < count; i++)
        {
            const uint8_t* p = &img.data[i * 4];
            if (p[3] < 128)
            {
                idx[i] = 255;
                continue;
            }
            int key = (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3);
            if (cache[(size_t) key] < 0)
            {
                cache[(size_t) key] = (int16_t) nearest_entry (pal, p[0], p[1], p[2]);
            }
            idx[i] = (uint8_t) cache[(size_t) key];
        }
        ColorMapObject* map = GifMakeMapObject (256, NULL);
        for (int i = 0; i < 256; i++)
        {
            uint32_t c           = i < (int) pal.size () ? pal[(size_t) i] : 0;
            map->Colors[i].Red   = (GifByteType) (c >> 16);
            map->Colors[i].Green = (GifByteType) (c >> 8);
            map->Colors[i].Blue  = (GifByteType) c;
        }
        out->clear ();
        int e          = 0;
        GifFileType* g = EGifOpen (out, gif_write, &e);
        bool ok        = g != NULL;
        if (ok)
        {
            EGifSetGifVersion (g, true);
            ok = EGifPutScreenDesc (g, (int) img.width, (int) img.height, 8, 0, map) == GIF_OK;
            if (ok && transparent)
            {
                GraphicsControlBlock gcb;
                memset (&gcb, 0, sizeof gcb);
                gcb.DisposalMode     = DISPOSAL_UNSPECIFIED;
                gcb.TransparentColor = 255;
                GifByteType ext[4];
                EGifGCBToExtension (&gcb, ext);
                ok = EGifPutExtension (g, GRAPHICS_EXT_FUNC_CODE, 4, ext) == GIF_OK;
            }
            if (ok)
            {
                ok = EGifPutImageDesc (g, 0, 0, (int) img.width, (int) img.height, false, NULL) == GIF_OK;
            }
            for (uint32_t y = 0; y < img.height && ok; y++)
            {
                ok = EGifPutLine (g, &idx[(size_t) y * img.width], (int) img.width) == GIF_OK;
            }
            if (EGifCloseFile (g, &e) != GIF_OK)
            {
                ok = false;
            }
        }
        GifFreeMapObject (map);
        if (!ok)
        {
            set_error (err, err_len, "GIF: writing failed");
        }
        (void) opts;
        return ok;
    }

} // namespace codecs
