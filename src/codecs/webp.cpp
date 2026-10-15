#include "codecs/codecs.h"
#include <math.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/encode.h>

namespace codecs
{

    /* WebP through libwebp: read as 8-bit RGB or RGBA (the first frame of an animation is not
       handled); written lossy at the quality setting, or lossless. */

    bool webp_decode (const uint8_t* bytes, size_t len, const Limits& limits, Image* out, char* err, size_t err_len)
    {
        WebPBitstreamFeatures f;
        if (WebPGetFeatures (bytes, len, &f) != VP8_STATUS_OK)
        {
            set_error (err, err_len, "WebP: the file can't be read");
            return false;
        }
        if (f.has_animation)
        {
            set_error (err, err_len, "WebP: animated WebP files are not supported");
            return false;
        }
        ChannelLayout layout = f.has_alpha ? ChannelLayout::Rgba : ChannelLayout::Rgb;
        if (!limits.check ((uint32_t) f.width, (uint32_t) f.height, layout, Sample::U8, err, err_len))
        {
            return false;
        }
        size_t n = f.has_alpha ? 4 : 3, stride = (size_t) f.width * n;
        std::vector<uint8_t> data (stride * (size_t) f.height);
        uint8_t* ok = f.has_alpha ? WebPDecodeRGBAInto (bytes, len, data.data (), data.size (), (int) stride)
                                  : WebPDecodeRGBInto (bytes, len, data.data (), data.size (), (int) stride);
        if (ok == NULL)
        {
            set_error (err, err_len, "WebP: the image data is damaged");
            return false;
        }
        return image_from_raw ((uint32_t) f.width, (uint32_t) f.height, layout, Sample::U8, data, out, err, err_len);
    }

    bool webp_encode (const Image& img, const EncodeOptions& opts, std::vector<uint8_t>* out, char* err, size_t err_len)
    {
        if (img.width > 16383 || img.height > 16383)
        {
            set_error (err, err_len, "WebP images are at most 16383 px a side");
            return false;
        }
        bool alpha   = layout_has_alpha (img.layout);
        int stride   = (int) (img.width * (alpha ? 4 : 3));
        uint8_t* buf = NULL;
        size_t n;
        if (opts.webp_lossless)
        {
            n = alpha ? WebPEncodeLosslessRGBA (img.data.data (), (int) img.width, (int) img.height, stride, &buf)
                      : WebPEncodeLosslessRGB (img.data.data (), (int) img.width, (int) img.height, stride, &buf);
        }
        else
        {
            float q = (float) opts.webp_quality;
            n       = alpha ? WebPEncodeRGBA (img.data.data (), (int) img.width, (int) img.height, stride, q, &buf)
                            : WebPEncodeRGB (img.data.data (), (int) img.width, (int) img.height, stride, q, &buf);
        }
        if (n == 0 || buf == NULL)
        {
            set_error (err, err_len, "WebP: encoding failed");
            return false;
        }
        out->assign (buf, buf + n);
        WebPFree (buf);
        return true;
    }

} // namespace codecs
