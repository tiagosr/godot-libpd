/*
 * tjlistfind <target> <tolerance>  --  (tj externals series)
 *
 * Search a list of floats for the first occurrence of a value and output
 * its 1-INDEXED position; 0 when absent.
 *
 *   [tjlistfind 2.0]           search for 2.0 (exact match)
 *   [tjlistfind 0.1 0.001]     search within |value - target| <= 0.001
 *
 * Inputs (left inlet):
 *   - list of atoms: scanned left to right; the first float atom within
 *     tolerance wins, its position (1, 2, 3, ...) is output as a float.
 *     Non-float atoms are skipped. No match (or empty list) -> 0.
 *   - single float: treated as a one-element list (1 if within tolerance,
 *     else 0).
 *   - "find f": change the target value (no output).
 *   - "tolerance f": change the absolute tolerance, f >= 0 (no output).
 *
 * Output (right-most, only outlet): float = 1-indexed position, or 0.
 *
 * MIT license (see repo root).
 */

#include "m_pd.h"

typedef struct _tjlistfind t_tjlistfind;

struct _tjlistfind
{
	t_object x_obj;		/* must be first */
	t_outlet *x_outlet;	/* index outlet */
	t_float x_target;		/* value being searched for */
	t_float x_tol;		/* absolute tolerance (0 = exact) */
};

static t_class *tjlistfind_class;

static t_float tjlistfind_hit(const t_tjlistfind *x, t_float v)
{
	t_float d = v - x->x_target;
	if (d < 0)
		d = -d;
	return (d <= x->x_tol);
}

/* left inlet: pure list (a message like [1.5 2 3] is a pure list;
 * the worker's MESSAGE path sends float-first lists via libpd_list). */
void tjlistfind_list(t_tjlistfind *x, t_symbol *s, int argc, t_atom *argv)
{
	t_int i;
	t_float idx = 0;
	for (i = 0; i < argc; i++) {
		if (argv[i].a_type == A_FLOAT) {
			if (tjlistfind_hit(x, atom_getfloat(&argv[i]))) {
				idx = (t_float)(i + 1);
				break;
			}
		}
	}
	outlet_float(x->x_outlet, idx);
}

void tjlistfind_float(t_tjlistfind *x, t_floatarg f)
{
	/* single float = one-element list */
	outlet_float(x->x_outlet, tjlistfind_hit(x, f) ? 1 : 0);
}

void tjlistfind_find(t_tjlistfind *x, t_floatarg f)
{
	x->x_target = f;
}

void tjlistfind_tolerance(t_tjlistfind *x, t_floatarg f)
{
	if (f < 0) {
		post("tjlistfind: tolerance must be >= 0");
		return;
	}
	x->x_tol = f;
}

static void *tjlistfind_new(t_floatarg target, t_floatarg tol)
{
	t_tjlistfind *x = (t_tjlistfind *)pd_new(tjlistfind_class);
	x->x_target = target;
	x->x_tol = (tol > 0 ? tol : 0);
	x->x_outlet = outlet_new(&x->x_obj, &s_float);
	return x;
}

void tjlistfind_setup(void)
{
	tjlistfind_class = class_new(gensym("tjlistfind"), (t_newmethod)tjlistfind_new,
			0, sizeof(t_tjlistfind), CLASS_DEFAULT, A_DEFFLOAT, A_DEFFLOAT, 0);
	class_addlist(tjlistfind_class, tjlistfind_list);
	class_addfloat(tjlistfind_class, &tjlistfind_float);
	class_addmethod(tjlistfind_class, (t_method)tjlistfind_find, gensym("find"), A_FLOAT, 0);
	class_addmethod(tjlistfind_class, (t_method)tjlistfind_tolerance, gensym("tolerance"), A_FLOAT, 0);
}
