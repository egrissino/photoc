#include "psd/fill.h"

#include "psd/descriptor.h"
#include "psd/effects.h"
#include <math.h>
#include <string.h>

namespace psd
{

    /* GdFl, a gradient fill layer: version 16, then a descriptor with the gradient (Grad: colour
       stops Clrs and opacity stops Trns, locations 0..4096), its style (Type), angle, scale,
       Reverse, Dither and Align with layer. */

    const char* const GRAD_TYPES[5] = {"Lnr ", "Rdl ", "Angl", "Rflc", "Dmnd"};

    bool parse_gradient_fill (const uint8_t* data, size_t len, doc::FillLayer* out)
    {
        Reader r = reader (data, len);
        rd_u32 (&r);
        DValue d;
        if (!parse_descriptor (&r, &d))
        {
            return false;
        }
        const DValue* grad = d_get (d, "Grad");
        if (grad == NULL)
        {
            return false;
        }
        doc::FillLayer f   = doc::FillLayer::make ();
        f.kind             = doc::FillKind::Gradient;
        const DValue* clrs = d_get (*grad, "Clrs");
        if (clrs == NULL || clrs->items.size () < 1)
        {
            return false;
        }
        f.stops.clear ();
        for (size_t i = 0; i < clrs->items.size (); i++)
        {
            const DValue& c = clrs->items[i];
            doc::GradientStop s;
            s.location = (float) fmin (fmax (d_num (c, "Lctn", 0) / 4096.0, 0.0), 1.0);
            color_from (d_get (c, "Clr "), s.color);
            f.stops.push_back (s);
        }
        for (size_t a = 1; a < f.stops.size (); a++)
        {
            for (size_t b = a; b > 0 && f.stops[b].location < f.stops[b - 1].location; b--)
            {
                std::swap (f.stops[b], f.stops[b - 1]);
            }
        }
        const DValue* trns = d_get (*grad, "Trns");
        bool opaque        = true;
        for (size_t i = 0; trns != NULL && i < trns->items.size (); i++)
        {
            const DValue& t    = trns->items[i];
            doc::CurvePoint cp = {(float) fmin (fmax (d_num (t, "Lctn", 0) / 4096.0, 0.0), 1.0),
                (float) fmin (fmax (d_num (t, "Opct", 100) / 100.0, 0.0), 1.0)};
            opaque             = opaque && cp.output >= 1.0f;
            f.opacity_stops.push_back (cp);
        }
        if (opaque)
        {
            f.opacity_stops.clear ();
        }
        std::string type = d_enum_of (d, "Type");
        for (int i = 0; i < 5; i++)
        {
            if (type == GRAD_TYPES[i])
            {
                f.style = (doc::GradientStyle) i;
            }
        }
        f.angle   = (float) d_num (d, "Angl", 90.0);
        f.scale   = (float) d_num (d, "Scl ", 100.0) / 100.0f;
        f.reverse = d_flag (d, "Rvrs", false);
        f.dither  = d_flag (d, "Dthr", false);
        f.align   = d_flag (d, "Algn", true);
        *out      = f;
        return true;
    }

    std::vector<uint8_t> write_gradient_fill (const doc::FillLayer& f)
    {
        DValue grad = d_object ("Grdn");
        d_set (&grad, "Nm  ", d_text ("Custom"));
        d_set (&grad, "GrdF", d_enum ("GrdF", "CstS"));
        d_set (&grad, "Intr", d_double (4096.0));
        DValue clrs = d_list ();
        for (size_t i = 0; i < f.stops.size (); i++)
        {
            DValue c = d_object ("Clrt");
            d_set (&c, "Clr ", color_value (f.stops[i].color));
            d_set (&c, "Type", d_enum ("Clry", "UsrS"));
            d_set (&c, "Lctn", d_long ((int32_t) lroundf (f.stops[i].location * 4096.0f)));
            d_set (&c, "Mdpn", d_long (50));
            clrs.items.push_back (c);
        }
        d_set (&grad, "Clrs", clrs);
        DValue trns = d_list ();
        if (f.opacity_stops.empty ())
        {
            for (int i = 0; i < 2; i++)
            {
                DValue t = d_object ("TrnS");
                d_set (&t, "Opct", d_unit ("#Prc", 100.0));
                d_set (&t, "Lctn", d_long (i * 4096));
                d_set (&t, "Mdpn", d_long (50));
                trns.items.push_back (t);
            }
        }
        for (size_t i = 0; i < f.opacity_stops.size (); i++)
        {
            DValue t = d_object ("TrnS");
            d_set (&t, "Opct", d_unit ("#Prc", f.opacity_stops[i].output * 100.0));
            d_set (&t, "Lctn", d_long ((int32_t) lroundf (f.opacity_stops[i].input * 4096.0f)));
            d_set (&t, "Mdpn", d_long (50));
            trns.items.push_back (t);
        }
        d_set (&grad, "Trns", trns);
        DValue d = d_object ("null");
        d_set (&d, "Grad", grad);
        d_set (&d, "Type", d_enum ("GrdT", GRAD_TYPES[(int) f.style]));
        d_set (&d, "Angl", d_unit ("#Ang", f.angle));
        d_set (&d, "Scl ", d_unit ("#Prc", f.scale * 100.0));
        d_set (&d, "Rvrs", d_bool (f.reverse));
        d_set (&d, "Dthr", d_bool (f.dither));
        d_set (&d, "Algn", d_bool (f.align));
        DValue ofst = d_object ("Pnt ");
        d_set (&ofst, "Hrzn", d_unit ("#Prc", 0.0));
        d_set (&ofst, "Vrtc", d_unit ("#Prc", 0.0));
        d_set (&d, "Ofst", ofst);
        std::vector<uint8_t> w;
        wr_u32 (&w, 16);
        write_descriptor (&w, d);
        return w;
    }

} // namespace psd
