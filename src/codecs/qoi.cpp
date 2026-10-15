#include "codecs/codecs.h"
#include <string.h>

namespace codecs
{

    /* QOI ("Quite OK Image"), 8-bit RGB or RGBA, after the specification at qoiformat.org. */

    const uint8_t QOI_INDEX = 0x00;
    const uint8_t QOI_DIFF  = 0x40;
    const uint8_t QOI_LUMA  = 0x80;
    const uint8_t QOI_RUN   = 0xc0;
    const uint8_t QOI_RGB   = 0xfe;
    const uint8_t QOI_RGBA  = 0xff;

    uint32_t rd_be32 (const uint8_t* p)
    {
        return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
    }

    void wr_be32 (std::vector<uint8_t>* w, uint32_t v)
    {
        for (int i = 3; i >= 0; i--)
        {
            w->push_back ((uint8_t) (v >> (8 * i)));
        }
    }

    int qoi_hash (const uint8_t* c)
    {
        return (c[0] * 3 + c[1] * 5 + c[2] * 7 + c[3] * 11) % 64;
    }

    bool qoi_decode (const uint8_t* b, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (len < 22 || memcmp (b, "qoif", 4) != 0)
        {
            set_error (err, err_len, "QOI: not a QOI file");
            return false;
        }
        uint32_t w = rd_be32 (b + 4), h = rd_be32 (b + 8);
        uint8_t ch           = b[12];
        ChannelLayout layout = ch == 4 ? ChannelLayout::Rgba : ChannelLayout::Rgb;
        if (!limits.check (w, h, layout, Sample::U8, err, err_len))
        {
            return false;
        }
        size_t n = ch == 4 ? 4 : 3, count = (size_t) w * h;
        std::vector<uint8_t> data (count * n);
        uint8_t index[64][4];
        memset (index, 0, sizeof index);
        uint8_t px[4] = {0, 0, 0, 255};
        size_t at = 14, run = 0;
        for (size_t i = 0; i < count; i++)
        {
            if (run > 0)
            {
                run--;
            }
            else if (at < len)
            {
                uint8_t op = b[at++];
                if (op == QOI_RGB && at + 3 <= len)
                {
                    memcpy (px, b + at, 3);
                    at += 3;
                }
                else if (op == QOI_RGBA && at + 4 <= len)
                {
                    memcpy (px, b + at, 4);
                    at += 4;
                }
                else if ((op & 0xc0) == QOI_INDEX)
                {
                    memcpy (px, index[op & 63], 4);
                }
                else if ((op & 0xc0) == QOI_DIFF)
                {
                    px[0] = (uint8_t) (px[0] + ((op >> 4) & 3) - 2);
                    px[1] = (uint8_t) (px[1] + ((op >> 2) & 3) - 2);
                    px[2] = (uint8_t) (px[2] + (op & 3) - 2);
                }
                else if ((op & 0xc0) == QOI_LUMA && at < len)
                {
                    uint8_t b2 = b[at++];
                    int dg     = (op & 63) - 32;
                    px[0]      = (uint8_t) (px[0] + dg - 8 + ((b2 >> 4) & 15));
                    px[1]      = (uint8_t) (px[1] + dg);
                    px[2]      = (uint8_t) (px[2] + dg - 8 + (b2 & 15));
                }
                else if ((op & 0xc0) == QOI_RUN)
                {
                    run = op & 63;
                }
                memcpy (index[qoi_hash (px)], px, 4);
            }
            memcpy (&data[i * n], px, n);
        }
        return image_from_raw (w, h, layout, Sample::U8, data, out, err, err_len);
    }

    /* 8-bit RGB or RGBA (encode_plan). */
    bool qoi_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        size_t n = img.channels (), count = (size_t) img.width * img.height;
        out->clear ();
        out->insert (out->end (), {'q', 'o', 'i', 'f'});
        wr_be32 (out, img.width);
        wr_be32 (out, img.height);
        out->push_back ((uint8_t) n);
        out->push_back (0);
        uint8_t index[64][4];
        memset (index, 0, sizeof index);
        uint8_t prev[4] = {0, 0, 0, 255}, px[4] = {0, 0, 0, 255};
        int run = 0;
        for (size_t i = 0; i < count; i++)
        {
            memcpy (px, &img.data[i * n], n);
            if (memcmp (px, prev, 4) == 0)
            {
                run++;
                if (run == 62 || i + 1 == count)
                {
                    out->push_back ((uint8_t) (QOI_RUN | (run - 1)));
                    run = 0;
                }
                continue;
            }
            if (run > 0)
            {
                out->push_back ((uint8_t) (QOI_RUN | (run - 1)));
                run = 0;
            }
            int h = qoi_hash (px);
            if (memcmp (index[h], px, 4) == 0)
            {
                out->push_back ((uint8_t) (QOI_INDEX | h));
            }
            else
            {
                memcpy (index[h], px, 4);
                if (px[3] == prev[3])
                {
                    int dr = (int8_t) (px[0] - prev[0]), dg = (int8_t) (px[1] - prev[1]),
                        db    = (int8_t) (px[2] - prev[2]);
                    int dr_dg = dr - dg, db_dg = db - dg;
                    if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2)
                    {
                        out->push_back ((uint8_t) (QOI_DIFF | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2)));
                    }
                    else if (dr_dg > -9 && dr_dg < 8 && dg > -33 && dg < 32 && db_dg > -9 && db_dg < 8)
                    {
                        out->push_back ((uint8_t) (QOI_LUMA | (dg + 32)));
                        out->push_back ((uint8_t) ((dr_dg + 8) << 4 | (db_dg + 8)));
                    }
                    else
                    {
                        out->push_back (QOI_RGB);
                        out->insert (out->end (), px, px + 3);
                    }
                }
                else
                {
                    out->push_back (QOI_RGBA);
                    out->insert (out->end (), px, px + 4);
                }
            }
            memcpy (prev, px, 4);
        }
        out->insert (out->end (), 7, 0);
        out->push_back (1);
        (void) err;
        (void) err_len;
        return true;
    }

} // namespace codecs
