#include "codecs/codecs.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>

namespace codecs
{

    /* BMP and ICO. BMP: OS/2 and Windows headers (up to V5), 1/4/8-bit palettes, 16/24/32-bit with
       bit masks, RLE4 and RLE8, bottom-up or top-down; written as 24-bit, or 32-bit with a V4
       header when there is alpha. ICO: the largest image of the file, PNG or DIB (with its AND
       mask); written as one PNG entry (256 px a side at most). */

    uint16_t rd_le16 (const uint8_t* p)
    {
        return (uint16_t) (p[0] | (p[1] << 8));
    }

    uint32_t rd_le32 (const uint8_t* p)
    {
        return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
    }

    void wr_le16 (std::vector<uint8_t>* w, uint16_t v)
    {
        w->push_back ((uint8_t) v);
        w->push_back ((uint8_t) (v >> 8));
    }

    void wr_le32 (std::vector<uint8_t>* w, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            w->push_back ((uint8_t) (v >> (8 * i)));
        }
    }

    /* A masked field scaled to 0..255. */
    uint8_t mask_value (uint32_t px, uint32_t mask)
    {
        if (mask == 0)
        {
            return 255;
        }
        int shift = 0;
        while (((mask >> shift) & 1) == 0)
        {
            shift++;
        }
        uint32_t m = mask >> shift, v = (px & mask) >> shift;
        return (uint8_t) ((v * 255 + m / 2) / m);
    }

    /* RLE8 / RLE4 into one byte per pixel index (bottom-up rows as stored). */
    bool rle_expand (const uint8_t* src, size_t len, uint32_t w, uint32_t h, bool four, std::vector<uint8_t>* out)
    {
        out->assign ((size_t) w * h, 0);
        size_t i   = 0;
        uint32_t x = 0, y = 0;
        while (i + 1 < len && y < h)
        {
            uint8_t n = src[i], v = src[i + 1];
            i += 2;
            if (n > 0)
            {
                for (uint32_t k = 0; k < n && x < w; k++, x++)
                {
                    (*out)[(size_t) y * w + x] = four ? (uint8_t) (k & 1 ? v & 15 : v >> 4) : v;
                }
            }
            else if (v == 0)
            {
                x = 0;
                y++;
            }
            else if (v == 1)
            {
                break;
            }
            else if (v == 2)
            {
                if (i + 1 >= len)
                {
                    break;
                }
                x += src[i];
                y += src[i + 1];
                i += 2;
            }
            else
            {
                size_t bytes = four ? (v + 1) / 2 : v;
                for (uint32_t k = 0; k < v && x < w; k++, x++)
                {
                    size_t at = i + (four ? k / 2 : k);
                    if (at >= len)
                    {
                        return false;
                    }
                    (*out)[(size_t) y * w + x] = four ? (uint8_t) (k & 1 ? src[at] & 15 : src[at] >> 4) : src[at];
                }
                i += (bytes + 1) & ~(size_t) 1;
            }
        }
        return true;
    }

    /* A device-independent bitmap: header at p, pixels at `pixels` (NULL: right after the palette).
       Icons store twice the height (the AND mask follows the colour rows). */
    bool dib_decode (const uint8_t* p, size_t len, const uint8_t* pixels, size_t pixels_len, bool icon,
        const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (len < 12)
        {
            set_error (err, err_len, "BMP: the header is cut short");
            return false;
        }
        uint32_t hs = rd_le32 (p);
        int32_t w, h;
        uint16_t bpp;
        uint32_t comp = 0, colors_used = 0, masks[4] = {0, 0, 0, 0};
        size_t entry = 4;
        if (hs == 12)
        {
            w     = (int16_t) rd_le16 (p + 4);
            h     = (int16_t) rd_le16 (p + 6);
            bpp   = rd_le16 (p + 10);
            entry = 3;
        }
        else
        {
            if (hs < 40 || len < hs)
            {
                set_error (err, err_len, "BMP: unsupported header");
                return false;
            }
            w           = (int32_t) rd_le32 (p + 4);
            h           = (int32_t) rd_le32 (p + 8);
            bpp         = rd_le16 (p + 14);
            comp        = rd_le32 (p + 16);
            colors_used = rd_le32 (p + 32);
            if (hs >= 52)
            {
                for (int k = 0; k < 3; k++)
                {
                    masks[k] = rd_le32 (p + 40 + 4 * k);
                }
            }
            if (hs >= 56)
            {
                masks[3] = rd_le32 (p + 52);
            }
        }
        size_t at = hs;
        if (hs == 40 && (comp == 3 || comp == 6))
        {
            int n = comp == 6 ? 4 : 3;
            if (len < at + 4 * (size_t) n)
            {
                return false;
            }
            for (int k = 0; k < n; k++)
            {
                masks[k] = rd_le32 (p + at + 4 * k);
            }
            at += 4 * (size_t) n;
        }
        bool top_down = h < 0;
        uint32_t W = (uint32_t) std::abs (w), H = (uint32_t) std::abs (h);
        if (icon)
        {
            H /= 2;
        }
        if (W == 0 || H == 0 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        {
            set_error (err, err_len, "BMP: unsupported %u-bit image", bpp);
            return false;
        }
        std::vector<uint8_t> pal;
        if (bpp <= 8)
        {
            size_t n = colors_used ? colors_used : (size_t) 1 << bpp;
            if (n > 256)
            {
                n = 256;
            }
            if (len < at + n * entry)
            {
                set_error (err, err_len, "BMP: the palette is cut short");
                return false;
            }
            pal.assign (256 * 3, 0);
            for (size_t i = 0; i < n; i++)
            {
                pal[i * 3]     = p[at + i * entry + 2];
                pal[i * 3 + 1] = p[at + i * entry + 1];
                pal[i * 3 + 2] = p[at + i * entry];
            }
            at += n * entry;
        }
        if (pixels == NULL)
        {
            pixels     = p + at;
            pixels_len = len - at;
        }
        if (bpp == 16 && masks[0] == 0)
        {
            masks[0] = 0x7c00;
            masks[1] = 0x03e0;
            masks[2] = 0x001f;
        }
        bool use_masks = comp == 3 || comp == 6 || bpp == 16;
        if (!limits.check (W, H, ChannelLayout::Rgba, Sample::U8, err, err_len))
        {
            return false;
        }
        std::vector<uint8_t> data ((size_t) W * H * 4, 255);
        size_t stride = (((size_t) W * bpp + 31) / 32) * 4;
        std::vector<uint8_t> rle;
        if (comp == 1 || comp == 2)
        {
            if (!rle_expand (pixels, pixels_len, W, H, comp == 2, &rle))
            {
                set_error (err, err_len, "BMP: the compressed data is damaged");
                return false;
            }
        }
        else if (pixels_len < stride * H)
        {
            set_error (err, err_len, "BMP: the pixel data is cut short");
            return false;
        }
        bool any_alpha = false;
        for (uint32_t y = 0; y < H; y++)
        {
            uint32_t row       = top_down ? y : H - 1 - y;
            const uint8_t* src = pixels + (size_t) y * stride;
            for (uint32_t x = 0; x < W; x++)
            {
                uint8_t* o = &data[((size_t) row * W + x) * 4];
                if (!rle.empty ())
                {
                    uint8_t i = rle[(size_t) y * W + x];
                    memcpy (o, &pal[(size_t) i * 3], 3);
                }
                else if (bpp <= 8)
                {
                    size_t bit = (size_t) x * bpp;
                    uint8_t i  = (uint8_t) ((src[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1));
                    memcpy (o, &pal[(size_t) i * 3], 3);
                }
                else if (bpp == 24)
                {
                    o[0] = src[x * 3 + 2];
                    o[1] = src[x * 3 + 1];
                    o[2] = src[x * 3];
                }
                else
                {
                    uint32_t v = bpp == 16 ? rd_le16 (src + x * 2) : rd_le32 (src + x * 4);
                    if (use_masks)
                    {
                        for (int k = 0; k < 3; k++)
                        {
                            o[k] = mask_value (v, masks[k]);
                        }
                        if (masks[3] != 0)
                        {
                            o[3]      = mask_value (v, masks[3]);
                            any_alpha = true;
                        }
                    }
                    else
                    {
                        o[0] = (uint8_t) (v >> 16);
                        o[1] = (uint8_t) (v >> 8);
                        o[2] = (uint8_t) v;
                        /* BI_RGB 32-bit: the spare byte is alpha only if anything uses it. */
                        o[3] = (uint8_t) (v >> 24);
                        if (o[3] != 0)
                        {
                            any_alpha = true;
                        }
                    }
                }
            }
        }
        if (bpp == 32 && !use_masks && !any_alpha)
        {
            for (size_t i = 0; i < (size_t) W * H; i++)
            {
                data[i * 4 + 3] = 255;
            }
        }
        /* An icon's AND mask: 1 bit per pixel, set where transparent. */
        if (icon && bpp < 32)
        {
            size_t mstride = (((size_t) W + 31) / 32) * 4, start = stride * H;
            if (pixels_len >= start + mstride * H)
            {
                for (uint32_t y = 0; y < H; y++)
                {
                    uint32_t row     = top_down ? y : H - 1 - y;
                    const uint8_t* m = pixels + start + (size_t) y * mstride;
                    for (uint32_t x = 0; x < W; x++)
                    {
                        if ((m[x / 8] >> (7 - x % 8)) & 1)
                        {
                            data[((size_t) row * W + x) * 4 + 3] = 0;
                        }
                    }
                }
                any_alpha = true;
            }
        }
        bool alpha = any_alpha && (bpp == 32 || icon || masks[3] != 0);
        if (!alpha)
        {
            std::vector<uint8_t> rgb ((size_t) W * H * 3);
            for (size_t i = 0; i < (size_t) W * H; i++)
            {
                memcpy (&rgb[i * 3], &data[i * 4], 3);
            }
            return image_from_raw (W, H, ChannelLayout::Rgb, Sample::U8, rgb, out, err, err_len);
        }
        return image_from_raw (W, H, ChannelLayout::Rgba, Sample::U8, data, out, err, err_len);
    }

    bool bmp_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (len < 26 || bytes[0] != 'B' || bytes[1] != 'M')
        {
            set_error (err, err_len, "BMP: not a bitmap");
            return false;
        }
        uint32_t off = rd_le32 (bytes + 10);
        if (off >= len)
        {
            set_error (err, err_len, "BMP: the pixel data is missing");
            return false;
        }
        if (!dib_decode (bytes + 14, len - 14, bytes + off, len - off, false, limits, out, err, err_len))
        {
            return false;
        }
        uint32_t hs = rd_le32 (bytes + 14);
        if (hs >= 40)
        {
            uint32_t xp = rd_le32 (bytes + 14 + 24), yp = rd_le32 (bytes + 14 + 28);
            if (xp > 0 && yp > 0)
            {
                out->has_dpi = true;
                out->dpi_x   = (float) xp * 0.0254f;
                out->dpi_y   = (float) yp * 0.0254f;
            }
        }
        return true;
    }

    /* 8-bit RGB or RGBA (encode_plan). */
    bool bmp_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        bool alpha   = layout_has_alpha (img.layout);
        uint32_t bpp = alpha ? 32 : 24, hs = alpha ? 108 : 40;
        size_t stride = (((size_t) img.width * bpp + 31) / 32) * 4, size = 14 + hs + stride * img.height;
        if (size > 0xffffffffu)
        {
            set_error (err, err_len, "the image is too large for BMP");
            return false;
        }
        out->clear ();
        out->push_back ('B');
        out->push_back ('M');
        wr_le32 (out, (uint32_t) size);
        wr_le32 (out, 0);
        wr_le32 (out, 14 + hs);
        wr_le32 (out, hs);
        wr_le32 (out, img.width);
        wr_le32 (out, img.height);
        wr_le16 (out, 1);
        wr_le16 (out, (uint16_t) bpp);
        wr_le32 (out, alpha ? 3 : 0);
        wr_le32 (out, (uint32_t) (stride * img.height));
        uint32_t ppm = img.has_dpi ? (uint32_t) (img.dpi_x / 0.0254f + 0.5f) : 2835;
        wr_le32 (out, ppm);
        wr_le32 (out, img.has_dpi ? (uint32_t) (img.dpi_y / 0.0254f + 0.5f) : 2835);
        wr_le32 (out, 0);
        wr_le32 (out, 0);
        if (alpha)
        {
            wr_le32 (out, 0x00ff0000);
            wr_le32 (out, 0x0000ff00);
            wr_le32 (out, 0x000000ff);
            wr_le32 (out, 0xff000000);
            wr_le32 (out, 0x73524742);
            for (int i = 0; i < 12; i++)
            {
                wr_le32 (out, 0);
            }
        }
        size_t n = alpha ? 4 : 3;
        std::vector<uint8_t> row (stride, 0);
        for (uint32_t y = 0; y < img.height; y++)
        {
            const uint8_t* src = &img.data[(size_t) (img.height - 1 - y) * img.width * n];
            for (uint32_t x = 0; x < img.width; x++)
            {
                row[x * n]     = src[x * n + 2];
                row[x * n + 1] = src[x * n + 1];
                row[x * n + 2] = src[x * n];
                if (alpha)
                {
                    row[x * n + 3] = src[x * n + 3];
                }
            }
            out->insert (out->end (), row.begin (), row.end ());
        }
        return true;
    }

    bool ico_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        uint16_t count = len >= 6 ? rd_le16 (bytes + 4) : 0;
        if (count == 0 || len < 6 + (size_t) count * 16)
        {
            set_error (err, err_len, "ICO: the directory is damaged");
            return false;
        }
        /* The largest entry, the deepest among equals. */
        int best           = -1;
        uint32_t best_area = 0, best_bpp = 0;
        for (uint16_t i = 0; i < count; i++)
        {
            const uint8_t* e = bytes + 6 + (size_t) i * 16;
            uint32_t w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, bpp = rd_le16 (e + 6);
            uint32_t size = rd_le32 (e + 8), off = rd_le32 (e + 12);
            if ((size_t) off + size > len)
            {
                continue;
            }
            if (w * h > best_area || (w * h == best_area && bpp > best_bpp))
            {
                best      = i;
                best_area = w * h;
                best_bpp  = bpp;
            }
        }
        if (best < 0)
        {
            set_error (err, err_len, "ICO: no readable image");
            return false;
        }
        const uint8_t* e    = bytes + 6 + (size_t) best * 16;
        const uint8_t* data = bytes + rd_le32 (e + 12);
        size_t size         = rd_le32 (e + 8);
        if (size >= 8 && memcmp (data, "\x89PNG\r\n\x1a\n", 8) == 0)
        {
            return png_decode (data, size, limits, out, err, err_len);
        }
        return dib_decode (data, size, NULL, 0, true, limits, out, err, err_len);
    }

    /* 8-bit RGBA (encode_plan), as a PNG entry. */
    bool ico_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (img.width > 256 || img.height > 256)
        {
            set_error (err, err_len, "icons are at most 256 x 256 px (resize the image first)");
            return false;
        }
        std::vector<uint8_t> png;
        if (!png_encode (img, opts, &png, err, err_len))
        {
            return false;
        }
        out->clear ();
        wr_le16 (out, 0);
        wr_le16 (out, 1);
        wr_le16 (out, 1);
        out->push_back ((uint8_t) (img.width == 256 ? 0 : img.width));
        out->push_back ((uint8_t) (img.height == 256 ? 0 : img.height));
        out->push_back (0);
        out->push_back (0);
        wr_le16 (out, 1);
        wr_le16 (out, 32);
        wr_le32 (out, (uint32_t) png.size ());
        wr_le32 (out, 22);
        out->insert (out->end (), png.begin (), png.end ());
        return true;
    }

} // namespace codecs
