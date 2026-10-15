#include "psd/bytes.h"

#include <string.h>
#include <zlib.h>

namespace psd
{

    /* Big-endian reading and writing for PSD, PackBits and ZIP (with and without prediction). A
       reader that runs past its data returns zeros and clears `ok`. */

    Reader reader (const uint8_t* data, size_t len)
    {
        Reader r = {data, len, 0, true};
        return r;
    }

    size_t remaining (const Reader& r)
    {
        return r.pos <= r.len ? r.len - r.pos : 0;
    }

    const uint8_t* rd_bytes (Reader* r, size_t n)
    {
        if (!r->ok || n > remaining (*r))
        {
            r->ok  = false;
            r->pos = r->len;
            return NULL;
        }
        const uint8_t* p = r->data + r->pos;
        r->pos += n;
        return p;
    }

    void rd_skip (Reader* r, size_t n)
    {
        rd_bytes (r, n);
    }

    uint8_t rd_u8 (Reader* r)
    {
        const uint8_t* p = rd_bytes (r, 1);
        return p != NULL ? p[0] : 0;
    }

    uint16_t rd_u16 (Reader* r)
    {
        const uint8_t* p = rd_bytes (r, 2);
        return p != NULL ? (uint16_t) (p[0] << 8 | p[1]) : 0;
    }

    int16_t rd_i16 (Reader* r)
    {
        return (int16_t) rd_u16 (r);
    }

    uint32_t rd_u32 (Reader* r)
    {
        const uint8_t* p = rd_bytes (r, 4);
        return p != NULL ? (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3] : 0;
    }

    int32_t rd_i32 (Reader* r)
    {
        return (int32_t) rd_u32 (r);
    }

    uint64_t rd_u64 (Reader* r)
    {
        uint64_t hi = rd_u32 (r);
        return hi << 32 | rd_u32 (r);
    }

    double rd_f64 (Reader* r)
    {
        uint64_t v = rd_u64 (r);
        double d;
        memcpy (&d, &v, sizeof d);
        return d;
    }

    std::string rd_key4 (Reader* r)
    {
        const uint8_t* p = rd_bytes (r, 4);
        return p != NULL ? std::string ((const char*) p, 4) : std::string ("    ");
    }

    void append_utf8 (std::string* out, uint32_t c)
    {
        if (c < 0x80)
        {
            out->push_back ((char) c);
        }
        else if (c < 0x800)
        {
            out->push_back ((char) (0xC0 | c >> 6));
            out->push_back ((char) (0x80 | (c & 0x3F)));
        }
        else if (c < 0x10000)
        {
            out->push_back ((char) (0xE0 | c >> 12));
            out->push_back ((char) (0x80 | ((c >> 6) & 0x3F)));
            out->push_back ((char) (0x80 | (c & 0x3F)));
        }
        else
        {
            out->push_back ((char) (0xF0 | c >> 18));
            out->push_back ((char) (0x80 | ((c >> 12) & 0x3F)));
            out->push_back ((char) (0x80 | ((c >> 6) & 0x3F)));
            out->push_back ((char) (0x80 | (c & 0x3F)));
        }
    }

    /* A PSD Unicode string (UTF-16BE with a length in code units) as UTF-8, without a final NUL. */
    std::string rd_unicode (Reader* r)
    {
        uint32_t n = rd_u32 (r);
        std::string out;
        if (n > remaining (*r) / 2)
        {
            r->ok = false;
            return out;
        }
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t c = rd_u16 (r);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < n)
            {
                uint32_t lo = rd_u16 (r);
                i++;
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (c == 0 && i + 1 == n)
            {
                break;
            }
            append_utf8 (&out, c);
        }
        return out;
    }

    /* A Pascal string padded so its whole length (with the count byte) is a multiple of `pad`. */
    std::string rd_pascal (Reader* r, size_t pad)
    {
        uint8_t n        = rd_u8 (r);
        const uint8_t* p = rd_bytes (r, n);
        std::string s    = p != NULL ? std::string ((const char*) p, n) : std::string ();
        size_t total     = 1 + (size_t) n;
        if (pad > 1 && total % pad != 0)
        {
            rd_skip (r, pad - total % pad);
        }
        return s;
    }

    void wr_u8 (std::vector<uint8_t>* w, uint8_t v)
    {
        w->push_back (v);
    }

    void wr_u16 (std::vector<uint8_t>* w, uint16_t v)
    {
        w->push_back ((uint8_t) (v >> 8));
        w->push_back ((uint8_t) v);
    }

    void wr_u32 (std::vector<uint8_t>* w, uint32_t v)
    {
        for (int s = 24; s >= 0; s -= 8)
        {
            w->push_back ((uint8_t) (v >> s));
        }
    }

    void wr_u64 (std::vector<uint8_t>* w, uint64_t v)
    {
        wr_u32 (w, (uint32_t) (v >> 32));
        wr_u32 (w, (uint32_t) v);
    }

    void wr_f64 (std::vector<uint8_t>* w, double v)
    {
        uint64_t u;
        memcpy (&u, &v, sizeof u);
        wr_u64 (w, u);
    }

    void wr_bytes (std::vector<uint8_t>* w, const void* p, size_t n)
    {
        const uint8_t* b = (const uint8_t*) p;
        w->insert (w->end (), b, b + n);
    }

    void wr_key4 (std::vector<uint8_t>* w, const char* k)
    {
        char buf[4] = {' ', ' ', ' ', ' '};
        for (int i = 0; i < 4 && k[i] != 0; i++)
        {
            buf[i] = k[i];
        }
        wr_bytes (w, buf, 4);
    }

    /* UTF-8 to a PSD Unicode string; `nul` adds the terminating NUL Photoshop writes in
       descriptors. */
    void wr_unicode (std::vector<uint8_t>* w, const std::string& utf8, bool nul)
    {
        std::vector<uint16_t> units;
        for (size_t i = 0; i < utf8.size ();)
        {
            uint8_t c = (uint8_t) utf8[i];
            uint32_t cp;
            size_t n;
            if (c < 0x80)
            {
                cp = c, n = 1;
            }
            else if ((c >> 5) == 6)
            {
                cp = c & 0x1F, n = 2;
            }
            else if ((c >> 4) == 14)
            {
                cp = c & 0x0F, n = 3;
            }
            else
            {
                cp = c & 0x07, n = 4;
            }
            for (size_t k = 1; k < n && i + k < utf8.size (); k++)
            {
                cp = cp << 6 | ((uint8_t) utf8[i + k] & 0x3F);
            }
            i += n;
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                units.push_back ((uint16_t) (0xD800 + (cp >> 10)));
                units.push_back ((uint16_t) (0xDC00 + (cp & 0x3FF)));
            }
            else
            {
                units.push_back ((uint16_t) cp);
            }
        }
        if (nul)
        {
            units.push_back (0);
        }
        wr_u32 (w, (uint32_t) units.size ());
        for (size_t i = 0; i < units.size (); i++)
        {
            wr_u16 (w, units[i]);
        }
    }

    void wr_pascal (std::vector<uint8_t>* w, const std::string& s, size_t pad)
    {
        size_t n = s.size () > 255 ? 255 : s.size ();
        wr_u8 (w, (uint8_t) n);
        wr_bytes (w, s.data (), n);
        size_t total = 1 + n;
        while (pad > 1 && total % pad != 0)
        {
            wr_u8 (w, 0);
            total++;
        }
    }

    void put_u32_at (std::vector<uint8_t>* w, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            (*w)[at + (size_t) i] = (uint8_t) (v >> (24 - 8 * i));
        }
    }

    void put_u64_at (std::vector<uint8_t>* w, size_t at, uint64_t v)
    {
        put_u32_at (w, at, (uint32_t) (v >> 32));
        put_u32_at (w, at + 4, (uint32_t) v);
    }

    void pad_to (std::vector<uint8_t>* w, size_t start, size_t multiple)
    {
        while ((w->size () - start) % multiple != 0)
        {
            w->push_back (0);
        }
    }

    /* PackBits: runs of 2..128 equal bytes as (1 - n, byte), literals as (n - 1, bytes). */
    std::vector<uint8_t> packbits_encode (const uint8_t* row, size_t n)
    {
        std::vector<uint8_t> out;
        size_t i = 0;
        while (i < n)
        {
            size_t run = 1;
            while (i + run < n && run < 128 && row[i + run] == row[i])
            {
                run++;
            }
            if (run >= 2)
            {
                out.push_back ((uint8_t) (257 - run));
                out.push_back (row[i]);
                i += run;
                continue;
            }
            size_t start = i, lit = 0;
            while (i < n && lit < 128)
            {
                if (i + 1 < n && row[i] == row[i + 1])
                {
                    break;
                }
                i++;
                lit++;
            }
            out.push_back ((uint8_t) (lit - 1));
            out.insert (out.end (), row + start, row + start + lit);
        }
        return out;
    }

    bool packbits_decode (const uint8_t* src, size_t n, uint8_t* out, size_t out_len)
    {
        size_t i = 0, o = 0;
        while (i < n && o < out_len)
        {
            int8_t h = (int8_t) src[i++];
            if (h >= 0)
            {
                size_t cnt = (size_t) h + 1;
                if (i + cnt > n || o + cnt > out_len)
                {
                    return false;
                }
                memcpy (out + o, src + i, cnt);
                i += cnt;
                o += cnt;
            }
            else if (h != -128)
            {
                size_t cnt = (size_t) (1 - h);
                if (i >= n || o + cnt > out_len)
                {
                    return false;
                }
                memset (out + o, src[i++], cnt);
                o += cnt;
            }
        }
        return o == out_len;
    }

    bool zip_decode (const uint8_t* src, size_t n, uint8_t* out, size_t out_len)
    {
        z_stream zs;
        memset (&zs, 0, sizeof zs);
        if (inflateInit (&zs) != Z_OK)
        {
            return false;
        }
        zs.next_in   = (Bytef*) src;
        zs.avail_in  = (uInt) n;
        zs.next_out  = out;
        zs.avail_out = (uInt) out_len;
        int rc       = inflate (&zs, Z_FINISH);
        size_t got   = out_len - zs.avail_out;
        inflateEnd (&zs);
        return (rc == Z_STREAM_END || rc == Z_OK || rc == Z_BUF_ERROR) && got == out_len;
    }

    std::vector<uint8_t> zip_encode (const uint8_t* src, size_t n)
    {
        uLongf len = compressBound ((uLong) n);
        std::vector<uint8_t> out (len);
        if (compress2 (out.data (), &len, src, (uLong) n, 6) != Z_OK)
        {
            return std::vector<uint8_t> ();
        }
        out.resize (len);
        return out;
    }

    /* Undoes ZIP prediction: per-row deltas of 8-bit bytes, 16-bit big-endian samples, or (32 bit)
       bytes after splitting each row into four byte planes. */
    void unpredict (uint8_t* data, size_t w, size_t h, int depth)
    {
        if (depth == 8)
        {
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* row = data + y * w;
                for (size_t x = 1; x < w; x++)
                {
                    row[x] = (uint8_t) (row[x] + row[x - 1]);
                }
            }
        }
        else if (depth == 16)
        {
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* row  = data + y * w * 2;
                uint16_t prev = (uint16_t) (row[0] << 8 | row[1]);
                for (size_t x = 1; x < w; x++)
                {
                    uint16_t v     = (uint16_t) ((row[x * 2] << 8 | row[x * 2 + 1]) + prev);
                    row[x * 2]     = (uint8_t) (v >> 8);
                    row[x * 2 + 1] = (uint8_t) v;
                    prev           = v;
                }
            }
        }
        else
        {
            std::vector<uint8_t> tmp (w * 4);
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* row = data + y * w * 4;
                for (size_t x = 1; x < w * 4; x++)
                {
                    row[x] = (uint8_t) (row[x] + row[x - 1]);
                }
                for (size_t x = 0; x < w; x++)
                {
                    for (size_t b = 0; b < 4; b++)
                    {
                        tmp[x * 4 + b] = row[b * w + x];
                    }
                }
                memcpy (row, tmp.data (), w * 4);
            }
        }
    }

    void predict (uint8_t* data, size_t w, size_t h, int depth)
    {
        if (depth == 8)
        {
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* row = data + y * w;
                for (size_t x = w; x-- > 1;)
                {
                    row[x] = (uint8_t) (row[x] - row[x - 1]);
                }
            }
        }
        else if (depth == 16)
        {
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* row = data + y * w * 2;
                for (size_t x = w; x-- > 1;)
                {
                    uint16_t v     = (uint16_t) (row[x * 2] << 8 | row[x * 2 + 1]),
                             p     = (uint16_t) (row[x * 2 - 2] << 8 | row[x * 2 - 1]);
                    uint16_t d     = (uint16_t) (v - p);
                    row[x * 2]     = (uint8_t) (d >> 8);
                    row[x * 2 + 1] = (uint8_t) d;
                }
            }
        }
        else
        {
            std::vector<uint8_t> tmp (w * 4);
            for (size_t y = 0; y < h; y++)
            {
                uint8_t* row = data + y * w * 4;
                for (size_t x = 0; x < w; x++)
                {
                    for (size_t b = 0; b < 4; b++)
                    {
                        tmp[b * w + x] = row[x * 4 + b];
                    }
                }
                memcpy (row, tmp.data (), w * 4);
                for (size_t x = w * 4; x-- > 1;)
                {
                    row[x] = (uint8_t) (row[x] - row[x - 1]);
                }
            }
        }
    }

} // namespace psd
