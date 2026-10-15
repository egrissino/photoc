#include "codecs/codecs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace codecs
{

    /* Radiance HDR (RGBE): read flat or run-length scanlines into 32-bit float RGB (the -Y +X
       orientation, and +Y flipped); written with run-length scanlines. */

    void rgbe_to_float (const uint8_t* e, float* out)
    {
        if (e[3] == 0)
        {
            out[0] = out[1] = out[2] = 0.0f;
            return;
        }
        float f = ldexpf (1.0f, (int) e[3] - (128 + 8));
        for (int k = 0; k < 3; k++)
        {
            out[k] = ((float) e[k] + 0.5f) * f;
        }
    }

    void float_to_rgbe (const float* c, uint8_t* e)
    {
        float v = fmaxf (c[0], fmaxf (c[1], c[2]));
        if (v < 1e-32f)
        {
            e[0] = e[1] = e[2] = e[3] = 0;
            return;
        }
        int ex;
        float m = frexpf (v, &ex) * 256.0f / v;
        for (int k = 0; k < 3; k++)
        {
            e[k] = (uint8_t) fminf (fmaxf (c[k], 0.0f) * m, 255.0f);
        }
        e[3] = (uint8_t) (ex + 128);
    }

    bool hdr_decode (const uint8_t* b, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        size_t at = 0;
        std::string line;
        bool flip  = false;
        uint32_t w = 0, h = 0;
        /* Header lines until a blank one, then the resolution line. */
        bool blank = false;
        while (at < len)
        {
            size_t e = at;
            while (e < len && b[e] != '\n')
            {
                e++;
            }
            line.assign ((const char*) b + at, e - at);
            at = e + 1;
            if (blank)
            {
                char ys, xs;
                unsigned hh, ww;
                if (sscanf (line.c_str (), "%cY %u %cX %u", &ys, &hh, &xs, &ww) != 4)
                {
                    set_error (err, err_len, "HDR: unsupported orientation \"%s\"", line.c_str ());
                    return false;
                }
                flip = ys == '+';
                w    = ww;
                h    = hh;
                break;
            }
            if (line.empty ())
            {
                blank = true;
            }
            else if (line.compare (0, 7, "FORMAT=") == 0 && line != "FORMAT=32-bit_rle_rgbe")
            {
                set_error (err, err_len, "HDR: only RGBE files are supported");
                return false;
            }
        }
        if (w == 0 || h == 0 || !limits.check (w, h, ChannelLayout::Rgb, Sample::F32, err, err_len))
        {
            if (w == 0 || h == 0)
            {
                set_error (err, err_len, "HDR: the header is damaged");
            }
            return false;
        }
        std::vector<uint8_t> scan ((size_t) w * 4);
        std::vector<float> data ((size_t) w * h * 3);
        for (uint32_t y = 0; y < h; y++)
        {
            if (at + 4 <= len && b[at] == 2 && b[at + 1] == 2 && ((b[at + 2] << 8) | b[at + 3]) == (int) w && w >= 8
                && w < 32768)
            {
                at += 4;
                for (int c = 0; c < 4; c++)
                {
                    uint32_t x = 0;
                    while (x < w && at < len)
                    {
                        uint8_t n = b[at++];
                        if (n > 128)
                        {
                            n = (uint8_t) (n - 128);
                            if (at >= len)
                            {
                                break;
                            }
                            for (uint32_t k = 0; k < n && x < w; k++)
                            {
                                scan[(size_t) (x++) * 4 + (size_t) c] = b[at];
                            }
                            at++;
                        }
                        else
                        {
                            for (uint32_t k = 0; k < n && x < w && at < len; k++)
                            {
                                scan[(size_t) (x++) * 4 + (size_t) c] = b[at++];
                            }
                        }
                    }
                }
            }
            else
            {
                if (at + (size_t) w * 4 > len)
                {
                    set_error (err, err_len, "HDR: the pixel data is cut short");
                    return false;
                }
                memcpy (scan.data (), b + at, (size_t) w * 4);
                at += (size_t) w * 4;
            }
            uint32_t row = flip ? h - 1 - y : y;
            for (uint32_t x = 0; x < w; x++)
            {
                rgbe_to_float (&scan[(size_t) x * 4], &data[((size_t) row * w + x) * 3]);
            }
        }
        std::vector<uint8_t> bytes (data.size () * 4);
        memcpy (bytes.data (), data.data (), bytes.size ());
        return image_from_raw (w, h, ChannelLayout::Rgb, Sample::F32, bytes, out, err, err_len);
    }

    /* 32-bit float RGB (encode_plan). */
    bool hdr_encode (const Image& img, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        char head[128];
        snprintf (head, sizeof head, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %u +X %u\n", img.height, img.width);
        out->assign (head, head + strlen (head));
        const float* v = (const float*) img.data.data ();
        std::vector<uint8_t> scan ((size_t) img.width * 4);
        bool rle = img.width >= 8 && img.width < 32768;
        for (uint32_t y = 0; y < img.height; y++)
        {
            for (uint32_t x = 0; x < img.width; x++)
            {
                float_to_rgbe (&v[((size_t) y * img.width + x) * 3], &scan[(size_t) x * 4]);
            }
            if (!rle)
            {
                out->insert (out->end (), scan.begin (), scan.end ());
                continue;
            }
            out->push_back (2);
            out->push_back (2);
            out->push_back ((uint8_t) (img.width >> 8));
            out->push_back ((uint8_t) img.width);
            for (int c = 0; c < 4; c++)
            {
                uint32_t x = 0;
                while (x < img.width)
                {
                    uint32_t run = 1;
                    while (x + run < img.width && run < 127
                           && scan[(size_t) (x + run) * 4 + (size_t) c] == scan[(size_t) x * 4 + (size_t) c])
                    {
                        run++;
                    }
                    if (run >= 3)
                    {
                        out->push_back ((uint8_t) (128 + run));
                        out->push_back (scan[(size_t) x * 4 + (size_t) c]);
                        x += run;
                        continue;
                    }
                    uint32_t n = 0;
                    while (x + n < img.width && n < 128)
                    {
                        uint32_t r = 1;
                        while (
                            x + n + r < img.width && r < 3
                            && scan[(size_t) (x + n + r) * 4 + (size_t) c] == scan[(size_t) (x + n) * 4 + (size_t) c])
                        {
                            r++;
                        }
                        if (r >= 3)
                        {
                            break;
                        }
                        n++;
                    }
                    if (n == 0)
                    {
                        n = 1;
                    }
                    out->push_back ((uint8_t) n);
                    for (uint32_t k = 0; k < n; k++)
                    {
                        out->push_back (scan[(size_t) (x + k) * 4 + (size_t) c]);
                    }
                    x += n;
                }
            }
        }
        (void) err;
        (void) err_len;
        return true;
    }

} // namespace codecs
