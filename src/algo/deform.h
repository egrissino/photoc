#pragma once

#include "algo/transform.h"
#include "geom/geom.h"
#include "raster/raster.h"
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace algo
{

    enum class DeformKind : uint8_t
    {
        Patch,
        Style,
        Pins,
        Homography,
    };

    enum class WarpStyle : uint8_t
    {
        Arc,
        ArcLower,
        ArcUpper,
        Arch,
        Bulge,
        ShellLower,
        ShellUpper,
        Flag,
        Wave,
        Fish,
        Rise,
        Fisheye,
        Inflate,
        Squeeze,
        Twist,
    };

    const int WARP_STYLE_COUNT = 15;
    extern const char* const WARP_STYLE_IDS[WARP_STYLE_COUNT];
    extern const char* const WARP_STYLE_LABELS[WARP_STYLE_COUNT];

    enum class PinMode : uint8_t
    {
        Rigid,
        Normal,
        Distort,
    };

    /* A forward map of the plane, over a source rectangle: a cubic Bezier patch (16 control
       points, row by row from the top left), a preset warp style, pins (moving least squares) or a
       homography. */
    struct Deform
    {
        DeformKind kind;
        double rect[4];
        double pts[16][2];
        WarpStyle style;
        bool vertical;
        double bend;
        double hdist;
        double vdist;
        std::vector<double> from;
        std::vector<double> to;
        PinMode mode;
        Homography h;

        static Deform patch_identity (const double rect[4]);
    };

    bool warp_style_from_id (const char* id, WarpStyle* out);
    void deform_point (const Deform& d, double x, double y, double* ox, double* oy);
    void style_to_patch (const Deform& style, Deform* out);
    void fit_patch (const double rect[4], const std::vector<double>& targets, int samples, double out[16][2]);
    raster::Surface mesh_warp (const raster::Surface& src, geom::Rect area, const Deform& d, Interp interp);
    void seam_carve (const std::vector<float>& px, size_t w, size_t h, size_t ch, size_t nw, size_t nh,
        const std::vector<float>* protect, std::vector<float>* out);

} // namespace algo
