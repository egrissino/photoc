#include "codecs/codecs.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace codecs
{

    /* Netpbm: P1-P6 (PBM/PGM/PPM, ASCII and binary), P7 (PAM, including CMYK / CMYK_ALPHA) and PFM
       (PF colour / Pf gray float). Writing picks the subtype from the image: float gray/RGB -> PFM,
       gray -> P5, RGB -> P6, anything with alpha or CMYK -> P7. */

    struct PnmReader
    {
        const uint8_t* b;
        size_t len;
        size_t pos;
    };

    void pnm_skip_ws_and_comments (PnmReader* r)
    {
        while (r->pos < r->len)
        {
            uint8_t c = r->b[r->pos];
            if (c == '#')
            {
                while (r->pos < r->len && r->b[r->pos] != '\n' && r->b[r->pos] != '\r')
                {
                    r->pos++;
                }
            }
            else if (isspace (c))
            {
                r->pos++;
            }
            else
            {
                break;
            }
        }
    }

    bool pnm_token (PnmReader* r, std::string* out, char* err, size_t err_len)
    {
        pnm_skip_ws_and_comments (r);
        size_t start = r->pos;
        while (r->pos < r->len && !isspace (r->b[r->pos]) && r->b[r->pos] != '#')
        {
            r->pos++;
        }
        if (start == r->pos)
        {
            set_error (err, err_len, "Netpbm: unexpected end of header");
            return false;
        }
        out->assign ((const char*) r->b + start, r->pos - start);
        return true;
    }

    bool pnm_uint (PnmReader* r, uint32_t* out, char* err, size_t err_len)
    {
        std::string t;
        if (!pnm_token (r, &t, err, err_len))
        {
            return false;
        }
        if (t.empty () || t.size () > 10)
        {
            set_error (err, err_len, "Netpbm: invalid integer in header");
            return false;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < t.size (); i++)
        {
            if (!isdigit ((unsigned char) t[i]))
            {
                set_error (err, err_len, "Netpbm: invalid integer in header");
                return false;
            }
            v = v * 10 + (uint64_t) (t[i] - '0');
        }
        if (v > UINT32_MAX)
        {
            set_error (err, err_len, "Netpbm: invalid integer in header");
            return false;
        }
        *out = (uint32_t) v;
        return true;
    }

    /* The single whitespace byte between header and raster. */
    bool pnm_single_ws (PnmReader* r, char* err, size_t err_len)
    {
        if (r->pos < r->len && isspace (r->b[r->pos]))
        {
            r->pos++;
            return true;
        }
        set_error (err, err_len, "Netpbm: missing whitespace after header");
        return false;
    }

    bool pnm_sample_for_maxval (uint32_t maxval, Sample* out, char* err, size_t err_len)
    {
        if (maxval >= 1 && maxval <= 255)
        {
            *out = Sample::U8;
        }
        else if (maxval >= 256 && maxval <= 65535)
        {
            *out = Sample::U16;
        }
        else
        {
            set_error (err, err_len, "Netpbm: invalid maxval %u", maxval);
            return false;
        }
        return true;
    }

    bool pnm_sample_count (uint32_t w, uint32_t h, size_t channels, size_t* out, char* err, size_t err_len)
    {
        size_t px = (size_t) w * (size_t) h;
        if (channels != 0 && px > SIZE_MAX / 2 / channels)
        {
            set_error (err, err_len, "%ux%ux%zu samples do not fit in memory", w, h, channels);
            return false;
        }
        *out = px * channels;
        return true;
    }

    /* Raw value v in [0, maxval], rescaled to the full range of the output sample. */
    bool pnm_push (std::vector<uint8_t>* out, Sample sample, uint32_t maxval, uint32_t v, char* err, size_t err_len)
    {
        if (v > maxval)
        {
            set_error (err, err_len, "Netpbm: sample exceeds maxval");
            return false;
        }
        if (sample == Sample::U8)
        {
            out->push_back (maxval == 255 ? (uint8_t) v : (uint8_t) ((v * 255 + maxval / 2) / maxval));
        }
        else
        {
            uint16_t q = maxval == 65535 ? (uint16_t) v : (uint16_t) (((uint64_t) v * 65535 + maxval / 2) / maxval);
            uint8_t b[2];
            memcpy (b, &q, 2);
            out->push_back (b[0]);
            out->push_back (b[1]);
        }
        return true;
    }

    bool pnm_read_binary (
        PnmReader* r, size_t n, uint32_t maxval, Sample sample, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        const uint8_t* data = r->b + r->pos;
        size_t avail        = r->len - r->pos;
        size_t need         = maxval < 256 ? n : n * 2;
        if (avail < need)
        {
            set_error (err, err_len, "Netpbm: truncated raster");
            return false;
        }
        out->reserve (n * (sample == Sample::U8 ? 1 : 2));
        for (size_t i = 0; i < n; i++)
        {
            uint32_t v = maxval < 256 ? data[i] : ((uint32_t) data[i * 2] << 8) | data[i * 2 + 1];
            if (!pnm_push (out, sample, maxval, v, err, err_len))
            {
                return false;
            }
        }
        return true;
    }

    bool pnm_decode_pam (PnmReader* r, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        bool has_w = false, has_h = false, has_d = false, has_m = false;
        uint32_t w = 0, h = 0, depth = 0, maxval = 0;
        std::string tupltype;
        for (;;)
        {
            std::string tok;
            if (!pnm_token (r, &tok, err, err_len))
            {
                return false;
            }
            if (tok == "WIDTH")
            {
                if (!pnm_uint (r, &w, err, err_len))
                {
                    return false;
                }
                has_w = true;
            }
            else if (tok == "HEIGHT")
            {
                if (!pnm_uint (r, &h, err, err_len))
                {
                    return false;
                }
                has_h = true;
            }
            else if (tok == "DEPTH")
            {
                if (!pnm_uint (r, &depth, err, err_len))
                {
                    return false;
                }
                has_d = true;
            }
            else if (tok == "MAXVAL")
            {
                if (!pnm_uint (r, &maxval, err, err_len))
                {
                    return false;
                }
                has_m = true;
            }
            else if (tok == "TUPLTYPE")
            {
                std::string t;
                if (!pnm_token (r, &t, err, err_len))
                {
                    return false;
                }
                if (!tupltype.empty ())
                {
                    tupltype += ' ';
                }
                tupltype += t;
            }
            else if (tok == "ENDHDR")
            {
                break;
            }
            else
            {
                set_error (err, err_len, "Netpbm: unknown PAM header field");
                return false;
            }
        }
        if (!pnm_single_ws (r, err, err_len))
        {
            return false;
        }
        if (!has_w || !has_h || !has_d || !has_m)
        {
            set_error (err, err_len, "Netpbm: incomplete PAM header");
            return false;
        }
        ChannelLayout layout;
        const std::string& t = tupltype;
        if (depth == 1 && (t == "BLACKANDWHITE" || t == "GRAYSCALE" || t.empty ()))
        {
            layout = ChannelLayout::Gray;
        }
        else if (depth == 2 && (t == "BLACKANDWHITE_ALPHA" || t == "GRAYSCALE_ALPHA" || t.empty ()))
        {
            layout = ChannelLayout::GrayA;
        }
        else if (depth == 3 && (t == "RGB" || t.empty ()))
        {
            layout = ChannelLayout::Rgb;
        }
        else if (depth == 4 && (t == "RGB_ALPHA" || t.empty ()))
        {
            layout = ChannelLayout::Rgba;
        }
        else if (depth == 4 && t == "CMYK")
        {
            layout = ChannelLayout::Cmyk;
        }
        else if (depth == 5 && t == "CMYK_ALPHA")
        {
            layout = ChannelLayout::CmykA;
        }
        else
        {
            set_error (err, err_len, "Netpbm: PAM tuple type \"%s\" with depth %u is not supported", t.c_str (), depth);
            return false;
        }
        Sample sample;
        if (!pnm_sample_for_maxval (maxval, &sample, err, err_len))
        {
            return false;
        }
        if (!limits.check (w, h, layout, sample, err, err_len))
        {
            return false;
        }
        size_t n;
        if (!pnm_sample_count (w, h, layout_channels (layout), &n, err, err_len))
        {
            return false;
        }
        std::vector<uint8_t> data;
        if (!pnm_read_binary (r, n, maxval, sample, &data, err, err_len))
        {
            return false;
        }
        return image_from_raw (w, h, layout, sample, data, out, err, err_len);
    }

    bool pnm_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        if (len < 2 || bytes[0] != 'P')
        {
            set_error (err, err_len, "Netpbm: not a Netpbm file");
            return false;
        }
        uint8_t kind = bytes[1];
        PnmReader r  = {bytes, len, 2};

        if (kind == '1' || kind == '4')
        {
            uint32_t w, h;
            if (!pnm_uint (&r, &w, err, err_len) || !pnm_uint (&r, &h, err, err_len))
            {
                return false;
            }
            if (!limits.check (w, h, ChannelLayout::Gray, Sample::U8, err, err_len))
            {
                return false;
            }
            size_t n;
            if (!pnm_sample_count (w, h, 1, &n, err, err_len))
            {
                return false;
            }
            std::vector<uint8_t> px;
            if (kind == '4')
            {
                if (!pnm_single_ws (&r, err, err_len))
                {
                    return false;
                }
                size_t row = ((size_t) w + 7) / 8;
                if (r.len - r.pos < row * h)
                {
                    set_error (err, err_len, "Netpbm: truncated raster");
                    return false;
                }
                const uint8_t* data = r.b + r.pos;
                px.reserve (n);
                for (size_t y = 0; y < h; y++)
                {
                    for (size_t x = 0; x < w; x++)
                    {
                        int bit = (data[y * row + x / 8] >> (7 - x % 8)) & 1;
                        px.push_back (bit == 1 ? 0 : 255);
                    }
                }
            }
            else
            {
                px.reserve (n < r.len - r.pos ? n : r.len - r.pos);
                while (px.size () < n)
                {
                    pnm_skip_ws_and_comments (&r);
                    if (r.pos >= r.len)
                    {
                        set_error (err, err_len, "Netpbm: truncated raster");
                        return false;
                    }
                    uint8_t c = r.b[r.pos];
                    if (c == '0')
                    {
                        px.push_back (255);
                    }
                    else if (c == '1')
                    {
                        px.push_back (0);
                    }
                    else
                    {
                        set_error (err, err_len, "Netpbm: invalid PBM digit");
                        return false;
                    }
                    r.pos++;
                }
            }
            return image_from_raw (w, h, ChannelLayout::Gray, Sample::U8, px, out, err, err_len);
        }

        if (kind == '2' || kind == '3' || kind == '5' || kind == '6')
        {
            uint32_t w, h, maxval;
            if (!pnm_uint (&r, &w, err, err_len) || !pnm_uint (&r, &h, err, err_len)
                || !pnm_uint (&r, &maxval, err, err_len))
            {
                return false;
            }
            ChannelLayout layout = (kind == '2' || kind == '5') ? ChannelLayout::Gray : ChannelLayout::Rgb;
            Sample sample;
            if (!pnm_sample_for_maxval (maxval, &sample, err, err_len))
            {
                return false;
            }
            if (!limits.check (w, h, layout, sample, err, err_len))
            {
                return false;
            }
            size_t n;
            if (!pnm_sample_count (w, h, layout_channels (layout), &n, err, err_len))
            {
                return false;
            }
            std::vector<uint8_t> data;
            if (kind == '5' || kind == '6')
            {
                if (!pnm_single_ws (&r, err, err_len))
                {
                    return false;
                }
                if (!pnm_read_binary (&r, n, maxval, sample, &data, err, err_len))
                {
                    return false;
                }
            }
            else
            {
                size_t cap = (r.len - r.pos) / 2 + 1;
                data.reserve ((n < cap ? n : cap) * (sample == Sample::U8 ? 1 : 2));
                for (size_t i = 0; i < n; i++)
                {
                    uint32_t v;
                    if (!pnm_uint (&r, &v, err, err_len))
                    {
                        return false;
                    }
                    if (!pnm_push (&data, sample, maxval, v, err, err_len))
                    {
                        return false;
                    }
                }
            }
            return image_from_raw (w, h, layout, sample, data, out, err, err_len);
        }

        if (kind == '7')
        {
            return pnm_decode_pam (&r, limits, out, err, err_len);
        }

        if (kind == 'F' || kind == 'f')
        {
            ChannelLayout layout = kind == 'F' ? ChannelLayout::Rgb : ChannelLayout::Gray;
            uint32_t w, h;
            if (!pnm_uint (&r, &w, err, err_len) || !pnm_uint (&r, &h, err, err_len))
            {
                return false;
            }
            std::string scale_tok;
            if (!pnm_token (&r, &scale_tok, err, err_len))
            {
                return false;
            }
            char* end   = NULL;
            float scale = strtof (scale_tok.c_str (), &end);
            if (end == scale_tok.c_str () || *end != 0 || !(scale == scale) || scale == 0.0f || scale > 3.0e38f
                || scale < -3.0e38f)
            {
                set_error (err, err_len, "Netpbm: invalid PFM scale");
                return false;
            }
            if (!pnm_single_ws (&r, err, err_len))
            {
                return false;
            }
            if (!limits.check (w, h, layout, Sample::F32, err, err_len))
            {
                return false;
            }
            size_t nc = layout_channels (layout);
            size_t n;
            if (!pnm_sample_count (w, h, nc, &n, err, err_len))
            {
                return false;
            }
            if (r.len - r.pos < n * 4)
            {
                set_error (err, err_len, "Netpbm: truncated raster");
                return false;
            }
            bool little      = scale < 0.0f;
            size_t row_bytes = (size_t) w * nc * 4;
            std::vector<uint8_t> data (row_bytes * h);
            const uint8_t* src = r.b + r.pos;
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* dst = &data[(h - 1 - y) * row_bytes];
                for (size_t i = 0; i < (size_t) w * nc; i++)
                {
                    const uint8_t* s = src + y * row_bytes + i * 4;
                    uint32_t bits    = little ? ((uint32_t) s[0] | ((uint32_t) s[1] << 8) | ((uint32_t) s[2] << 16)
                                                 | ((uint32_t) s[3] << 24))
                                              : (((uint32_t) s[0] << 24) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 8)
                                                 | (uint32_t) s[3]);
                    memcpy (dst + i * 4, &bits, 4);
                }
            }
            return image_from_raw (w, h, layout, Sample::F32, data, out, err, err_len);
        }

        set_error (err, err_len, "Netpbm: unknown subtype P%c", kind);
        return false;
    }

    void pnm_append (std::vector<uint8_t>* out, const char* s)
    {
        out->insert (out->end (), s, s + strlen (s));
    }

    bool pnm_encode (const Image& src, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        bool float_sample = src.sample == Sample::F32 || src.sample == Sample::F16;
        bool pfm          = float_sample && (src.layout == ChannelLayout::Gray || src.layout == ChannelLayout::Rgb);
        Sample want       = pfm ? Sample::F32 : (src.sample == Sample::U8 ? Sample::U8 : Sample::U16);
        Image converted;
        const Image* img = &src;
        if (src.sample != want)
        {
            if (!image_convert (src, src.layout, want, &converted, err, err_len))
            {
                return false;
            }
            img = &converted;
        }
        uint32_t w = img->width, h = img->height;
        size_t nc = img->channels ();
        char header[256];
        out->clear ();

        if (pfm)
        {
            snprintf (
                header, sizeof header, "%s\n%u %u\n-1.0\n", img->layout == ChannelLayout::Gray ? "Pf" : "PF", w, h);
            pnm_append (out, header);
            size_t row = (size_t) w * nc;
            for (size_t y = h; y-- > 0;)
            {
                for (size_t i = 0; i < row; i++)
                {
                    uint32_t bits;
                    memcpy (&bits, &img->data[(y * row + i) * 4], 4);
                    uint8_t le[4] = {
                        (uint8_t) bits, (uint8_t) (bits >> 8), (uint8_t) (bits >> 16), (uint8_t) (bits >> 24)};
                    out->insert (out->end (), le, le + 4);
                }
            }
            return true;
        }

        unsigned maxval = img->sample == Sample::U8 ? 255 : 65535;
        if (img->layout == ChannelLayout::Gray)
        {
            snprintf (header, sizeof header, "P5\n%u %u\n%u\n", w, h, maxval);
        }
        else if (img->layout == ChannelLayout::Rgb)
        {
            snprintf (header, sizeof header, "P6\n%u %u\n%u\n", w, h, maxval);
        }
        else
        {
            const char* t = img->layout == ChannelLayout::GrayA ? "GRAYSCALE_ALPHA"
                          : img->layout == ChannelLayout::Rgba  ? "RGB_ALPHA"
                          : img->layout == ChannelLayout::Cmyk  ? "CMYK"
                                                                : "CMYK_ALPHA";
            snprintf (header, sizeof header, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH %zu\nMAXVAL %u\nTUPLTYPE %s\nENDHDR\n", w,
                h, nc, maxval, t);
        }
        pnm_append (out, header);
        if (img->sample == Sample::U8)
        {
            out->insert (out->end (), img->data.begin (), img->data.end ());
        }
        else
        {
            size_t n = img->data.size () / 2;
            out->reserve (out->size () + n * 2);
            for (size_t i = 0; i < n; i++)
            {
                uint16_t v;
                memcpy (&v, &img->data[i * 2], 2);
                out->push_back ((uint8_t) (v >> 8));
                out->push_back ((uint8_t) v);
            }
        }
        return true;
    }

} // namespace codecs
