#pragma once

#include "doc/adjust.h"
#include "engine/params.h"

namespace ui
{

    struct AdjustEditor
    {
        doc::AdjustmentKind kind;
        engine::Value params;
        int channel;
        int tone;
        int row;
        int curve_drag;
    };

    void adjust_editor_init (AdjustEditor* ed, const doc::Adjustment& adj);
    void adjust_editor_draw (AdjustEditor* ed, bool* changed, bool* finished);
    bool adjust_has_dialog (doc::AdjustmentKind kind);

} // namespace ui
