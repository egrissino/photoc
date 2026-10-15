#include "codecs/codecs.h"
#include <string.h>

namespace codecs
{

    /* ZSoft PCX: 1-bit mono, 4-bit and 1-bit x 4-plane EGA palettes, 8-bit with the 256-colour
       palette at the end, 24-bit (three planes) and 32-bit (four). Written as 24-bit, run-length
       encoded. */

    bool pcx_decode (const uint8_t* b, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (len < 128 || b[0] != 0x0A || b[2] != 1)
        {
            set_error (err, err_len, "PCX: not a PCX file");
            return false;
        }
        int bpp = b[3], planes = b[65];
        uint32_t w   = (uint32_t) (rd_le16 (b + 8) - rd_le16 (b + 4) + 1),
                 h   = (uint32_t) (rd_le16 (b + 10) - rd_le16 (b + 6) + 1);
        size_t bpl   = rd_le16 (b + 66);
        bool ok_kind = (bpp == 8 && (planes == 1 || planes == 3 || planes == 4))
                    || (bpp == 1 && (planes == 1 || planes == 4)) || (bpp == 4 && planes == 1);
        if (!ok_kind || w == 0 || h == 0 || bpl == 0)
        {
            set_error (err, err_len, "PCX: unsupported %d-bit %d-plane image", bpp, planes);
            return false;
        }
        ChannelLayout layout = planes == 4 && bpp == 8 ? ChannelLayout::Rgba : ChannelLayout::Rgb;
        if (!limits.check (w, h, layout, Sample::U8, err, err_len))
        {
            return false;
        }
        /* The 256-colour palette sits after a 0x0C marker at the end; 16 colours in the header. */
        uint8_t pal[256 * 3];
        memcpy (pal, b + 16, 48);
        if (bpp == 8 && planes == 1 && len >= 769 + 128 && b[len - 769] == 0x0C)
        {
            memcpy (pal, b + len - 768, 768);
        }
        if (bpp == 1 && planes == 1)
        {
            memset (pal, 0, 3);
            memset (pal + 3, 255, 3);
        }
        size_t line = bpl * (size_t) planes, at = 128, n = layout_channels (layout);
        std::vector<uint8_t> scan (line), data ((size_t) w * h * n);
        for (uint32_t y = 0; y < h; y++)
        {
            size_t i = 0;
            while (i < line && at < len)
            {
                uint8_t c = b[at++];
                if ((c & 0xC0) == 0xC0)
                {
                    size_t k = c & 0x3F;
                    if (at >= len)
                    {
                        break;
                    }
                    uint8_t v = b[at++];
                    for (size_t j = 0; j < k && i < line; j++)
                    {
                        scan[i++] = v;
                    }
                }
                else
                {
                    scan[i++] = c;
                }
            }
            for (uint32_t x = 0; x < w; x++)
            {
                uint8_t* o = &data[((size_t) y * w + x) * n];
                if (bpp == 8 && planes >= 3)
                {
                    for (int p = 0; p < planes; p++)
                    {
                        o[p] = scan[(size_t) p * bpl + x];
                    }
                    continue;
                }
                int idx;
                if (bpp == 8)
                {
                    idx = scan[x];
                }
                else if (bpp == 4)
                {
                    idx = (scan[x / 2] >> (x & 1 ? 0 : 4)) & 15;
                }
                else
                {
                    idx = 0;
                    for (int p = 0; p < planes; p++)
                    {
                        idx |= ((scan[(size_t) p * bpl + x / 8] >> (7 - x % 8)) & 1) << p;
                    }
                }
                memcpy (o, &pal[idx * 3], 3);
            }
        }
        return image_from_raw (w, h, layout, Sample::U8, data, out, err, err_len);
    }

    void pcx_rle (const uint8_t* src, size_t n, std::vector<uint8_t>* out)
    {
        size_t i = 0;
        while (i < n)
        {
            size_t run = 1;
            while (i + run < n && run < 63 && src[i + run] == src[i])
            {
                run++;
            }
            if (run > 1 || (src[i] & 0xC0) == 0xC0)
            {
                out->push_back ((uint8_t) (0xC0 | run));
            }
            out->push_back (src[i]);
            i += run;
        }
    }

    /* 8-bit RGB (encode_plan). */
    bool pcx_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (img.width > 65535 || img.height > 65535)
        {
            set_error (err, err_len, "PCX images are at most 65535 px a side");
            return false;
        }
        size_t bpl = (img.width + 1) & ~(size_t) 1;
        out->assign (128, 0);
        (*out)[0]     = 0x0A;
        (*out)[1]     = 5;
        (*out)[2]     = 1;
        (*out)[3]     = 8;
        uint16_t xmax = (uint16_t) (img.width - 1), ymax = (uint16_t) (img.height - 1);
        (*out)[8]    = (uint8_t) xmax;
        (*out)[9]    = (uint8_t) (xmax >> 8);
        (*out)[10]   = (uint8_t) ymax;
        (*out)[11]   = (uint8_t) (ymax >> 8);
        uint16_t dpi = img.has_dpi ? (uint16_t) (img.dpi_x + 0.5f) : 72;
        (*out)[12] = (*out)[14] = (uint8_t) dpi;
        (*out)[13] = (*out)[15] = (uint8_t) (dpi >> 8);
        (*out)[65]              = 3;
        (*out)[66]              = (uint8_t) bpl;
        (*out)[67]              = (uint8_t) (bpl >> 8);
        (*out)[68]              = 1;
        std::vector<uint8_t> plane (bpl, 0);
        for (uint32_t y = 0; y < img.height; y++)
        {
            for (int p = 0; p < 3; p++)
            {
                for (uint32_t x = 0; x < img.width; x++)
                {
                    plane[x] = img.data[((size_t) y * img.width + x) * 3 + (size_t) p];
                }
                pcx_rle (plane.data (), bpl, out);
            }
        }
        (void) err;
        (void) err_len;
        return true;
    }

} // namespace codecs
