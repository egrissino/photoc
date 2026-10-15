#include "engine/filter.h"
#include "engine/params.h"
#include "ui/app.h"
#include <algorithm>
#include <imgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace ui
{

    /* One dialog for every filter with settings: its fields come from FILTER_DIALOGS. Preview shows
       the result on a copy of the document; OK runs the command. */

    enum class FieldKind
    {
        Number,
        Toggle,
        Choice,
        Seed,
        Color,
        Matrix,
        Document,
    };

    /* A setting: Number (a slider), Toggle, Choice (`format` the shown names and `ids` the values,
       both '|'-separated; `value` the default's index), Seed (Randomize), Color (`ids` the default
       "#rrggbb"), Matrix (Custom's 5 x 5 grid) or Document (an open document's index, -1 for
       none). */
    struct FilterField
    {
        const char* key;
        const char* label;
        FieldKind kind;
        float min;
        float max;
        float value;
        const char* format;
        bool log;
        const char* ids;
    };

    struct FilterDialog
    {
        const char* id;
        const char* title;
        int count;
        FilterField fields[10];
    };

    const int FILTER_SLOTS = 48;

    const FilterDialog FILTER_DIALOGS[] = {
        {"filter.blur.gaussianBlur", "Gaussian Blur", 1,
            {{"radius", "Radius", FieldKind::Number, 0.1f, 1000.0f, 1.0f, "%.1f px", true, ""}}},
        {"filter.blur.boxBlur", "Box Blur", 1,
            {{"radius", "Radius", FieldKind::Number, 1.0f, 2000.0f, 1.0f, "%.0f px", true, ""}}},
        {"filter.blur.motionBlur", "Motion Blur", 2,
            {{"angle", "Angle", FieldKind::Number, -360.0f, 360.0f, 0.0f, "%.0f°", false, ""},
                {"distance", "Distance", FieldKind::Number, 1.0f, 2000.0f, 10.0f, "%.0f px", true, ""}}},
        {"filter.blur.lensBlur", "Lens Blur", 3,
            {{"radius", "Iris Radius", FieldKind::Number, 0.0f, 100.0f, 15.0f, "%.0f", false, ""},
                {"brightness", "Specular Brightness", FieldKind::Number, 0.0f, 100.0f, 0.0f, "%.0f", false, ""},
                {"threshold", "Specular Threshold", FieldKind::Number, 0.0f, 255.0f, 255.0f, "%.0f", false, ""}}},
        {"filter.blur.radialBlur", "Radial Blur", 4,
            {{"amount", "Amount", FieldKind::Number, 1.0f, 100.0f, 10.0f, "%.0f", false, ""},
                {"method", "Blur Method", FieldKind::Choice, 0, 0, 0.0f, "Spin|Zoom", false, "spin|zoom"},
                {"centerX", "Center X", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"centerY", "Center Y", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""}}},
        {"filter.blur.smartBlur", "Smart Blur", 3,
            {{"radius", "Radius", FieldKind::Number, 0.1f, 100.0f, 3.0f, "%.1f", true, ""},
                {"threshold", "Threshold", FieldKind::Number, 0.1f, 100.0f, 25.0f, "%.1f", false, ""},
                {"mode", "Mode", FieldKind::Choice, 0, 0, 0.0f, "Normal|Edge Only|Overlay Edge", false,
                    "normal|edgeOnly|overlayEdge"}}},
        {"filter.blur.surfaceBlur", "Surface Blur", 2,
            {{"radius", "Radius", FieldKind::Number, 1.0f, 100.0f, 5.0f, "%.0f px", true, ""},
                {"threshold", "Threshold", FieldKind::Number, 2.0f, 255.0f, 15.0f, "%.0f levels", false, ""}}},
        {"filter.blurGallery.fieldBlur", "Field Blur", 1,
            {{"blur", "Blur", FieldKind::Number, 0.0f, 500.0f, 15.0f, "%.0f px", true, ""}}},
        {"filter.blurGallery.irisBlur", "Iris Blur", 6,
            {{"blur", "Blur", FieldKind::Number, 0.0f, 500.0f, 15.0f, "%.0f px", true, ""},
                {"centerX", "Center X", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"centerY", "Center Y", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"radiusX", "Width", FieldKind::Number, 0.01f, 2.0f, 0.3f, "%.2f", false, ""},
                {"radiusY", "Height", FieldKind::Number, 0.01f, 2.0f, 0.3f, "%.2f", false, ""},
                {"feather", "Feather", FieldKind::Number, 0.0f, 100.0f, 50.0f, "%.0f %%", false, ""}}},
        {"filter.blurGallery.tiltShift", "Tilt-Shift", 5,
            {{"blur", "Blur", FieldKind::Number, 0.0f, 500.0f, 15.0f, "%.0f px", true, ""},
                {"centerY", "Center", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"angle", "Angle", FieldKind::Number, -90.0f, 90.0f, 0.0f, "%.0f°", false, ""},
                {"focus", "Focus Band", FieldKind::Number, 0.0f, 100.0f, 10.0f, "%.0f %%", false, ""},
                {"transition", "Transition", FieldKind::Number, 0.0f, 100.0f, 15.0f, "%.0f %%", false, ""}}},
        {"filter.blurGallery.spinBlur", "Spin Blur", 5,
            {{"angle", "Blur Angle", FieldKind::Number, 0.0f, 360.0f, 15.0f, "%.0f°", false, ""},
                {"centerX", "Center X", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"centerY", "Center Y", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"radiusX", "Width", FieldKind::Number, 0.01f, 2.0f, 0.3f, "%.2f", false, ""},
                {"radiusY", "Height", FieldKind::Number, 0.01f, 2.0f, 0.3f, "%.2f", false, ""}}},
        {"filter.distort.displace", "Displace", 3,
            {{"horizontalScale", "Horizontal Scale", FieldKind::Number, -999.0f, 999.0f, 10.0f, "%.0f %%", false, ""},
                {"verticalScale", "Vertical Scale", FieldKind::Number, -999.0f, 999.0f, 10.0f, "%.0f %%", false, ""},
                {"undefined", "Undefined Areas", FieldKind::Choice, 0, 0, 1.0f, "Wrap Around|Repeat Edge Pixels", false,
                    "wrap|repeat"}}},
        {"filter.distort.pinch", "Pinch", 1,
            {{"amount", "Amount", FieldKind::Number, -100.0f, 100.0f, 50.0f, "%.0f %%", false, ""}}},
        {"filter.distort.polarCoordinates", "Polar Coordinates", 1,
            {{"mode", "", FieldKind::Choice, 0, 0, 0.0f, "Rectangular to Polar|Polar to Rectangular", false,
                "rectToPolar|polarToRect"}}},
        {"filter.distort.ripple", "Ripple", 2,
            {{"amount", "Amount", FieldKind::Number, -999.0f, 999.0f, 100.0f, "%.0f %%", false, ""},
                {"size", "Size", FieldKind::Choice, 0, 0, 1.0f, "Small|Medium|Large", false, "small|medium|large"}}},
        {"filter.distort.shear", "Shear", 2,
            {{"bend", "Bend", FieldKind::Number, -100.0f, 100.0f, 30.0f, "%.0f", false, ""},
                {"undefined", "Undefined Areas", FieldKind::Choice, 0, 0, 0.0f, "Wrap Around|Repeat Edge Pixels", false,
                    "wrap|repeat"}}},
        {"filter.distort.spherize", "Spherize", 2,
            {{"amount", "Amount", FieldKind::Number, -100.0f, 100.0f, 100.0f, "%.0f %%", false, ""},
                {"mode", "Mode", FieldKind::Choice, 0, 0, 0.0f, "Normal|Horizontal Only|Vertical Only", false,
                    "normal|horizontal|vertical"}}},
        {"filter.distort.twirl", "Twirl", 1,
            {{"angle", "Angle", FieldKind::Number, -999.0f, 999.0f, 50.0f, "%.0f°", false, ""}}},
        {"filter.distort.wave", "Wave", 10,
            {{"generators", "Number of Generators", FieldKind::Number, 1.0f, 999.0f, 5.0f, "%.0f", true, ""},
                {"minWavelength", "Wavelength Min", FieldKind::Number, 1.0f, 998.0f, 10.0f, "%.0f", true, ""},
                {"maxWavelength", "Wavelength Max", FieldKind::Number, 2.0f, 999.0f, 120.0f, "%.0f", true, ""},
                {"minAmplitude", "Amplitude Min", FieldKind::Number, 1.0f, 998.0f, 5.0f, "%.0f", true, ""},
                {"maxAmplitude", "Amplitude Max", FieldKind::Number, 1.0f, 999.0f, 35.0f, "%.0f", true, ""},
                {"scaleX", "Scale Horizontal", FieldKind::Number, 1.0f, 100.0f, 100.0f, "%.0f %%", false, ""},
                {"scaleY", "Scale Vertical", FieldKind::Number, 1.0f, 100.0f, 100.0f, "%.0f %%", false, ""},
                {"type", "Type", FieldKind::Choice, 0, 0, 0.0f, "Sine|Triangle|Square", false, "sine|triangle|square"},
                {"undefined", "Undefined Areas", FieldKind::Choice, 0, 0, 1.0f, "Wrap Around|Repeat Edge Pixels", false,
                    "wrap|repeat"},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.distort.zigZag", "ZigZag", 3,
            {{"amount", "Amount", FieldKind::Number, -100.0f, 100.0f, 10.0f, "%.0f", false, ""},
                {"ridges", "Ridges", FieldKind::Number, 1.0f, 20.0f, 5.0f, "%.0f", false, ""},
                {"style", "Style", FieldKind::Choice, 0, 0, 2.0f, "Around Center|Out From Center|Pond Ripples", false,
                    "aroundCenter|outFromCenter|pondRipples"}}},
        {"filter.noise.addNoise", "Add Noise", 4,
            {{"amount", "Amount", FieldKind::Number, 0.1f, 400.0f, 12.5f, "%.1f %%", true, ""},
                {"distribution", "Distribution", FieldKind::Choice, 0, 0, 0.0f, "Uniform|Gaussian", false,
                    "uniform|gaussian"},
                {"monochromatic", "Monochromatic", FieldKind::Toggle, 0.0f, 1.0f, 0.0f, "", false, ""},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.noise.dustAndScratches", "Dust & Scratches", 2,
            {{"radius", "Radius", FieldKind::Number, 1.0f, 100.0f, 1.0f, "%.0f px", true, ""},
                {"threshold", "Threshold", FieldKind::Number, 0.0f, 255.0f, 0.0f, "%.0f levels", false, ""}}},
        {"filter.noise.median", "Median", 1,
            {{"radius", "Radius", FieldKind::Number, 1.0f, 100.0f, 1.0f, "%.0f px", true, ""}}},
        {"filter.noise.reduceNoise", "Reduce Noise", 4,
            {{"strength", "Strength", FieldKind::Number, 0.0f, 10.0f, 6.0f, "%.0f", false, ""},
                {"preserveDetails", "Preserve Details", FieldKind::Number, 0.0f, 100.0f, 60.0f, "%.0f %%", false, ""},
                {"reduceColorNoise", "Reduce Color Noise", FieldKind::Number, 0.0f, 100.0f, 45.0f, "%.0f %%", false,
                    ""},
                {"sharpenDetails", "Sharpen Details", FieldKind::Number, 0.0f, 100.0f, 25.0f, "%.0f %%", false, ""}}},
        {"filter.pixelate.colorHalftone", "Color Halftone", 5,
            {{"maxRadius", "Max. Radius", FieldKind::Number, 4.0f, 127.0f, 8.0f, "%.0f px", false, ""},
                {"channel1", "Channel 1", FieldKind::Number, -360.0f, 360.0f, 108.0f, "%.0f°", false, ""},
                {"channel2", "Channel 2", FieldKind::Number, -360.0f, 360.0f, 162.0f, "%.0f°", false, ""},
                {"channel3", "Channel 3", FieldKind::Number, -360.0f, 360.0f, 90.0f, "%.0f°", false, ""},
                {"channel4", "Channel 4", FieldKind::Number, -360.0f, 360.0f, 45.0f, "%.0f°", false, ""}}},
        {"filter.pixelate.crystallize", "Crystallize", 2,
            {{"cellSize", "Cell Size", FieldKind::Number, 3.0f, 300.0f, 10.0f, "%.0f", true, ""},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.pixelate.mezzotint", "Mezzotint", 2,
            {{"type", "Type", FieldKind::Choice, 0, 0, 0.0f,
                 "Fine Dots|Medium Dots|Grainy Dots|Coarse Dots|Short Lines|Medium Lines|Long Lines|Short "
                 "Strokes|Medium Strokes|Long Strokes",
                 false,
                 "fineDots|mediumDots|grainyDots|coarseDots|shortLines|mediumLines|longLines|shortStrokes|"
                 "mediumStrokes|longStrokes"},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.pixelate.mosaic", "Mosaic", 1,
            {{"cellSize", "Cell Size", FieldKind::Number, 2.0f, 200.0f, 10.0f, "%.0f square", true, ""}}},
        {"filter.pixelate.pointillize", "Pointillize", 2,
            {{"cellSize", "Cell Size", FieldKind::Number, 3.0f, 300.0f, 5.0f, "%.0f", true, ""},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.render.fibers", "Fibers", 3,
            {{"variance", "Variance", FieldKind::Number, 1.0f, 64.0f, 16.0f, "%.0f", false, ""},
                {"strength", "Strength", FieldKind::Number, 1.0f, 64.0f, 4.0f, "%.0f", false, ""},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.render.lensFlare", "Lens Flare", 4,
            {{"brightness", "Brightness", FieldKind::Number, 10.0f, 300.0f, 100.0f, "%.0f %%", false, ""},
                {"centerX", "Flare Center X", FieldKind::Number, 0.0f, 1.0f, 0.3f, "%.2f", false, ""},
                {"centerY", "Flare Center Y", FieldKind::Number, 0.0f, 1.0f, 0.3f, "%.2f", false, ""},
                {"lens", "Lens Type", FieldKind::Choice, 0, 0, 0.0f, "50-300mm Zoom|35mm Prime|105mm Prime|Movie Prime",
                    false, "zoom50300|prime35|prime105|moviePrime"}}},
        {"filter.render.lightingEffects", "Lighting Effects", 9,
            {{"type", "Light", FieldKind::Choice, 0, 0, 0.0f, "Spot|Point|Infinite", false, "spot|omni|infinite"},
                {"color", "Color", FieldKind::Color, 0, 0, 0.0f, "", false, "#ffffff"},
                {"intensity", "Intensity", FieldKind::Number, 0.0f, 100.0f, 35.0f, "%.0f", false, ""},
                {"centerX", "Position X", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"centerY", "Position Y", FieldKind::Number, 0.0f, 1.0f, 0.5f, "%.2f", false, ""},
                {"radius", "Size", FieldKind::Number, 0.01f, 2.0f, 0.5f, "%.2f", false, ""},
                {"angle", "Angle", FieldKind::Number, -360.0f, 360.0f, -45.0f, "%.0f°", false, ""},
                {"ambience", "Ambience", FieldKind::Number, -100.0f, 100.0f, 8.0f, "%.0f", false, ""},
                {"height", "Texture Height", FieldKind::Number, 0.0f, 100.0f, 0.0f, "%.0f", false, ""}}},
        {"filter.sharpen.smartSharpen", "Smart Sharpen", 5,
            {{"amount", "Amount", FieldKind::Number, 1.0f, 500.0f, 150.0f, "%.0f %%", false, ""},
                {"radius", "Radius", FieldKind::Number, 0.1f, 64.0f, 1.0f, "%.1f px", true, ""},
                {"reduceNoise", "Reduce Noise", FieldKind::Number, 0.0f, 100.0f, 10.0f, "%.0f %%", false, ""},
                {"remove", "Remove", FieldKind::Choice, 0, 0, 1.0f, "Gaussian Blur|Lens Blur|Motion Blur", false,
                    "gaussian|lens|motion"},
                {"angle", "Angle (Motion)", FieldKind::Number, -360.0f, 360.0f, 0.0f, "%.0f°", false, ""}}},
        {"filter.sharpen.unsharpMask", "Unsharp Mask", 3,
            {{"amount", "Amount", FieldKind::Number, 1.0f, 500.0f, 100.0f, "%.0f %%", false, ""},
                {"radius", "Radius", FieldKind::Number, 0.1f, 1000.0f, 1.0f, "%.1f px", true, ""},
                {"threshold", "Threshold", FieldKind::Number, 0.0f, 255.0f, 0.0f, "%.0f levels", false, ""}}},
        {"filter.stylize.diffuse", "Diffuse", 2,
            {{"mode", "Mode", FieldKind::Choice, 0, 0, 0.0f, "Normal|Darken Only|Lighten Only|Anisotropic", false,
                 "normal|darkenOnly|lightenOnly|anisotropic"},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.stylize.emboss", "Emboss", 3,
            {{"angle", "Angle", FieldKind::Number, -360.0f, 360.0f, 135.0f, "%.0f°", false, ""},
                {"height", "Height", FieldKind::Number, 1.0f, 100.0f, 3.0f, "%.0f px", true, ""},
                {"amount", "Amount", FieldKind::Number, 1.0f, 500.0f, 100.0f, "%.0f %%", false, ""}}},
        {"filter.stylize.extrude", "Extrude", 7,
            {{"type", "Type", FieldKind::Choice, 0, 0, 0.0f, "Blocks|Pyramids", false, "blocks|pyramids"},
                {"size", "Size", FieldKind::Number, 2.0f, 255.0f, 30.0f, "%.0f px", false, ""},
                {"depth", "Depth", FieldKind::Number, 1.0f, 255.0f, 30.0f, "%.0f", false, ""},
                {"depthMode", "Depth From", FieldKind::Choice, 0, 0, 0.0f, "Random|Level-based", false,
                    "random|levelBased"},
                {"solidFronts", "Solid Front Faces", FieldKind::Toggle, 0.0f, 1.0f, 0.0f, "", false, ""},
                {"maskIncomplete", "Mask Incomplete Blocks", FieldKind::Toggle, 0.0f, 1.0f, 0.0f, "", false, ""},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.stylize.oilPaint", "Oil Paint", 7,
            {{"stylization", "Stylization", FieldKind::Number, 0.1f, 10.0f, 2.5f, "%.1f", false, ""},
                {"cleanliness", "Cleanliness", FieldKind::Number, 0.0f, 10.0f, 5.0f, "%.1f", false, ""},
                {"scale", "Scale", FieldKind::Number, 0.1f, 10.0f, 1.0f, "%.1f", false, ""},
                {"bristleDetail", "Bristle Detail", FieldKind::Number, 0.0f, 10.0f, 2.0f, "%.1f", false, ""},
                {"lighting", "Lighting", FieldKind::Toggle, 0.0f, 1.0f, 1.0f, "", false, ""},
                {"angle", "Angle", FieldKind::Number, -180.0f, 180.0f, -60.0f, "%.0f°", false, ""},
                {"shine", "Shine", FieldKind::Number, 0.0f, 10.0f, 1.0f, "%.1f", false, ""}}},
        {"filter.stylize.tiles", "Tiles", 4,
            {{"number", "Number of Tiles", FieldKind::Number, 1.0f, 99.0f, 10.0f, "%.0f", false, ""},
                {"maxOffset", "Maximum Offset", FieldKind::Number, 1.0f, 99.0f, 10.0f, "%.0f %%", false, ""},
                {"fill", "Fill Empty Area With", FieldKind::Choice, 0, 0, 0.0f,
                    "Background Color|Foreground Color|Inverse Image|Unaltered Image", false,
                    "background|foreground|inverse|unaltered"},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.stylize.traceContour", "Trace Contour", 2,
            {{"level", "Level", FieldKind::Number, 0.0f, 255.0f, 128.0f, "%.0f", false, ""},
                {"edge", "Edge", FieldKind::Choice, 0, 0, 1.0f, "Lower|Upper", false, "lower|upper"}}},
        {"filter.stylize.wind", "Wind", 3,
            {{"method", "Method", FieldKind::Choice, 0, 0, 0.0f, "Wind|Blast|Stagger", false, "wind|blast|stagger"},
                {"direction", "Direction", FieldKind::Choice, 0, 0, 0.0f, "From the Right|From the Left", false,
                    "fromRight|fromLeft"},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.video.deInterlace", "De-Interlace", 2,
            {{"eliminate", "Eliminate", FieldKind::Choice, 0, 0, 0.0f, "Odd Fields|Even Fields", false, "odd|even"},
                {"create", "Create New Fields by", FieldKind::Choice, 0, 0, 1.0f, "Duplication|Interpolation", false,
                    "duplication|interpolation"}}},
        {"filter.other.colorToAlpha", "Color to Alpha", 1,
            {{"color", "Color", FieldKind::Color, 0, 0, 0.0f, "", false, "#ffffff"}}},
        {"filter.other.custom", "Custom", 3,
            {{"scale", "Scale", FieldKind::Number, 1.0f, 9999.0f, 1.0f, "%.0f", true, ""},
                {"offset", "Offset", FieldKind::Number, -9999.0f, 9999.0f, 0.0f, "%.0f", false, ""},
                {"kernel", "", FieldKind::Matrix, -999.0f, 999.0f, 0.0f, "", false, ""}}},
        {"filter.other.highPass", "High Pass", 1,
            {{"radius", "Radius", FieldKind::Number, 0.1f, 1000.0f, 10.0f, "%.1f px", true, ""}}},
        {"filter.other.hsbHsl", "HSB/HSL Parameters", 2,
            {{"input", "Input Mode", FieldKind::Choice, 0, 0, 0.0f, "RGB|HSB|HSL", false, "rgb|hsb|hsl"},
                {"rowOrder", "Row Order", FieldKind::Choice, 0, 0, 1.0f, "RGB|HSB|HSL", false, "rgb|hsb|hsl"}}},
        {"filter.other.maximum", "Maximum", 1,
            {{"radius", "Radius", FieldKind::Number, 1.0f, 500.0f, 1.0f, "%.0f px", true, ""}}},
        {"filter.other.minimum", "Minimum", 1,
            {{"radius", "Radius", FieldKind::Number, 1.0f, 500.0f, 1.0f, "%.0f px", true, ""}}},
        {"filter.other.offset", "Offset", 2,
            {{"horizontal", "Horizontal", FieldKind::Number, -4000.0f, 4000.0f, 0.0f, "%.0f px right", false, ""},
                {"vertical", "Vertical", FieldKind::Number, -4000.0f, 4000.0f, 0.0f, "%.0f px down", false, ""}}},
        {"filter.gallery.cutout", "Cutout", 3,
            {{"levels", "Number of Levels", FieldKind::Number, 2.0f, 8.0f, 4.0f, "%.0f", false, ""},
                {"edgeSimplicity", "Edge Simplicity", FieldKind::Number, 0.0f, 10.0f, 4.0f, "%.0f", false, ""},
                {"edgeFidelity", "Edge Fidelity", FieldKind::Number, 1.0f, 3.0f, 2.0f, "%.0f", false, ""}}},
        {"filter.gallery.graphicPen", "Graphic Pen", 4,
            {{"strokeLength", "Stroke Length", FieldKind::Number, 1.0f, 15.0f, 15.0f, "%.0f", false, ""},
                {"balance", "Light/Dark Balance", FieldKind::Number, 0.0f, 100.0f, 50.0f, "%.0f", false, ""},
                {"direction", "Stroke Direction", FieldKind::Choice, 0, 0, 0.0f,
                    "Right Diagonal|Horizontal|Left Diagonal|Vertical", false,
                    "rightDiagonal|horizontal|leftDiagonal|vertical"},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"filter.gallery.stamp", "Stamp", 2,
            {{"balance", "Light/Dark Balance", FieldKind::Number, 0.0f, 50.0f, 25.0f, "%.0f", false, ""},
                {"smoothness", "Smoothness", FieldKind::Number, 1.0f, 50.0f, 5.0f, "%.0f", false, ""}}},
        {"filter.gallery.stainedGlass", "Stained Glass", 4,
            {{"cellSize", "Cell Size", FieldKind::Number, 2.0f, 50.0f, 10.0f, "%.0f", false, ""},
                {"borderThickness", "Border Thickness", FieldKind::Number, 1.0f, 20.0f, 4.0f, "%.0f", false, ""},
                {"lightIntensity", "Light Intensity", FieldKind::Number, 0.0f, 10.0f, 3.0f, "%.0f", false, ""},
                {"seed", "Randomize", FieldKind::Seed, 0, 0, 1.0f, "", false, ""}}},
        {"image.adjustments.shadowsHighlights", "Shadows/Highlights", 10,
            {{"shadowAmount", "Shadows: Amount", FieldKind::Number, 0.0f, 100.0f, 35.0f, "%.0f%%", false, ""},
                {"shadowTone", "Shadows: Tone", FieldKind::Number, 0.0f, 100.0f, 50.0f, "%.0f%%", false, ""},
                {"shadowRadius", "Shadows: Radius", FieldKind::Number, 0.0f, 2500.0f, 30.0f, "%.0f px", true, ""},
                {"highlightAmount", "Highlights: Amount", FieldKind::Number, 0.0f, 100.0f, 0.0f, "%.0f%%", false, ""},
                {"highlightTone", "Highlights: Tone", FieldKind::Number, 0.0f, 100.0f, 50.0f, "%.0f%%", false, ""},
                {"highlightRadius", "Highlights: Radius", FieldKind::Number, 0.0f, 2500.0f, 30.0f, "%.0f px", true, ""},
                {"color", "Color", FieldKind::Number, -100.0f, 100.0f, 20.0f, "%+.0f", false, ""},
                {"midtone", "Midtone", FieldKind::Number, -100.0f, 100.0f, 0.0f, "%+.0f", false, ""},
                {"blackClip", "Black Clip", FieldKind::Number, 0.0f, 50.0f, 0.01f, "%.3f%%", true, ""},
                {"whiteClip", "White Clip", FieldKind::Number, 0.0f, 50.0f, 0.01f, "%.3f%%", true, ""}}},
        {"image.adjustments.matchColor", "Match Color", 7,
            {{"source", "Source", FieldKind::Document, 0, 0, -1.0f, "", false, ""},
                {"luminance", "Luminance", FieldKind::Number, 1.0f, 200.0f, 100.0f, "%.0f", false, ""},
                {"intensity", "Color Intensity", FieldKind::Number, 1.0f, 200.0f, 100.0f, "%.0f", false, ""},
                {"fade", "Fade", FieldKind::Number, 0.0f, 100.0f, 0.0f, "%.0f", false, ""},
                {"neutralize", "Neutralize", FieldKind::Toggle, 0, 0, 0.0f, "", false, ""},
                {"useSelectionInSource", "Use Selection in Source to Calculate Colors", FieldKind::Toggle, 0, 0, 0.0f,
                    "", false, ""},
                {"useSelectionInTarget", "Use Selection in Target to Calculate Adjustment", FieldKind::Toggle, 0, 0,
                    0.0f, "", false, ""}}},
        {"image.adjustments.replaceColor", "Replace Color", 5,
            {{"color", "Color", FieldKind::Color, 0, 0, 0.0f, "", false, "#808080"},
                {"fuzziness", "Fuzziness", FieldKind::Number, 0.0f, 200.0f, 40.0f, "%.0f", false, ""},
                {"hue", "Hue", FieldKind::Number, -180.0f, 180.0f, 0.0f, "%+.0f", false, ""},
                {"saturation", "Saturation", FieldKind::Number, -100.0f, 100.0f, 0.0f, "%+.0f", false, ""},
                {"lightness", "Lightness", FieldKind::Number, -100.0f, 100.0f, 0.0f, "%+.0f", false, ""}}},
        {"image.adjustments.hdrToning", "HDR Toning", 9,
            {{"radius", "Edge Glow: Radius", FieldKind::Number, 1.0f, 500.0f, 30.0f, "%.0f px", true, ""},
                {"strength", "Edge Glow: Strength", FieldKind::Number, 0.1f, 4.0f, 0.5f, "%.2f", false, ""},
                {"gamma", "Gamma", FieldKind::Number, 0.1f, 2.0f, 1.0f, "%.2f", false, ""},
                {"exposure", "Exposure", FieldKind::Number, -5.0f, 5.0f, 0.0f, "%+.2f", false, ""},
                {"detail", "Detail", FieldKind::Number, -100.0f, 300.0f, 30.0f, "%+.0f%%", false, ""},
                {"shadow", "Shadow", FieldKind::Number, -100.0f, 100.0f, 0.0f, "%+.0f%%", false, ""},
                {"highlight", "Highlight", FieldKind::Number, -100.0f, 100.0f, 0.0f, "%+.0f%%", false, ""},
                {"vibrance", "Vibrance", FieldKind::Number, -100.0f, 100.0f, 0.0f, "%+.0f%%", false, ""},
                {"saturation", "Saturation", FieldKind::Number, -100.0f, 100.0f, 20.0f, "%+.0f%%", false, ""}}},
        {"image.mode.indexedColor", "Indexed Color", 6,
            {{"palette", "Palette", FieldKind::Choice, 0, 0, 0.0f,
                 "Local (Selective)|Local (Perceptual)|Local (Adaptive)|Exact|System (Mac OS)|System "
                 "(Windows)|Web|Uniform",
                 false, "selective|perceptual|adaptive|exact|systemMac|systemWindows|web|uniform"},
                {"colors", "Colors", FieldKind::Number, 2.0f, 256.0f, 256.0f, "%.0f", false, ""},
                {"forced", "Forced", FieldKind::Choice, 0, 0, 1.0f, "None|Black and White|Primaries|Web", false,
                    "none|blackWhite|primaries|web"},
                {"transparency", "Transparency", FieldKind::Toggle, 0, 0, 1.0f, "", false, ""},
                {"dither", "Dither", FieldKind::Choice, 0, 0, 1.0f, "None|Diffusion|Pattern|Noise", false,
                    "none|diffusion|pattern|noise"},
                {"amount", "Amount", FieldKind::Number, 0.0f, 100.0f, 75.0f, "%.0f%%", false, ""}}},
        {"image.mode.bitmap", "Bitmap", 4,
            {{"method", "Method", FieldKind::Choice, 0, 0, 2.0f,
                 "50% Threshold|Pattern Dither|Diffusion Dither|Halftone Screen", false,
                 "threshold|pattern|diffusion|halftone"},
                {"frequency", "Halftone Frequency (lines/inch)", FieldKind::Number, 1.0f, 999.0f, 53.0f, "%.0f", true,
                    ""},
                {"angle", "Halftone Angle", FieldKind::Number, -180.0f, 180.0f, 45.0f, "%.0f°", false, ""},
                {"shape", "Halftone Shape", FieldKind::Choice, 0, 0, 0.0f, "Round|Ellipse|Line|Square|Diamond|Cross",
                    false, "round|ellipse|line|square|diamond|cross"}}},
        {"image.mode.duotone", "Duotone Options", 5,
            {{"type", "Type", FieldKind::Choice, 0, 0, 1.0f, "Monotone|Duotone|Tritone|Quadtone", false,
                 "monotone|duotone|tritone|quadtone"},
                {"ink1", "Ink 1", FieldKind::Color, 0, 0, 0.0f, "", false, "#000000"},
                {"ink2", "Ink 2", FieldKind::Color, 0, 0, 0.0f, "", false, "#9e612e"},
                {"ink3", "Ink 3", FieldKind::Color, 0, 0, 0.0f, "", false, "#dbad2e"},
                {"ink4", "Ink 4", FieldKind::Color, 0, 0, 0.0f, "", false, "#456194"}}},
        {"layer.layerStyle.scaleEffects", "Scale Layer Effects", 1,
            {{"scale", "Scale", FieldKind::Number, 1.0f, 1000.0f, 100.0f, "%.0f%%", true, ""}}},
        {"image.imageRotation.arbitrary", "Rotate Canvas", 1,
            {{"angle", "Angle (° clockwise)", FieldKind::Number, -359.99f, 359.99f, 0.0f, "%.2f°", false, ""}}},
    };
    const int FILTER_DIALOG_COUNT = sizeof FILTER_DIALOGS / sizeof FILTER_DIALOGS[0];

    /* How many value slots a field takes. */
    int field_slots (const FilterField& f)
    {
        return f.kind == FieldKind::Matrix ? 25 : f.kind == FieldKind::Color ? 3 : 1;
    }

    /* The `index`th '|'-separated part of `list`. */
    std::string list_part (const char* list, int index)
    {
        const char* p = list;
        for (int i = 0; i < index && p != NULL; i++)
        {
            p = strchr (p, '|');
            if (p != NULL)
            {
                p++;
            }
        }
        if (p == NULL)
        {
            return "";
        }
        const char* e = strchr (p, '|');
        return e != NULL ? std::string (p, (size_t) (e - p)) : std::string (p);
    }

    int list_count (const char* list)
    {
        int n = 1;
        for (const char* p = list; *p; p++)
        {
            n += *p == '|';
        }
        return n;
    }

    int filter_dialog_index (const char* id)
    {
        for (int i = 0; i < FILTER_DIALOG_COUNT; i++)
        {
            if (strcmp (FILTER_DIALOGS[i].id, id) == 0)
            {
                return i;
            }
        }
        return -1;
    }

    /* True when `id` has settings: the dialog opens (values kept from the last use). */
    bool App::open_filter_dialog (const char* id)
    {
        int i = filter_dialog_index (id);
        if (i < 0)
        {
            return false;
        }
        if (filter_dialog_ != i)
        {
            int slot = 0;
            for (int f = 0; f < FILTER_DIALOGS[i].count; f++)
            {
                const FilterField& ff = FILTER_DIALOGS[i].fields[f];
                if (ff.kind == FieldKind::Color)
                {
                    float c[4] = {1, 1, 1, 1};
                    engine::parse_hex (ff.ids, c);
                    memcpy (&filter_values_[slot], c, 3 * sizeof (float));
                }
                else if (ff.kind == FieldKind::Matrix)
                {
                    for (int k = 0; k < 25; k++)
                    {
                        filter_values_[slot + k] = k == 12 ? 1.0f : 0.0f;
                    }
                }
                else
                {
                    filter_values_[slot] = ff.value;
                }
                slot += field_slots (ff);
            }
        }
        filter_dialog_        = i;
        show_filter_          = true;
        filter_edit_index_    = -1;
        filter_mode_          = FILTER_RUN;
        filter_preview_dirty_ = true;
        return true;
    }

    /* The dialog for filter `id` opened with `p`'s settings. False when the filter has no dialog. */
    bool App::load_filter_dialog (const char* id, const engine::Value& p)
    {
        int i = filter_dialog_index (id);
        if (i < 0)
        {
            return false;
        }
        filter_dialog_ = -1;
        open_filter_dialog (id);
        const FilterDialog& fd = FILTER_DIALOGS[i];
        int slot               = 0;
        for (int f = 0; f < fd.count && p.is_object (); f++)
        {
            const FilterField& ff = fd.fields[f];
            float* v              = &filter_values_[slot];
            slot += field_slots (ff);
            if (!p.contains (ff.key))
            {
                continue;
            }
            const engine::Value& x = p[ff.key];
            if ((ff.kind == FieldKind::Number || ff.kind == FieldKind::Seed || ff.kind == FieldKind::Document)
                && x.is_number ())
            {
                v[0] = x.get<float> ();
            }
            else if (ff.kind == FieldKind::Toggle && x.is_boolean ())
            {
                v[0] = x.get<bool> () ? 1.0f : 0.0f;
            }
            else if (ff.kind == FieldKind::Choice && x.is_string ())
            {
                for (int c = 0; c < list_count (ff.ids); c++)
                {
                    if (list_part (ff.ids, c) == x.get<std::string> ())
                    {
                        v[0] = (float) c;
                    }
                }
            }
            else if (ff.kind == FieldKind::Color && x.is_string ())
            {
                float c[4] = {1, 1, 1, 1};
                if (engine::parse_hex (x.get<std::string> ().c_str (), c))
                {
                    memcpy (v, c, 3 * sizeof (float));
                }
            }
            else if (ff.kind == FieldKind::Matrix && x.is_array () && x.size () == 25)
            {
                for (int k = 0; k < 25; k++)
                {
                    v[k] = x[(size_t) k].is_number () ? x[(size_t) k].get<float> () : 0.0f;
                }
            }
        }
        filter_preview_dirty_ = true;
        return true;
    }

    bool App::has_filter_dialog (const char* id) const
    {
        return filter_dialog_index (id) >= 0;
    }

    /* The dialog loaded with a smart filter's settings; OK changes that filter. */
    bool App::edit_smart_filter (doc::LayerId layer, int index)
    {
        const engine::DocState* d = session_.active ();
        const doc::Layer* l       = d != NULL ? d->doc->layer (layer) : NULL;
        if (l == NULL || index < 0 || (size_t) index >= l->smart.filters.size ())
        {
            return false;
        }
        const doc::SmartFilter& sf = l->smart.filters[(size_t) index];
        engine::Value p            = engine::Value::parse (sf.params, nullptr, false);
        if (!load_filter_dialog (sf.id.c_str (), p))
        {
            set_error (sf.id.empty () ? "this smart filter isn't available here" : "this filter has no settings");
            return false;
        }
        filter_mode_       = FILTER_SMART_EDIT;
        filter_edit_layer_ = layer;
        filter_edit_index_ = index;
        return true;
    }

    engine::Value App::filter_params ()
    {
        const FilterDialog& fd = FILTER_DIALOGS[filter_dialog_];
        engine::Value p        = engine::Value::object ();
        int slot               = 0;
        for (int f = 0; f < fd.count; f++)
        {
            const FilterField& ff = fd.fields[f];
            const float* v        = &filter_values_[slot];
            if (ff.kind == FieldKind::Number)
            {
                p[ff.key] = v[0];
            }
            else if (ff.kind == FieldKind::Toggle)
            {
                p[ff.key] = v[0] > 0.5f;
            }
            else if (ff.kind == FieldKind::Choice)
            {
                p[ff.key] = list_part (ff.ids, (int) v[0]);
            }
            else if (ff.kind == FieldKind::Seed)
            {
                p[ff.key] = (int64_t) v[0];
            }
            else if (ff.kind == FieldKind::Document)
            {
                if (v[0] >= 0.0f)
                {
                    p[ff.key] = (int64_t) v[0];
                }
            }
            else if (ff.kind == FieldKind::Color)
            {
                char hex[8];
                snprintf (hex, sizeof hex, "#%02x%02x%02x", (int) lroundf (v[0] * 255), (int) lroundf (v[1] * 255),
                    (int) lroundf (v[2] * 255));
                p[ff.key] = hex;
            }
            else
            {
                engine::Value k = engine::Value::array ();
                for (int i = 0; i < 25; i++)
                {
                    k.push_back ((int) lroundf (v[i]));
                }
                p[ff.key] = k;
            }
            slot += field_slots (ff);
        }
        return p;
    }

    void App::filter_dialog ()
    {
        if (!show_filter_)
        {
            return;
        }
        const engine::DocState* d = session_.active ();
        if (d == NULL)
        {
            show_filter_ = false;
            clear_preview ();
            return;
        }
        const FilterDialog& fd = FILTER_DIALOGS[filter_dialog_];
        char title[96];
        snprintf (title, sizeof title, "%s###filter", fd.title);
        ImGui::OpenPopup ("###filter");
        if (!ImGui::BeginPopupModal (title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k      = ui_scale_;
        bool editing = false;
        int slot     = 0;
        for (int f = 0; f < fd.count; f++)
        {
            const FilterField& ff = fd.fields[f];
            float* v              = &filter_values_[slot];
            slot += field_slots (ff);
            bool changed = false;
            ImGui::PushID (f);
            if (ff.kind == FieldKind::Number)
            {
                ImGui::SetNextItemWidth (220 * k);
                ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp | (ff.log ? ImGuiSliderFlags_Logarithmic : 0);
                changed                = ImGui::SliderFloat (ff.label, v, ff.min, ff.max, ff.format, flags);
                editing                = editing || ImGui::IsItemActive ();
            }
            else if (ff.kind == FieldKind::Toggle)
            {
                bool on = v[0] > 0.5f;
                changed = ImGui::Checkbox (ff.label, &on);
                v[0]    = on ? 1.0f : 0.0f;
            }
            else if (ff.kind == FieldKind::Choice)
            {
                int cur = (int) v[0], n = list_count (ff.format);
                std::string shown = list_part (ff.format, cur);
                ImGui::SetNextItemWidth (220 * k);
                if (ImGui::BeginCombo (ff.label[0] ? ff.label : "##choice", shown.c_str ()))
                {
                    for (int i = 0; i < n; i++)
                    {
                        std::string item = list_part (ff.format, i);
                        if (ImGui::Selectable (item.c_str (), i == cur))
                        {
                            v[0]    = (float) i;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo ();
                }
            }
            else if (ff.kind == FieldKind::Seed)
            {
                if (ImGui::Button (ff.label))
                {
                    v[0]    = (float) (1 + (int) (ImGui::GetTime () * 1000.0) % 100000);
                    changed = true;
                }
            }
            else if (ff.kind == FieldKind::Document)
            {
                int cur                    = (int) v[0];
                const engine::DocState* sd = cur >= 0 ? session_.doc_at ((size_t) cur) : NULL;
                ImGui::SetNextItemWidth (220 * k);
                if (ImGui::BeginCombo (ff.label, sd != NULL ? sd->doc->name.c_str () : "None"))
                {
                    if (ImGui::Selectable ("None", sd == NULL))
                    {
                        v[0]    = -1.0f;
                        changed = true;
                    }
                    for (size_t i = 0; i < session_.doc_count (); i++)
                    {
                        ImGui::PushID ((int) i);
                        if (ImGui::Selectable (session_.doc_at (i)->doc->name.c_str (), (int) i == cur))
                        {
                            v[0]    = (float) i;
                            changed = true;
                        }
                        ImGui::PopID ();
                    }
                    ImGui::EndCombo ();
                }
            }
            else if (ff.kind == FieldKind::Color)
            {
                changed = ImGui::ColorEdit3 (ff.label, v, ImGuiColorEditFlags_NoInputs);
                editing = editing || ImGui::IsItemActive ();
            }
            else
            {
                /* Custom's grid, the middle weight in the centre. */
                for (int i = 0; i < 25; i++)
                {
                    if (i % 5 != 0)
                    {
                        ImGui::SameLine ();
                    }
                    int iv = (int) lroundf (v[i]);
                    ImGui::PushID (i);
                    ImGui::SetNextItemWidth (46 * k);
                    if (ImGui::InputInt ("##m", &iv, 0, 0))
                    {
                        v[i]    = (float) std::min (std::max (iv, -999), 999);
                        changed = true;
                    }
                    ImGui::PopID ();
                }
            }
            ImGui::PopID ();
            if (changed)
            {
                filter_preview_dirty_ = true;
            }
        }
        if (ImGui::Checkbox ("Preview", &filter_preview_))
        {
            filter_preview_dirty_ = true;
        }
        /* The preview follows once a slider is let go (large blurs take a moment). */
        if (filter_preview_dirty_ && !editing)
        {
            filter_preview_dirty_ = false;
            if (filter_preview_)
            {
                std::shared_ptr<doc::Document> copy = std::make_shared<doc::Document> (*d->doc);
                engine::Error err;
                engine::Value fp         = filter_params ();
                doc::LayerId active      = d->active_layer;
                bool hdr                 = strcmp (fd.id, "image.adjustments.hdrToning") == 0;
                doc::LayerId mode_active = d->active_layer;
                if (strncmp (fd.id, "layer.", 6) == 0)
                {
                    clear_preview ();
                }
                else if (strncmp (fd.id, "image.mode.", 11) == 0)
                {
                    if (engine::apply_mode_command (session_, copy.get (), &mode_active, fd.id, fp, &err))
                    {
                        preview_doc_ = copy;
                        preview_serial_++;
                    }
                    else
                    {
                        set_error (err.message);
                        clear_preview ();
                    }
                }
                else if (strcmp (fd.id, "image.imageRotation.arbitrary") == 0)
                {
                    clear_preview ();
                }
                else if (hdr ? engine::apply_hdr_toning (session_, copy.get (), fp, &active, &err)
                         : filter_mode_ == FILTER_SMART_EDIT
                             ? engine::set_smart_filter (copy.get (), filter_edit_layer_, filter_edit_index_, fp,
                                   session_.cmyk, session_.tools.foreground, session_.tools.background, &err)
                             : engine::prepare_filter_params (session_, fd.id, &fp, &err)
                                   && engine::apply_filter (copy.get (), d->active_layer, fd.id, fp, session_.cmyk,
                                       session_.tools.foreground, session_.tools.background, &err))
                {
                    preview_doc_ = copy;
                    preview_serial_++;
                }
                else
                {
                    set_error (err.message);
                    clear_preview ();
                }
            }
            else
            {
                clear_preview ();
            }
        }
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter)
               || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok || cancel)
        {
            show_filter_ = false;
            clear_preview ();
            ImGui::CloseCurrentPopup ();
            int mode = filter_mode_, edit_index = filter_edit_index_;
            filter_mode_       = FILTER_RUN;
            filter_edit_index_ = -1;
            if (ok && mode == FILTER_SMART_EDIT)
            {
                run_command ("layer.smartFilter.edit",
                    engine::Value{{"layer", filter_edit_layer_}, {"index", edit_index}, {"params", filter_params ()}});
            }
            else if (mode == FILTER_STEP_EDIT)
            {
                if (ok)
                {
                    run_command ("actions.setStep", engine::Value{{"set", act_ref_[0]}, {"action", act_ref_[1]},
                                                        {"step", act_ref_[2]}, {"params", filter_params ()}});
                }
            }
            else if (mode == FILTER_ACTION_RESUME)
            {
                /* Playback carries on after the step, with the settings chosen; Cancel stops it. */
                if (ok)
                {
                    run_command (fd.id, filter_params ());
                    play_action (act_ref_[0], act_ref_[1], act_ref_[2] + 1);
                }
                else
                {
                    status_ = "Action stopped";
                }
            }
            else if (ok)
            {
                run_command (fd.id, filter_params ());
            }
        }
        ImGui::EndPopup ();
    }

    /* A smart object's filters under it in the Layers panel: "Smart Filters" (the eye turns the
       stack off), then each filter, top first. Double-click edits the settings; right-click for
       blending options, order and deletion. */
    void App::smart_filter_rows (const doc::Layer& l, int depth)
    {
        float k = ui_scale_, rh = roundf (24.0f * k), eye_w = 30.0f * k;
        ImDrawList* dl = ImGui::GetWindowDrawList ();
        float th = ImGui::GetTextLineHeight (), g = 14 * k;
        int n = (int) l.smart.filters.size ();
        for (int r = -1; r < n; r++)
        {
            int index = r < 0 ? -1 : n - 1 - r;
            ImGui::PushID ((int) l.id * 128 + r + 7000);
            ImVec2 p                  = ImGui::GetCursorScreenPos ();
            float w                   = ImGui::GetContentRegionAvail ().x;
            const doc::SmartFilter* f = index >= 0 ? &l.smart.filters[(size_t) index] : NULL;
            bool on                   = f == NULL ? l.smart.filters_enabled : f->enabled;
            dl->AddLine (ImVec2 (p.x, p.y + rh - 1), ImVec2 (p.x + w, p.y + rh - 1), theme_.separator);
            if (ImGui::InvisibleButton ("##sfeye", ImVec2 (eye_w, rh)))
            {
                engine::Value params = {{"layer", l.id}, {"enabled", !on}};
                if (index >= 0)
                {
                    params["index"] = index;
                }
                run_command ("layer.smartFilter.enabled", params);
            }
            if (on)
            {
                draw_icon (
                    dl, &icons_, "eye", ImVec2 (p.x + (eye_w - g) * 0.5f, p.y + (rh - g) * 0.5f), g, theme_.icon);
            }
            dl->AddLine (ImVec2 (p.x + eye_w, p.y), ImVec2 (p.x + eye_w, p.y + rh), theme_.separator);
            float x = p.x + eye_w + (float) (depth * 16 + (f == NULL ? 40 : 56)) * k;
            ImGui::SetCursorScreenPos (ImVec2 (p.x + eye_w, p.y));
            ImGui::InvisibleButton ("##sfrow", ImVec2 (w - eye_w, rh));
            if (f != NULL && ImGui::IsItemHovered () && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                if (session_.active ()->active_layer != l.id)
                {
                    run_command ("layer.select", engine::Value{{"layer", l.id}});
                }
                edit_smart_filter (l.id, index);
            }
            if (f != NULL && ImGui::BeginPopupContextItem ("##sfctx"))
            {
                if (ImGui::MenuItem ("Edit Smart Filter…", NULL, false, filter_dialog_index (f->id.c_str ()) >= 0))
                {
                    edit_smart_filter (l.id, index);
                }
                if (ImGui::MenuItem ("Edit Smart Filter Blending Options…"))
                {
                    blend_edit_layer_   = l.id;
                    blend_edit_index_   = index;
                    blend_edit_mode_    = f->blend;
                    blend_edit_opacity_ = f->opacity * 100.0f;
                }
                if (ImGui::MenuItem (f->enabled ? "Disable Smart Filter" : "Enable Smart Filter"))
                {
                    run_command ("layer.smartFilter.enabled",
                        engine::Value{{"layer", l.id}, {"index", index}, {"enabled", !f->enabled}});
                }
                ImGui::Separator ();
                if (ImGui::MenuItem ("Move Up", NULL, false, index + 1 < n))
                {
                    run_command (
                        "layer.smartFilter.move", engine::Value{{"layer", l.id}, {"index", index}, {"to", index + 1}});
                }
                if (ImGui::MenuItem ("Move Down", NULL, false, index > 0))
                {
                    run_command (
                        "layer.smartFilter.move", engine::Value{{"layer", l.id}, {"index", index}, {"to", index - 1}});
                }
                ImGui::Separator ();
                if (ImGui::MenuItem ("Delete Smart Filter"))
                {
                    run_command ("layer.smartFilter.delete", engine::Value{{"layer", l.id}, {"index", index}});
                }
                ImGui::EndPopup ();
            }
            if (f == NULL && ImGui::BeginPopupContextItem ("##sfall"))
            {
                if (ImGui::MenuItem (l.smart.filters_enabled ? "Disable Smart Filters" : "Enable Smart Filters"))
                {
                    run_command ("layer.smartFilter.enabled",
                        engine::Value{{"layer", l.id}, {"enabled", !l.smart.filters_enabled}});
                }
                if (ImGui::MenuItem ("Clear Smart Filters"))
                {
                    run_command ("layer.smartFilter.clear", engine::Value{{"layer", l.id}});
                }
                ImGui::EndPopup ();
            }
            std::string label = f == NULL ? "Smart Filters" : f->name;
            if (f != NULL && (f->blend != color::BlendMode::Normal || f->opacity < 1.0f))
            {
                char extra[64];
                snprintf (extra, sizeof extra, "  (%s, %.0f%%)", color::blend_label (f->blend), f->opacity * 100.0f);
                label += extra;
            }
            bool live = f == NULL || !f->id.empty ();
            dl->AddText (ImVec2 (x, p.y + (rh - th) * 0.5f),
                on && l.visible && live ? theme_.text_dim : theme_.text_faint, label.c_str ());
            ImGui::SetCursorScreenPos (ImVec2 (p.x, p.y + rh));
            ImGui::Dummy (ImVec2 (w, 0));
            ImGui::PopID ();
        }
    }

    /* Blending Options for one smart filter: its mode and opacity over the filters below. */
    void App::smart_filter_blending_dialog ()
    {
        if (blend_edit_index_ < 0)
        {
            return;
        }
        ImGui::OpenPopup ("Blending Options###sfblend");
        if (!ImGui::BeginPopupModal ("Blending Options###sfblend", NULL, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        float k = ui_scale_;
        ImGui::SetNextItemWidth (200 * k);
        if (ImGui::BeginCombo ("Mode", color::blend_label (blend_edit_mode_)))
        {
            for (size_t i = 0; i < color::LAYER_MODE_COUNT; i++)
            {
                color::BlendMode m = color::LAYER_MODES[i];
                if (ImGui::Selectable (color::blend_label (m), m == blend_edit_mode_))
                {
                    blend_edit_mode_ = m;
                }
            }
            ImGui::EndCombo ();
        }
        ImGui::SetNextItemWidth (200 * k);
        ImGui::SliderFloat ("Opacity", &blend_edit_opacity_, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::Spacing ();
        bool ok = ImGui::Button ("OK", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Enter);
        ImGui::SameLine ();
        bool cancel = ImGui::Button ("Cancel", ImVec2 (90 * k, 0)) || ImGui::IsKeyPressed (ImGuiKey_Escape);
        if (ok)
        {
            run_command ("layer.smartFilter.blending",
                engine::Value{{"layer", blend_edit_layer_}, {"index", blend_edit_index_},
                    {"mode", color::blend_label (blend_edit_mode_)}, {"opacity", blend_edit_opacity_}});
        }
        if (ok || cancel)
        {
            blend_edit_index_ = -1;
            ImGui::CloseCurrentPopup ();
        }
        ImGui::EndPopup ();
    }

} // namespace ui
