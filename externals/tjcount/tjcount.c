/*
 * tjcount [min max] — up-only ranged counter with a wrap bang.
 *
 * Part of the tj externals (see README.md). Buildable both as a plain
 * Pd / Plugdata external (makefile next to this file) and, via the
 * godot-libpd project's CMake, into the embedded libpd build.
 *
 * Semantics
 * ---------
 *   state:   count (float), min, max.  count starts at min.
 *   bang  -> count + 1; if the result would exceed max, WRAP: count = min,
 *            a bang goes out the RIGHT outlet FIRST, then min out the
 *            LEFT outlet (standard pd right-outlet-bang convention).
 *   float -> count = clamped float, output on the left outlet.
 *   "set f"  -> count = clamped f, no output.
 *   "min f" / inlet 1 -> new minimum (accepted only while min < max,
 *            else the old value is kept and an error is posted);
 *            the current count is clamped into the new range (no output).
 *   "max f" / inlet 2 -> new maximum (same guard).
 *   creation args [min max] are optional (default 0 1); an invalid range
 *   falls back to 0..1 with a note. No loadbang, no output at creation.
 *
 * Outlets:  left   = float count
 *           right  = bang on wrap only
 * Inlets:   0 = bang | float (set+output) | "set f" | "min f" | "max f"
 *           1 = min (float)
 *           2 = max (float)
 *
 * The increment is a fixed +1 (same stepping as else/count's upward
 * mode). Ganged use: drive several [tjcount]s from one [metro] via
 * [send]/[receive]; the wrap bang is what you use to advance the next
 * stage in the chain (it fires exactly on max -> min, not early at
 * the boundary).
 */

#include "m_pd.h"

typedef struct _tjcount
{
    t_object  x_obj;
    t_inlet  *x_mininlet;    /* inlet 1: min   (created last) */
    t_inlet  *x_maxinlet;    /* inlet 2: max   (created first) */
    t_outlet *x_countoutlet; /* left:  float count */
    t_outlet *x_wrapoutlet;  /* right: bang on wrap */
    t_float   x_min;
    t_float   x_max;
    t_float   x_count;
} t_tjcount;

static t_class *tjcount_class;

static t_float tjcount_clamp(t_tjcount *x, t_float f)
{
    if (f < x->x_min)
        return (x->x_min);
    if (f > x->x_max)
        return (x->x_max);
    return (f);
}

static void tjcount_clamp_count(t_tjcount *x)
{
    x->x_count = tjcount_clamp(x, x->x_count);
}

static void tjcount_bang(t_tjcount *x)
{
    t_float next = x->x_count + 1.;
    if (next > x->x_max) {
        /* wrap: bang FIRST (right outlet), then the new count */
        x->x_count = x->x_min;
        outlet_bang(x->x_wrapoutlet);
        outlet_float(x->x_countoutlet, x->x_count);
    }
    else {
        x->x_count = next;
        outlet_float(x->x_countoutlet, x->x_count);
    }
}

static void tjcount_float(t_tjcount *x, t_float f)
{
    x->x_count = tjcount_clamp(x, f);
    outlet_float(x->x_countoutlet, x->x_count);
}

static void tjcount_set(t_tjcount *x, t_float f)
{
    x->x_count = tjcount_clamp(x, f);
}

static void tjcount_min(t_tjcount *x, t_float f)
{
    if (f >= x->x_max) {
        pd_error((t_pd *)x, "tjcount: min %g must be < max %g", f, x->x_max);
        return;
    }
    x->x_min = f;
    tjcount_clamp_count(x);
}

static void tjcount_max(t_tjcount *x, t_float f)
{
    if (f <= x->x_min) {
        pd_error((t_pd *)x, "tjcount: max %g must be > min %g", f, x->x_min);
        return;
    }
    x->x_max = f;
    tjcount_clamp_count(x);
}

static void *tjcount_new(t_floatarg fmin, t_floatarg fmax)
{
    t_tjcount *x = (t_tjcount *)pd_new(tjcount_class);
    if (!(fmin < fmax)) {
        post("tjcount: invalid range (min %g, max %g); using 0..1", fmin, fmax);
        fmin = 0;
        fmax = 1;
    }
    x->x_min = fmin;
    x->x_max = fmax;
    x->x_count = fmin;
    x->x_countoutlet = outlet_new(&x->x_obj, &s_float);            /* left: count */
    x->x_wrapoutlet = outlet_new(&x->x_obj, &s_bang); /* right: wrap bang */
    /* inlets: created right-to-left, so max (inlet 2) first */
    x->x_maxinlet = inlet_new(&x->x_obj, &x->x_obj.ob_pd,
            &s_float, gensym("float"));
    x->x_mininlet = inlet_new(&x->x_obj, &x->x_obj.ob_pd,
            &s_float, gensym("float"));
    return (x);
}

void tjcount_setup(void)
{
    tjcount_class = class_new(gensym("tjcount"), (t_newmethod)tjcount_new,
            0, sizeof(t_tjcount), CLASS_DEFAULT, A_DEFFLOAT, A_DEFFLOAT, 0);
    class_addbang(tjcount_class, tjcount_bang);
    class_addfloat(tjcount_class, tjcount_float);
    class_addmethod(tjcount_class, (t_method)tjcount_set, gensym("set"), A_FLOAT, 0);
    class_addmethod(tjcount_class, (t_method)tjcount_min, gensym("min"), A_FLOAT, 0);
    class_addmethod(tjcount_class, (t_method)tjcount_max, gensym("max"), A_FLOAT, 0);
}
