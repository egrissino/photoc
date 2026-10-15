#pragma once

#include <imgui.h>

namespace ui
{

    void opt_label (const char* text);
    void opt_gap ();
    void rgb_to_hsv (const float c[3], float* h, float* s, float* v);
    void hsv_to_rgb (float h, float s, float v, float out[3]);
    ImU32 rgb_u32 (const float c[3]);
    void blend_combo (const char* id, int* mode, float width);

} // namespace ui
