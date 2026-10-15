#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace psd
{

    struct Reader
    {
        const uint8_t* data;
        size_t len;
        size_t pos;
        bool ok;
    };

    Reader reader (const uint8_t* data, size_t len);
    size_t remaining (const Reader& r);
    uint8_t rd_u8 (Reader* r);
    uint16_t rd_u16 (Reader* r);
    int16_t rd_i16 (Reader* r);
    uint32_t rd_u32 (Reader* r);
    int32_t rd_i32 (Reader* r);
    uint64_t rd_u64 (Reader* r);
    double rd_f64 (Reader* r);
    const uint8_t* rd_bytes (Reader* r, size_t n);
    void rd_skip (Reader* r, size_t n);
    std::string rd_key4 (Reader* r);
    std::string rd_unicode (Reader* r);
    std::string rd_pascal (Reader* r, size_t pad);

    void wr_u8 (std::vector<uint8_t>* w, uint8_t v);
    void wr_u16 (std::vector<uint8_t>* w, uint16_t v);
    void wr_u32 (std::vector<uint8_t>* w, uint32_t v);
    void wr_u64 (std::vector<uint8_t>* w, uint64_t v);
    void wr_f64 (std::vector<uint8_t>* w, double v);
    void wr_bytes (std::vector<uint8_t>* w, const void* p, size_t n);
    void wr_key4 (std::vector<uint8_t>* w, const char* k);
    void wr_unicode (std::vector<uint8_t>* w, const std::string& utf8, bool nul);
    void wr_pascal (std::vector<uint8_t>* w, const std::string& s, size_t pad);
    void put_u32_at (std::vector<uint8_t>* w, size_t at, uint32_t v);
    void put_u64_at (std::vector<uint8_t>* w, size_t at, uint64_t v);
    void pad_to (std::vector<uint8_t>* w, size_t start, size_t multiple);

    std::vector<uint8_t> packbits_encode (const uint8_t* row, size_t n);
    bool packbits_decode (const uint8_t* src, size_t n, uint8_t* out, size_t out_len);
    bool zip_decode (const uint8_t* src, size_t n, uint8_t* out, size_t out_len);
    std::vector<uint8_t> zip_encode (const uint8_t* src, size_t n);
    void unpredict (uint8_t* data, size_t w, size_t h, int depth);
    void predict (uint8_t* data, size_t w, size_t h, int depth);

} // namespace psd
