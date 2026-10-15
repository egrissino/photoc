#pragma once

#include <imgui.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace ui
{

    bool rasterize_icon (const char* svg, int px, std::vector<uint8_t>* alpha);

    class IconCache
    {
    public:
        IconCache ();
        ~IconCache ();
        unsigned int texture (const char* name, int px);
        void clear ();

        IconCache (const IconCache&)            = delete;
        IconCache& operator= (const IconCache&) = delete;

    private:
        struct Entry
        {
            std::string name;
            int px;
            unsigned int tex;
        };
        std::vector<Entry> entries_;
    };

    void draw_icon (ImDrawList* dl, IconCache* cache, const char* name, ImVec2 pos, float size, ImU32 color);

} // namespace ui
