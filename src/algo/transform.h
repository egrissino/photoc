#pragma once

#include "geom/geom.h"
#include "raster/raster.h"

namespace algo
{

    struct Homography
    {
        double m[9];

        static Homography identity ();
        static Homography affine (double a, double b, double c, double d, double e, double f);
        static bool rect_to_quad (const double r[4], const double quad[4][2], Homography* out);
        bool is_identity () const;
        bool is_affine () const;
        void apply (double x, double y, double* ox, double* oy) const;
        Homography mul (const Homography& o) const;
        bool inverse (Homography* out) const;
        double local_scale (double x, double y) const;
    };

    enum class Interp
    {
        Nearest,
        Bilinear,
        Bicubic,
    };

    Interp interp_from_str (const char* s);
    raster::Surface warp_surface (const raster::Surface& src, geom::Rect src_rect, const Homography& h, Interp interp);
    raster::Surface warp_gray (const raster::Surface& s, const Homography& h, Interp interp);
    raster::Surface warp_gray_selected (
        const raster::Surface& s, const raster::Surface& sel, const Homography& h, Interp interp);
    void split_selected (
        const raster::Surface& surf, const raster::Surface& sel, raster::Surface* lifted, raster::Surface* rest);
    void composite_over (raster::Surface* dst, const raster::Surface& top);

} // namespace algo
