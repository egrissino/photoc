#include "codecs/codecs.h"
#include <string.h>

namespace codecs
{

    /* Truevision TGA: colour-mapped, true-colour (15/16/24/32-bit) and gray (8-bit, 16-bit gray +
       alpha), raw or RLE, any origin. Written RLE-compressed as 24/32-bit colour or 8/16-bit gray,
       top-left origin. */

    /* One pixel of `depth` bits as RGBA. */
    void tga_pixel (const uint8_t* p, int depth, bool gray, bool alpha_bits, uint8_t* o)
    {
        if (gray)
        {
            o[0] = o[1] = o[2] = p[0];
            o[3]               = depth == 16 ? p[1] : 255;
            return;
        }
        if (depth == 15 || depth == 16)
        {
            uint16_t v = rd_le16 (p);
            o[0]       = (uint8_t) (((v >> 10) & 31) * 255 / 31);
            o[1]       = (uint8_t) (((v >> 5) & 31) * 255 / 31);
            o[2]       = (uint8_t) ((v & 31) * 255 / 31);
            o[3]       = depth == 16 && alpha_bits ? (v & 0x8000 ? 255 : 0) : 255;
            return;
        }
        o[0] = p[2];
        o[1] = p[1];
        o[2] = p[0];
        o[3] = depth == 32 && alpha_bits ? p[3] : 255;
    }

    bool tga_decode (const uint8_t* b, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (len < 18)
        {
            set_error (err, err_len, "TGA: the header is cut short");
            return false;
        }
        uint8_t id_len = b[0], cmap_type = b[1], type = b[2];
        uint16_t cmap_first = rd_le16 (b + 3), cmap_len = rd_le16 (b + 5);
        uint8_t cmap_depth = b[7];
        uint32_t w = rd_le16 (b + 12), h = rd_le16 (b + 14);
        int depth    = b[16];
        uint8_t desc = b[17];
        bool rle = type >= 9, gray = type == 3 || type == 11, mapped = type == 1 || type == 9;
        bool alpha_bits = (desc & 15) != 0 || (depth == 32 && !mapped);
        size_t at       = 18 + id_len;
        std::vector<uint8_t> cmap;
        if (cmap_type == 1)
        {
            size_t eb = (cmap_depth + 7) / 8;
            if (len < at + eb * cmap_len)
            {
                set_error (err, err_len, "TGA: the colour map is cut short");
                return false;
            }
            cmap.assign ((size_t) (cmap_first + cmap_len) * 4, 255);
            for (size_t i = 0; i < cmap_len; i++)
            {
                tga_pixel (b + at + i * eb, cmap_depth, false, cmap_depth == 32 || cmap_depth == 16,
                    &cmap[(cmap_first + i) * 4]);
            }
            at += eb * cmap_len;
        }
        if (!limits.check (w, h, ChannelLayout::Rgba, Sample::U8, err, err_len))
        {
            return false;
        }
        size_t pb = (size_t) (depth + 7) / 8, count = (size_t) w * h;
        std::vector<uint8_t> raw (count * pb);
        if (rle)
        {
            size_t i = 0;
            while (i < count)
            {
                if (at >= len)
                {
                    break;
                }
                uint8_t hdr = b[at++];
                size_t n    = (size_t) (hdr & 127) + 1;
                if (hdr & 128)
                {
                    if (at + pb > len)
                    {
                        break;
                    }
                    for (size_t k = 0; k < n && i < count; k++, i++)
                    {
                        memcpy (&raw[i * pb], b + at, pb);
                    }
                    at += pb;
                }
                else
                {
                    if (at + n * pb > len)
                    {
                        break;
                    }
                    for (size_t k = 0; k < n && i < count; k++, i++)
                    {
                        memcpy (&raw[i * pb], b + at + k * pb, pb);
                    }
                    at += n * pb;
                }
            }
            if (i < count)
            {
                set_error (err, err_len, "TGA: the compressed data is cut short");
                return false;
            }
        }
        else
        {
            if (len < at + raw.size ())
            {
                set_error (err, err_len, "TGA: the pixel data is cut short");
                return false;
            }
            memcpy (raw.data (), b + at, raw.size ());
        }
        bool top = (desc & 0x20) != 0, right = (desc & 0x10) != 0, any_alpha = false;
        std::vector<uint8_t> data (count * 4);
        for (uint32_t y = 0; y < h; y++)
        {
            for (uint32_t x = 0; x < w; x++)
            {
                const uint8_t* p = &raw[((size_t) y * w + x) * pb];
                uint32_t oy = top ? y : h - 1 - y, ox = right ? w - 1 - x : x;
                uint8_t* o = &data[((size_t) oy * w + ox) * 4];
                if (mapped)
                {
                    size_t idx = pb == 2 ? rd_le16 (p) : p[0];
                    if (idx * 4 + 3 < cmap.size ())
                    {
                        memcpy (o, &cmap[idx * 4], 4);
                    }
                    else
                    {
                        o[0] = o[1] = o[2] = 0, o[3] = 255;
                    }
                }
                else
                {
                    tga_pixel (p, depth, gray, alpha_bits, o);
                }
                if (o[3] != 255)
                {
                    any_alpha = true;
                }
            }
        }
        ChannelLayout layout = gray      ? (any_alpha ? ChannelLayout::GrayA : ChannelLayout::Gray)
                             : any_alpha ? ChannelLayout::Rgba
                                         : ChannelLayout::Rgb;
        size_t n             = layout_channels (layout);
        std::vector<uint8_t> packed (count * n);
        for (size_t i = 0; i < count; i++)
        {
            const uint8_t* s = &data[i * 4];
            if (gray)
            {
                packed[i * n] = s[0];
                if (any_alpha)
                {
                    packed[i * n + 1] = s[3];
                }
            }
            else
            {
                memcpy (&packed[i * n], s, n);
            }
        }
        return image_from_raw (w, h, layout, Sample::U8, packed, out, err, err_len);
    }

    /* 8-bit gray, gray + alpha, RGB or RGBA (encode_plan). */
    bool tga_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (img.width > 65535 || img.height > 65535)
        {
            set_error (err, err_len, "TGA images are at most 65535 px a side");
            return false;
        }
        bool gray = layout_is_gray (img.layout), alpha = layout_has_alpha (img.layout);
        size_t n = img.channels ();
        out->assign (18, 0);
        (*out)[2]  = gray ? 11 : 10;
        (*out)[12] = (uint8_t) img.width;
        (*out)[13] = (uint8_t) (img.width >> 8);
        (*out)[14] = (uint8_t) img.height;
        (*out)[15] = (uint8_t) (img.height >> 8);
        (*out)[16] = (uint8_t) (n * 8);
        (*out)[17] = (uint8_t) (0x20 | (alpha ? 8 : 0));
        std::vector<uint8_t> px (n);
        /* Packets never cross rows. */
        for (uint32_t y = 0; y < img.height; y++)
        {
            const uint8_t* row = &img.data[(size_t) y * img.width * n];
            uint32_t x         = 0;
            while (x < img.width)
            {
                uint32_t run = 1;
                while (x + run < img.width && run < 128
                       && memcmp (row + (size_t) (x + run) * n, row + (size_t) x * n, n) == 0)
                {
                    run++;
                }
                uint32_t count = run;
                bool repeat    = run > 1;
                if (!repeat)
                {
                    while (x + count < img.width && count < 128
                           && (x + count + 1 >= img.width
                               || memcmp (row + (size_t) (x + count) * n, row + (size_t) (x + count + 1) * n, n) != 0))
                    {
                        count++;
                    }
                }
                out->push_back ((uint8_t) ((repeat ? 128 : 0) | (count - 1)));
                for (uint32_t k = 0; k < (repeat ? 1 : count); k++)
                {
                    const uint8_t* s = row + (size_t) (x + k) * n;
                    if (gray)
                    {
                        out->insert (out->end (), s, s + n);
                    }
                    else
                    {
                        out->push_back (s[2]);
                        out->push_back (s[1]);
                        out->push_back (s[0]);
                        if (alpha)
                        {
                            out->push_back (s[3]);
                        }
                    }
                }
                x += count;
            }
        }
        out->insert (out->end (), 8, 0);
        const char sig[18] = "TRUEVISION-XFILE.";
        out->insert (out->end (), sig, sig + 18);
        return true;
    }

} // namespace codecs
