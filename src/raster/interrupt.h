#pragma once

namespace raster
{

    struct Interrupt
    {
        bool (*cancel) (void* user);
        void (*progress) (void* user, float fraction);
        void* user;

        static Interrupt none ();
        bool cancelled () const;
        void report (float fraction) const;
    };

} // namespace raster
