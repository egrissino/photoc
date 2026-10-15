#include "raster/interrupt.h"

#include <math.h>
#include <stddef.h>

namespace raster
{

    Interrupt Interrupt::none ()
    {
        Interrupt i = {NULL, NULL, NULL};
        return i;
    }

    bool Interrupt::cancelled () const
    {
        return cancel != NULL && cancel (user);
    }

    void Interrupt::report (float fraction) const
    {
        if (progress == NULL || !isfinite (fraction))
        {
            return;
        }
        if (fraction < 0.0f)
        {
            fraction = 0.0f;
        }
        if (fraction > 1.0f)
        {
            fraction = 1.0f;
        }
        progress (user, fraction);
    }

} // namespace raster
