#pragma once

#include "codecs/codecs.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "text/fonts.h"
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace io
{

    struct IoTiming
    {
        double read;
        double decode;
        double build;
        double smart;
        double flatten;
        double encode;
        double write;
    };

    struct ImportResult
    {
        doc::Document document;
        std::vector<std::string> warnings;
        IoTiming timing;
    };

    double monotonic_seconds ();

    bool read_file (const char* path, std::vector<uint8_t>* out, char* err, size_t err_len);
    bool write_file (const char* path, const std::vector<uint8_t>& data, char* err, size_t err_len);
    std::string file_stem (const char* path);

    bool image_to_document (doc::IdSource& ids, const std::string& name, const codecs::Image& img, ImportResult* out,
        char* err, size_t err_len);
    bool document_to_image (const doc::Document& d, const compose::RenderSettings& settings, codecs::Image* out,
        std::vector<std::string>* warnings, char* err, size_t err_len);

    bool import_bytes (doc::IdSource& ids, const std::string& name, const uint8_t* bytes, size_t len, ImportResult* out,
        char* err, size_t err_len);
    bool export_bytes_timed (const doc::Document& d, codecs::Format format, const codecs::EncodeOptions& opts,
        const compose::RenderSettings& settings, std::vector<uint8_t>* out, std::vector<std::string>* warnings,
        char* err, size_t err_len, IoTiming* timing);
    bool export_bytes (const doc::Document& d, codecs::Format format, const codecs::EncodeOptions& opts,
        const compose::RenderSettings& settings, std::vector<uint8_t>* out, std::vector<std::string>* warnings,
        char* err, size_t err_len);

    bool contents_from_bytes (doc::IdSource& ids, text::FontDb* fonts, const std::string& name,
        std::shared_ptr<const std::vector<uint8_t>> bytes, doc::SmartContents* out, std::vector<std::string>* warnings,
        char* err, size_t err_len);
    void decode_smart_contents (
        doc::IdSource& ids, text::FontDb* fonts, doc::Document* d, std::vector<std::string>* warnings, int depth);
    bool render_smart_layer (const doc::Document& d, doc::Layer* l, const color::CmykSpace* cmyk);

    bool open_file (
        doc::IdSource& ids, text::FontDb* fonts, const char* path, ImportResult* out, char* err, size_t err_len);
    bool save_file (const doc::Document& d, text::FontDb* fonts, const char* path, const char* format_ext,
        const codecs::EncodeOptions& opts, const compose::RenderSettings& settings, std::vector<std::string>* warnings,
        char* err, size_t err_len);
    bool save_file_timed (const doc::Document& d, text::FontDb* fonts, const char* path, const char* format_ext,
        const codecs::EncodeOptions& opts, const compose::RenderSettings& settings, std::vector<std::string>* warnings,
        char* err, size_t err_len, IoTiming* timing);

} // namespace io
